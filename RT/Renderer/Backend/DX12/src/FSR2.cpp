#include "FSR2.h"
#include "GlobalDX.h"
#include "Resource.h"
#ifdef D2_ENABLE_OPENVR
#include "OpenVRPrototype.h"
#endif

#include "ffx_api/ffx_api.hpp"
#include "ffx_api/ffx_upscale.hpp"
#include "ffx_api/dx12/ffx_api_dx12.hpp"

#include <algorithm>
#include <cmath>

using namespace RT;

static float FsrModeScalingFactors[FSR_MODE_NUM_MODES] = { 1.0f, 1.5f, 1.7f, 2.0f, 3.0f, 1.3f };

float FSR2::GetSharpnessForFSRMode()
{
	switch (g_d3d.amd_fsr2_mode)
	{
	case FSR_MODE_HIGH_QUALITY:      return 0.35f;
	case FSR_MODE_QUALITY:           return 0.45f;
	case FSR_MODE_BALANCED:          return 0.55f;
	case FSR_MODE_PERFORMANCE:       return 0.65f;
	case FSR_MODE_ULTRA_PERFORMANCE: return 0.75f;
	default:                         return 0.0f;
	}
}

#define FFX_CALL(ffx_code_) \
    do { \
		ffx::ReturnCode ffx_code = ffx_code_; \
		if (ffx_code != ffx::ReturnCode::Ok) \
		{ \
			RT_FATAL_ERROR(RT_ArenaPrintF(&g_thread_arena, "FSR 3.1 threw an error with code: %u", (uint32_t)ffx_code)); \
		} \
	} \
	while (false)

static void FSR3_MessageCallback(uint32_t type, const wchar_t* message)
{
	if (type == FFX_API_MESSAGE_TYPE_ERROR)
	{
		// TODO: Eh.. we cant use RT_FATAL_ERROR with wide chars yet
		OutputDebugStringW(message);
		__debugbreak();
		ExitProcess(1);
	}
	else if (type == FFX_API_MESSAGE_TYPE_WARNING)
	{
		OutputDebugStringW(message);
	}
}

struct Data
{
	ffx::Context context;
	ffx::CreateContextDescUpscale context_desc;
} static data;
static Data stereoData[2];
static bool stereoReady;

void FSR2::ExitStereo() {
    if(!stereoReady) return;
    for(auto& eye:stereoData) FFX_CALL(ffx::DestroyContext(eye.context));
    stereoReady=false;
}
void FSR2::InitStereo(uint32_t width,uint32_t height) {
    ExitStereo();
    for(auto& eye:stereoData) {
        eye.context_desc={};
        eye.context_desc.maxUpscaleSize={width,height};
        eye.context_desc.maxRenderSize={width,height};
        eye.context_desc.fpMessage=FSR3_MessageCallback;
        eye.context_desc.flags=FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE |
            FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE; // matches vr_fsr_depth.hlsl
        ffx::CreateBackendDX12Desc backend;backend.device=g_d3d.device;
        FFX_CALL(ffx::CreateContext(eye.context,nullptr,eye.context_desc,backend));
    }
    stereoReady=true;
}

void FSR2::Init()
{
	// FSR 3.1 context description. The FSR API owns its DX12 backend context.
	data.context_desc.maxUpscaleSize = { g_d3d.output_width, g_d3d.output_height };
	data.context_desc.maxRenderSize = { g_d3d.output_width, g_d3d.output_height };
	data.context_desc.fpMessage = FSR3_MessageCallback;
	data.context_desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE |
		FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE; // matches vr_fsr_depth.hlsl

	ffx::CreateBackendDX12Desc backend_desc;
	backend_desc.device = g_d3d.device;
	FFX_CALL(ffx::CreateContext(data.context, nullptr, data.context_desc, backend_desc));

}

void FSR2::Exit()
{
    ExitStereo();
	FFX_CALL(ffx::DestroyContext(data.context));
}

void FSR2::Dispatch(ID3D12CommandList* command_list, ID3D12Resource* rt_color, ID3D12Resource* rt_depth,
	ID3D12Resource* rt_motion_vector, ID3D12Resource* rt_reactive, ID3D12Resource* rt_output, uint32_t render_width, uint32_t render_height,
	float camera_jitter_x, float camera_jitter_y, float camera_near, float camera_far, float camera_vfov_angle, float delta_time,
	float sharpness, bool reset)
{
	ffx::DispatchDescUpscale dispatch_desc;
	dispatch_desc.commandList = command_list;
	dispatch_desc.color = ffxApiGetResourceDX12(rt_color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	dispatch_desc.depth = ffxApiGetResourceDX12(rt_depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	dispatch_desc.motionVectors = ffxApiGetResourceDX12(rt_motion_vector, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	dispatch_desc.reactive = ffxApiGetResourceDX12(rt_reactive, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	dispatch_desc.exposure = ffxApiGetResourceDX12(nullptr);
	dispatch_desc.transparencyAndComposition = ffxApiGetResourceDX12(nullptr);
	dispatch_desc.output = ffxApiGetResourceDX12(rt_output, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	// Jitter offsets can be calculated using ffxFsr2GetJitterPhaseCount and ffxFsr2GetJitterOffset
	// However we can also supply our own, which we already have implemented, so we will use our own halton sequence
	dispatch_desc.jitterOffset.x = camera_jitter_x;
	dispatch_desc.jitterOffset.y = camera_jitter_y;
	dispatch_desc.motionVectorScale.x = (float)render_width;
	dispatch_desc.motionVectorScale.y = (float)render_height;
	// Used to reset the temporal accumulation in case of jump cuts
	dispatch_desc.reset = reset;
	dispatch_desc.enableSharpening = sharpness > 0.0f;
	dispatch_desc.sharpness = std::clamp(sharpness, 0.0f, 1.0f);
	dispatch_desc.frameTimeDelta = delta_time;
	dispatch_desc.preExposure = 1.0;
	dispatch_desc.renderSize.width = render_width;
	dispatch_desc.renderSize.height = render_height;
	dispatch_desc.upscaleSize = { g_d3d.output_width, g_d3d.output_height };
	dispatch_desc.cameraFar = camera_far;
	dispatch_desc.cameraNear = camera_near;
	dispatch_desc.cameraFovAngleVertical = camera_vfov_angle;
	dispatch_desc.viewSpaceToMetersFactor = 1.0f;
	dispatch_desc.flags = 0;

	#ifdef D2_ENABLE_OPENVR
    if(OpenVRPrototype::EyePass()) {
        RT_ASSERT(stereoReady);
        // Ray generation applies Y jitter in upward UV coordinates, whereas
        // FSR's pixel-space jitter uses downward screen coordinates.
        dispatch_desc.jitterOffset.y=-camera_jitter_y;
        float left,right,top,bottom;OpenVRPrototype::ProjectionTangents(&left,&right,&top,&bottom);
        dispatch_desc.cameraFovAngleVertical=std::fabs(std::atan(top)-std::atan(bottom));
        const float vrSharpness=OpenVRPrototype::FsrSharpness(); // D2_VR_FSR_SHARPNESS overrides the preset
        dispatch_desc.enableSharpening=vrSharpness>0.0f;dispatch_desc.sharpness=std::clamp(vrSharpness,0.0f,1.0f);
        const auto output=rt_output->GetDesc();
        dispatch_desc.upscaleSize={static_cast<uint32_t>(output.Width),output.Height};
        // The contexts retain independent temporal images for the two eyes.
        FFX_CALL(ffx::Dispatch(stereoData[OpenVRPrototype::FirstEye() ? 0 : 1].context,dispatch_desc));
    } else
#endif
    FFX_CALL(ffx::Dispatch(data.context, dispatch_desc));
}

void FSR2::AdjustRenderResolutionForFSRMode(uint32_t output_width, uint32_t output_height, uint32_t& render_width, uint32_t& render_height)
{
	switch (g_d3d.amd_fsr2_mode)
	{
	case FSR_MODE_NO_UPSCALING:
	{
		render_width = output_width;
		render_height = output_height;
		break;
	}
	case FSR_MODE_QUALITY:
	{
		render_width = (uint32_t)((float)output_width / FsrModeScalingFactors[FSR_MODE_QUALITY]);
		render_height = (uint32_t)((float)output_height / FsrModeScalingFactors[FSR_MODE_QUALITY]);
		break;
	}
	case FSR_MODE_BALANCED:
	{
		render_width = (uint32_t)((float)output_width / FsrModeScalingFactors[FSR_MODE_BALANCED]);
		render_height = (uint32_t)((float)output_height / FsrModeScalingFactors[FSR_MODE_BALANCED]);
		break;
	}
	case FSR_MODE_PERFORMANCE:
	{
		render_width = (uint32_t)((float)output_width / FsrModeScalingFactors[FSR_MODE_PERFORMANCE]);
		render_height = (uint32_t)((float)output_height / FsrModeScalingFactors[FSR_MODE_PERFORMANCE]);
		break;
	}
	case FSR_MODE_ULTRA_PERFORMANCE:
	{
		render_width = (uint32_t)((float)output_width / FsrModeScalingFactors[FSR_MODE_ULTRA_PERFORMANCE]);
		render_height = (uint32_t)((float)output_height / FsrModeScalingFactors[FSR_MODE_ULTRA_PERFORMANCE]);
		break;
	}
	case FSR_MODE_HIGH_QUALITY:
	{
		render_width = (uint32_t)((float)output_width / FsrModeScalingFactors[FSR_MODE_HIGH_QUALITY]);
		render_height = (uint32_t)((float)output_height / FsrModeScalingFactors[FSR_MODE_HIGH_QUALITY]);
		break;
	}
	}

	render_width = std::max(render_width, 1u);
	render_height = std::max(render_height, 1u);
}

void FSR2::RescaleResolutionForFSRMode(uint32_t& width, uint32_t& height)
{
	width = (uint32_t)((float)width / FsrModeScalingFactors[g_d3d.amd_fsr2_mode]);
	height = (uint32_t)((float)height / FsrModeScalingFactors[g_d3d.amd_fsr2_mode]);
}

RT_Vec2 FSR2::GetMipBiasForFSRMode(uint32_t output_width, uint32_t output_height, uint32_t render_width, uint32_t render_height)
{
	return RT_Vec2Make(std::log2f((float)render_width / (float)output_width) - 1.0f, std::log2f((float)render_height / (float)output_height) - 1.0f);
}

void FSR2::OnWindowResize(uint32_t width, uint32_t height)
{
	width = std::max(width, 2u);
	height = std::max(height, 2u);

	FFX_CALL(ffx::DestroyContext(data.context));

	data.context_desc.maxUpscaleSize = { width, height };
	data.context_desc.maxRenderSize = { width, height };

	ffx::CreateBackendDX12Desc backend_desc;
	backend_desc.device = g_d3d.device;
	FFX_CALL(ffx::CreateContext(data.context, nullptr, data.context_desc, backend_desc));
}

#include <vector>
#include <utility>
#include "VRMirrorLayout.h"
#include <cwchar>
#include "RenderBackend.h"
#include "GlobalDX.h"
#include "TextureOpacity.h"
#include "CommandQueue.h"
#include "Commandlist.h"
#include "GPUProfiler.h"
#include "Resource.h"
#include "ImageReadWrite.h"
#include "FSR2.h"
#ifdef D2_ENABLE_OPENVR
#include "OpenVRPrototype.h"
#endif

#include "Core/MiniMath.h"
#include "Core/MemoryScope.hpp"

// ------------------------------------------------------------------
// Dear ImGui backend

#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx12.h"

// ------------------------------------------------------------------

#include <stdio.h>
#include <DirectXMath.h>
using namespace DirectX;

#define RT_RENDER_SETTINGS_CONFIG_FILE "render_settings.vars"
#define RT_DEFAULT_RENDER_SETTINGS_CONFIG_FILE "assets/render_presets/default.vars"

// ------------------------------------------------------------------
// These global arrays are directly accessible through calls in
// Renderer.h and mirrored to the GPU per-frame (see Renderer.h for 
// details):

RT_MaterialEdge g_rt_material_edges  [RT_MAX_MATERIAL_EDGES];
uint16_t        g_rt_material_indices[RT_MAX_MATERIALS];

// It's something like 205 KiB to copy per frame. For now I figure 
// that that's peanuts and just copy everything every time. 

// To be precise, it's being copied twice: Once from these purely
// CPU-side global arrays into the frame's upload buffer, and then
// from the frame's upload buffer to the actual GPU-side buffer.
// This seems to me like the simplest scheme, because it means that
// the address of the arrays for the game code to write into doesn't
// change every frame like it would if we exposed the mapped upload
// buffers instead.

// - Daniel 28/02/2023
// ------------------------------------------------------------------

RT::D3D12State RT::g_d3d;
RT::D3D12RasterState RT::g_d3d_raster;
RT::TweakVars RT::tweak_vars;

using namespace RT;

SlotMap<MeshResource> RT::g_mesh_slotmap(MAX_BOTTOM_LEVELS);
SlotMap<TextureResource> RT::g_texture_slotmap(RT_MAX_TEXTURES);

// Released textures/meshes may still be read by frames in flight. They are
// queued here, stamped with the fence of the frame that released them, and
// freed (GPU memory and bindless descriptor) once that fence completes.
// Previously they were tracked against a null command list and never freed,
// so every level change and texture unload leaked video memory.
namespace {
struct DeferredRelease { ID3D12Object* objects[2]; DescriptorAllocation descriptor; bool has_descriptor; uint64_t fence; };
std::vector<DeferredRelease> g_deferred_releases;
void QueueDeferredRelease(ID3D12Object* a, ID3D12Object* b, const DescriptorAllocation* descriptor)
{
	DeferredRelease entry = {};
	entry.objects[0] = a; entry.objects[1] = b;
	if (descriptor) { entry.descriptor = *descriptor; entry.has_descriptor = true; }
	g_deferred_releases.push_back(entry);
}
void StampDeferredReleases(uint64_t fence)
{
	for (auto& entry : g_deferred_releases) if (!entry.fence) entry.fence = fence;
}
// Cockpit camera monitors (rear view, guide-bot, missile, co-op player view).
// Each of the two views has its own scene data per swap slot, so a view never
// waits for another view's GPU work, and its own set of blit render targets and
// history counter, so its denoiser/TAA history is not the other monitor's image.
constexpr int kCameraViews = 2;
FrameData g_camera_frames[BACK_BUFFER_COUNT_SWAP][kCameraViews];
FrameData* g_active_camera_frame;
int g_active_camera_view = -1;
uint64_t g_camera_history[kCameraViews];
uint64_t g_camera_last_frame[kCameraViews]; // frame_index + 1 of the last render, 0 = never
bool g_camera_history_reset;
// Half-rate monitor updates (VR): the view is not re-rendered this frame and its
// last image is blitted again. Saved render_blit flag for that blit.
bool g_camera_reusing;
bool g_camera_saved_render_blit;
// View 1's blit targets; swapped into g_d3d.render_targets_blit while it renders.
ID3D12Resource* g_camera_blit_targets_b[RenderTarget_COUNT];

// Shield bubbles / first-person sweep for the post-process pass (b2).
RT_ShieldEffects g_shield_effects = { 0, 0.0f, -1.0f }; // no bubbles, no sweep
static_assert(sizeof(RT_ShieldEffects) == 32 + 96 * RT_MAX_SHIELD_BUBBLES,
	"RT_ShieldEffects must match ShieldEffects in post_process.hlsl");

// Fence of the most recent RaytraceRender that wrote the shared per-frame
// scene data (constants, instances, lights). Cockpit camera views reuse that
// data, so a camera must wait for the previous camera, but not for VR eyes,
// which render from their own per-eye copies.
uint64_t g_shared_scene_data_fence;

void ProcessDeferredReleases(uint64_t completed_fence, bool release_all)
{
	size_t kept = 0;
	for (size_t i = 0; i < g_deferred_releases.size(); ++i) {
		DeferredRelease& entry = g_deferred_releases[i];
		if (release_all || (entry.fence && entry.fence <= completed_fence)) {
			for (ID3D12Object* object : entry.objects) if (object) g_d3d.resource_tracker.Release(object);
			if (entry.has_descriptor) g_d3d.cbv_srv_uav.Free(entry.descriptor);
		} else {
			g_deferred_releases[kept++] = entry;
		}
	}
	g_deferred_releases.resize(kept);
}
}

D3D12_RESOURCE_BARRIER RT::GetUAVBarrier(ID3D12Resource* resource)
{
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	barrier.UAV.pResource = resource;

	return barrier;
}

void RT::UAVBarrier(ID3D12GraphicsCommandList *command_list, ID3D12Resource *resource)
{
	D3D12_RESOURCE_BARRIER uav_barrier = GetUAVBarrier(resource);
	command_list->ResourceBarrier(1, &uav_barrier);
}

void RT::UAVBarriers(ID3D12GraphicsCommandList *command_list, size_t count, ID3D12Resource **resources)
{
	MemoryScope scope;

	D3D12_RESOURCE_BARRIER *barriers = RT_ArenaAllocArray(scope, count, D3D12_RESOURCE_BARRIER);
	for (size_t i = 0; i < count; i++)
	{
		barriers[i] = GetUAVBarrier(resources[i]);
	}

	command_list->ResourceBarrier((UINT)count, barriers);
}

D3D12_RESOURCE_BARRIER RT::GetTransitionBarrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before_state, D3D12_RESOURCE_STATES after_state)
{
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = resource;
	barrier.Transition.StateBefore = before_state;
	barrier.Transition.StateAfter = after_state;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

	return barrier;
}

D3D12_RESOURCE_STATES RT::ResourceTransition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource, D3D12_RESOURCE_STATES dst_state)
{
	return g_d3d.resource_tracker.Transition(list, resource, dst_state); // yes, this just punts to the resource tracker.
	// I figured it's nicer however to just have a function call.
}

void RT::ResourceTransitions(ID3D12GraphicsCommandList* list, size_t num_resources, ID3D12Resource** resources, D3D12_RESOURCE_STATES dst_state)
{
	g_d3d.resource_tracker.Transitions(list, num_resources, resources, dst_state);
}

void RT::ResourceTransitions(ID3D12GraphicsCommandList* list, size_t num_resources, ID3D12Resource** resources, D3D12_RESOURCE_STATES* dst_states)
{
	g_d3d.resource_tracker.Transitions(list, num_resources, resources, dst_states);
}

namespace
{
    bool IndirectReferenceGroups()
    {
        static const bool reference = [] {
            wchar_t setting[8] = {};
            return GetEnvironmentVariableW(L"D2_VR_INDIRECT_REFERENCE", setting, RT_ARRAY_COUNT(setting)) > 0 &&
                   setting[0] == L'1';
        }();
        return reference;
    }
    bool DirectReferenceGroups()
    {
        static const bool reference = [] {
            wchar_t setting[8] = {};
            return GetEnvironmentVariableW(L"D2_VR_DIRECT_REFERENCE", setting, RT_ARRAY_COUNT(setting)) > 0 &&
                   setting[0] == L'1';
        }();
        return reference;
    }
	// yoinked from DDSc.h
    inline size_t DDSBitsPerPixel(_In_ DXGI_FORMAT fmt)
    {
        switch (fmt)
        {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT:
            return 128;

        case DXGI_FORMAT_R32G32B32_TYPELESS:
        case DXGI_FORMAT_R32G32B32_FLOAT:
        case DXGI_FORMAT_R32G32B32_UINT:
        case DXGI_FORMAT_R32G32B32_SINT:
            return 96;

        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_R32G32_TYPELESS:
        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R32G32_UINT:
        case DXGI_FORMAT_R32G32_SINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        case DXGI_FORMAT_Y416:
        case DXGI_FORMAT_Y210:
        case DXGI_FORMAT_Y216:
            return 64;

        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UINT:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SINT:
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16G16_SNORM:
        case DXGI_FORMAT_R16G16_SINT:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R32_SINT:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
        case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
        case DXGI_FORMAT_R8G8_B8G8_UNORM:
        case DXGI_FORMAT_G8R8_G8B8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_AYUV:
        case DXGI_FORMAT_Y410:
        case DXGI_FORMAT_YUY2:
#if (defined(_XBOX_ONE) && defined(_TITLE)) || defined(_GAMING_XBOX)
        case DXGI_FORMAT_R10G10B10_7E3_A2_FLOAT:
        case DXGI_FORMAT_R10G10B10_6E4_A2_FLOAT:
        case DXGI_FORMAT_R10G10B10_SNORM_A2_UNORM:
#endif
            return 32;

        case DXGI_FORMAT_P010:
        case DXGI_FORMAT_P016:
#if (_WIN32_WINNT >= _WIN32_WINNT_WIN10)
        case DXGI_FORMAT_V408:
#endif
#if (defined(_XBOX_ONE) && defined(_TITLE)) || defined(_GAMING_XBOX)
        case DXGI_FORMAT_D16_UNORM_S8_UINT:
        case DXGI_FORMAT_R16_UNORM_X8_TYPELESS:
        case DXGI_FORMAT_X16_TYPELESS_G8_UINT:
#endif
            return 24;

        case DXGI_FORMAT_R8G8_TYPELESS:
        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R8G8_UINT:
        case DXGI_FORMAT_R8G8_SNORM:
        case DXGI_FORMAT_R8G8_SINT:
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R16_SNORM:
        case DXGI_FORMAT_R16_SINT:
        case DXGI_FORMAT_B5G6R5_UNORM:
        case DXGI_FORMAT_B5G5R5A1_UNORM:
        case DXGI_FORMAT_A8P8:
        case DXGI_FORMAT_B4G4R4A4_UNORM:
#if (_WIN32_WINNT >= _WIN32_WINNT_WIN10)
        case DXGI_FORMAT_P208:
        case DXGI_FORMAT_V208:
#endif
            return 16;

        case DXGI_FORMAT_NV12:
        case DXGI_FORMAT_420_OPAQUE:
        case DXGI_FORMAT_NV11:
            return 12;

        case DXGI_FORMAT_R8_TYPELESS:
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_R8_SNORM:
        case DXGI_FORMAT_R8_SINT:
        case DXGI_FORMAT_A8_UNORM:
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
        case DXGI_FORMAT_AI44:
        case DXGI_FORMAT_IA44:
        case DXGI_FORMAT_P8:
#if (defined(_XBOX_ONE) && defined(_TITLE)) || defined(_GAMING_XBOX)
        case DXGI_FORMAT_R4G4_UNORM:
#endif
            return 8;

        case DXGI_FORMAT_R1_UNORM:
            return 1;

        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
            return 4;

        case DXGI_FORMAT_UNKNOWN:
        case DXGI_FORMAT_FORCE_UINT:
        default:
            return 0;
        }
    }

	struct ShaderDefineSettings
	{
		uint64_t last_modified_time;

		bool early_out_of_bounds = true;
	};

	ShaderDefineSettings g_shader_defines;

	void WriteTweakvarsToIOConfig();
	void UpdateTweakvarsFromIOConfig();
	void ResetTweakvarsToDefault();

	void InitTweakVars()
	{
		g_d3d.io.config = RT_ArenaAllocStructNoZero(g_d3d.arena, RT_Config);
		RT_InitializeConfig(g_d3d.io.config, g_d3d.arena);

		ResetTweakvarsToDefault();
		WriteTweakvarsToIOConfig();

		RT_InitializeConfig(&g_d3d.global_shader_defines, g_d3d.arena);
		RT_ConfigWriteString(&g_d3d.global_shader_defines, RT_StringLiteral("DO_EARLY_OUT_IN_COMPUTE_SHADERS"), RT_StringLiteral("1"));

		if (RT_DeserializeConfigFromFile(g_d3d.io.config, RT_RENDER_SETTINGS_CONFIG_FILE))
		{
			UpdateTweakvarsFromIOConfig();
		}
	}

	void ResetTweakvarsToDefault()
	{
		#define TWEAK_CATEGORY_BEGIN(name)
		#define TWEAK_CATEGORY_END()
		#define TWEAK_BOOL(name, var, value) tweak_vars.var = value;
		#define TWEAK_INT(name, var, value, min, max) tweak_vars.var = value;
		#define TWEAK_FLOAT(name, var, value, min, max) tweak_vars.var = (float)value;
		#define TWEAK_COLOR(name, var, value) tweak_vars.var.xyz = RT_PASTE(RT_Vec3Make, value);
		#define TWEAK_OPTIONS(name, var, value, ...) tweak_vars.var = value;

		#include "shared_tweakvars.hlsl.h"

		#undef TWEAK_CATEGORY_BEGIN
		#undef TWEAK_CATEGORY_END
		#undef TWEAK_BOOL
		#undef TWEAK_INT
		#undef TWEAK_FLOAT
		#undef TWEAK_COLOR
		#undef TWEAK_OPTIONS
	}

	void WriteTweakvarsToIOConfig()
	{
		RT_Config *cfg = g_d3d.io.config;

		#define TWEAK_CATEGORY_BEGIN(name)
		#define TWEAK_CATEGORY_END()
		#define TWEAK_BOOL(name, var, value) RT_ConfigWriteInt(cfg, RT_StringLiteral(#var), tweak_vars.var);
		#define TWEAK_INT(name, var, value, min, max) RT_ConfigWriteInt(cfg, RT_StringLiteral(#var), tweak_vars.var);
		#define TWEAK_FLOAT(name, var, value, min, max) RT_ConfigWriteFloat(cfg, RT_StringLiteral(#var), tweak_vars.var);
		#define TWEAK_COLOR(name, var, value) RT_ConfigWriteVec3(cfg, RT_StringLiteral(#var), tweak_vars.var.xyz);
		#define TWEAK_OPTIONS(name, var, value, ...) RT_ConfigWriteInt(cfg, RT_StringLiteral(#var), tweak_vars.var);

		#include "shared_tweakvars.hlsl.h"

		#undef TWEAK_CATEGORY_BEGIN
		#undef TWEAK_CATEGORY_END
		#undef TWEAK_BOOL
		#undef TWEAK_INT
		#undef TWEAK_FLOAT
		#undef TWEAK_COLOR
		#undef TWEAK_OPTIONS

		g_d3d.tweakvars_config_last_modified_time = cfg->last_modified_time;
	}

	void UpdateTweakvarsFromIOConfig()
	{
		if (g_d3d.tweakvars_config_last_modified_time != g_d3d.io.config->last_modified_time)
		{
			g_d3d.tweakvars_config_last_modified_time = g_d3d.io.config->last_modified_time;

			RT_Config *cfg = g_d3d.io.config;

			#define TWEAK_CATEGORY_BEGIN(name)
			#define TWEAK_CATEGORY_END() 
			#define TWEAK_BOOL(name, var, value) { int result = tweak_vars.var; RT_ConfigReadInt(cfg, RT_StringLiteral(#var), &result); tweak_vars.var = result; }
			#define TWEAK_INT(name, var, value, min, max) RT_ConfigReadInt(cfg, RT_StringLiteral(#var), &tweak_vars.var);
			#define TWEAK_FLOAT(name, var, value, min, max) RT_ConfigReadFloat(cfg, RT_StringLiteral(#var), &tweak_vars.var);
			#define TWEAK_COLOR(name, var, value) { RT_ConfigReadVec3(cfg, RT_StringLiteral(#var), &tweak_vars.var.xyz); tweak_vars.var.w = 1.0f; }
			#define TWEAK_OPTIONS(name, var, value, ...) RT_ConfigReadInt(cfg, RT_StringLiteral(#var), &tweak_vars.var);

			#include "shared_tweakvars.hlsl.h"

			#undef TWEAK_CATEGORY_BEGIN
			#undef TWEAK_CATEGORY_END
			#undef TWEAK_BOOL
			#undef TWEAK_INT
			#undef TWEAK_FLOAT
			#undef TWEAK_COLOR
			#undef TWEAK_OPTIONS
		}
	}

	float Halton(int i, int base)
	{
		float f = 1;
		float r = 0;

		while (i > 0)
		{
			f = f / base;
			r = r + f*(i % base);
			i = i / base;
		}

		return r;
	}

	wchar_t *Utf16FromUtf8(RT_Arena *arena, const char *utf8)
	{
		size_t utf8_count = strlen(utf8);

		if (NEVER(utf8_count > INT_MAX))  
			utf8_count = INT_MAX;

		if (utf8_count == 0)  
			nullptr;

		int      utf16_count = MultiByteToWideChar(CP_UTF8, 0, utf8, (int)utf8_count, NULL, 0);
		wchar_t *utf16_data  = RT_ArenaAllocArray(arena, utf16_count + 1, wchar_t);

		if (ALWAYS(utf16_data))
		{
			MultiByteToWideChar(CP_UTF8, 0, utf8, (int)utf8_count, (wchar_t *)utf16_data, utf16_count);
			utf16_data[utf16_count] = 0;
		}

		return utf16_data;
	}

	uint64_t GetLastWriteTime(const wchar_t *path)
	{
		FILETIME last_write_time = {};

		WIN32_FILE_ATTRIBUTE_DATA data;
		if (GetFileAttributesExW(path, GetFileExInfoStandard, &data))
		{
			last_write_time = data.ftLastWriteTime;
		}

		 ULARGE_INTEGER thanks;
		 thanks.LowPart  = last_write_time.dwLowDateTime;
		 thanks.HighPart = last_write_time.dwLowDateTime;

		return thanks.QuadPart;
	}

	wchar_t *Utf16PrintF(RT_Arena *arena, const char *fmt, ...)
	{
		va_list args;
		va_start(args, fmt);

		wchar_t *result = Utf16FromUtf8(arena, RT_ArenaPrintFV(&g_thread_arena, fmt, args));

		va_end(args);

		return result;
	}

	// -----------------------------------------------------------------------------------------------------
	// Shader cache: compiled shaders are saved to shader_cache/ next to the game, so later launches skip
	// DXC. The key covers the CONTENT of every file under assets/shaders (shaders include each other),
	// plus the file, entry point, target, defines and compile settings, so any shader edit recompiles.

	static uint64_t ShaderCacheHash(uint64_t h, const void *data, size_t size)
	{
		const uint8_t *p = (const uint8_t *)data;
		for (size_t i = 0; i < size; i++)
			h = (h ^ p[i]) * 1099511628211ull; // FNV-1a 64
		return h;
	}

	static uint64_t ShaderCacheHashWide(uint64_t h, const wchar_t *s)
	{
		h = ShaderCacheHash(h, s, s ? wcslen(s) * sizeof(wchar_t) : 0);
		return ShaderCacheHash(h, "|", 1);
	}

	static uint64_t ShaderCacheHashFolder(uint64_t h, const wchar_t *dir)
	{
		wchar_t pattern[MAX_PATH];
		swprintf(pattern, MAX_PATH, L"%s/*", dir);
		WIN32_FIND_DATAW fd;
		HANDLE find = FindFirstFileW(pattern, &fd);
		if (find == INVALID_HANDLE_VALUE)
			return h;
		do
		{
			if (fd.cFileName[0] == L'.')
				continue;
			wchar_t path[MAX_PATH];
			swprintf(path, MAX_PATH, L"%s/%s", dir, fd.cFileName);
			h = ShaderCacheHashWide(h, fd.cFileName);
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			{
				h = ShaderCacheHashFolder(h, path);
				continue;
			}
			FILE *f = _wfopen(path, L"rb");
			if (!f)
				continue;
			uint8_t buf[16384];
			size_t n;
			while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
				h = ShaderCacheHash(h, buf, n);
			fclose(f);
		} while (FindNextFileW(find, &fd));
		FindClose(find);
		return h;
	}

	// Deletes everything in shader_cache/ except the folder for the current shader version, so
	// shaders compiled from older shader files never pile up.
	static void ShaderCacheDeleteTree(const wchar_t *dir)
	{
		wchar_t pattern[MAX_PATH];
		swprintf(pattern, MAX_PATH, L"%s/*", dir);
		WIN32_FIND_DATAW fd;
		HANDLE find = FindFirstFileW(pattern, &fd);
		if (find != INVALID_HANDLE_VALUE)
		{
			do
			{
				if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
					continue;
				wchar_t path[MAX_PATH];
				swprintf(path, MAX_PATH, L"%s/%s", dir, fd.cFileName);
				if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					ShaderCacheDeleteTree(path);
				else
					DeleteFileW(path);
			} while (FindNextFileW(find, &fd));
			FindClose(find);
		}
		RemoveDirectoryW(dir);
	}

	static void ShaderCachePruneOldVersions(uint64_t source_hash)
	{
		wchar_t keep[32];
		swprintf(keep, 32, L"%016llx", (unsigned long long)source_hash);
		WIN32_FIND_DATAW fd;
		HANDLE find = FindFirstFileW(L"shader_cache/*", &fd);
		if (find == INVALID_HANDLE_VALUE)
			return;
		do
		{
			if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") || !wcscmp(fd.cFileName, keep))
				continue;
			wchar_t path[MAX_PATH];
			swprintf(path, MAX_PATH, L"shader_cache/%s", fd.cFileName);
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				ShaderCacheDeleteTree(path);
			else
				DeleteFileW(path);
		} while (FindNextFileW(find, &fd));
		FindClose(find);
	}

	// Hash of everything under assets/shaders, refreshed at most once a second (startup compiles
	// dozens of shaders back to back; a later hot-reload sees the edited files). Whenever it
	// changes, the cache for the old shader files is deleted.
	static uint64_t ShaderCacheSourceHash()
	{
		static uint64_t hash = 0;
		static ULONGLONG computed_at = 0;
		const ULONGLONG now = GetTickCount64();
		if (!hash || now - computed_at > 1000)
		{
			const uint64_t new_hash = ShaderCacheHashFolder(14695981039346656037ull, L"assets/shaders");
			if (new_hash != hash)
				ShaderCachePruneOldVersions(new_hash);
			hash = new_hash;
			computed_at = now;
		}
		return hash;
	}

	static void ShaderCachePath(wchar_t *out, size_t out_count, uint64_t key)
	{
		swprintf(out, out_count, L"shader_cache/%016llx/%016llx.dxil", (unsigned long long)ShaderCacheSourceHash(), (unsigned long long)key);
	}

	static IDxcBlob *ShaderCacheLoad(uint64_t key)
	{
		wchar_t path[MAX_PATH];
		ShaderCachePath(path, MAX_PATH, key);
		FILE *f = _wfopen(path, L"rb");
		if (!f)
			return nullptr;
		fseek(f, 0, SEEK_END);
		long size = ftell(f);
		fseek(f, 0, SEEK_SET);
		IDxcBlobEncoding *blob = nullptr;
		if (size > 0)
		{
			void *data = malloc((size_t)size);
			if (data && fread(data, 1, (size_t)size, f) == (size_t)size)
				g_d3d.dxc_utils->CreateBlob(data, (UINT32)size, 0, &blob);
			free(data);
		}
		fclose(f);
		return blob;
	}

	static void ShaderCacheStore(uint64_t key, IDxcBlob *blob)
	{
		wchar_t path[MAX_PATH], dir[64];
		CreateDirectoryW(L"shader_cache", nullptr);
		swprintf(dir, 64, L"shader_cache/%016llx", (unsigned long long)ShaderCacheSourceHash());
		CreateDirectoryW(dir, nullptr);
		ShaderCachePath(path, MAX_PATH, key);
		FILE *f = _wfopen(path, L"wb");
		if (!f)
			return;
		fwrite(blob->GetBufferPointer(), 1, blob->GetBufferSize(), f);
		fclose(f);
	}

	IDxcBlob *CompileShader(const wchar_t *file_name, const wchar_t *entry_point, const wchar_t *target_profile, uint32_t num_defines = 0, const DxcDefine *defines = nullptr)
	{
        HRESULT hr;

		uint64_t cache_key = ShaderCacheSourceHash();
		cache_key = ShaderCacheHashWide(cache_key, file_name);
		cache_key = ShaderCacheHashWide(cache_key, entry_point);
		cache_key = ShaderCacheHashWide(cache_key, target_profile);
		for (uint32_t i = 0; i < num_defines; i++)
		{
			cache_key = ShaderCacheHashWide(cache_key, defines[i].Name);
			cache_key = ShaderCacheHashWide(cache_key, defines[i].Value);
		}
		// Compile settings that change the output (see arguments below)
#ifdef SHIPPING_BUILD
		cache_key = ShaderCacheHashWide(cache_key, L"shipping");
#endif
#ifdef _DEBUG
		cache_key = ShaderCacheHashWide(cache_key, L"debug");
#endif
		cache_key = ShaderCacheHashWide(cache_key, L"O3 rowmajor HV2021 v1");

		if (IDxcBlob *cached = ShaderCacheLoad(cache_key))
			return cached;

		UINT32 codepage = 0;

		IDxcBlobEncoding *shader_text = nullptr;
		DeferRelease(shader_text);

		DX_CALL(g_d3d.dxc_utils->LoadFile(file_name, &codepage, &shader_text));

		const wchar_t* arguments[] =
		{
			L"-I", L"assets/shaders",
#ifndef SHIPPING_BUILD
		    DXC_ARG_WARNINGS_ARE_ERRORS,
#endif
			DXC_ARG_OPTIMIZATION_LEVEL3,
			DXC_ARG_PACK_MATRIX_ROW_MAJOR,
			L"-HV", L"2021", // NOTE(daniel): I switched to HLSL 2021 to have "using namespace"
#ifdef _DEBUG
			DXC_ARG_DEBUG,
			DXC_ARG_SKIP_OPTIMIZATIONS,
			//L"-D", L"IS_DEBUG",
#endif
		};

		IDxcOperationResult *result;
		DX_CALL(g_d3d.dxc_compiler->Compile(shader_text, file_name, entry_point, target_profile,
											arguments, RT_ARRAY_COUNT(arguments),
											defines, num_defines,
											g_d3d.dxc_include_handler, &result));
		DeferRelease(result);

		result->GetStatus(&hr);

		if (FAILED(hr))
		{
			IDxcBlobEncoding *error;
			result->GetErrorBuffer(&error);
			OutputDebugStringA((char *)error->GetBufferPointer());
			MessageBoxA(nullptr, (char *)error->GetBufferPointer(), "Shader Compilation Failed!", MB_OK);

			return nullptr;
		}

		IDxcBlob *blob;
		result->GetResult(&blob);

		if (blob)
			ShaderCacheStore(cache_key, blob);

		return blob;
	}

#ifdef D2_ENABLE_OPENVR
#include "HybridVisibility.inl"
#endif

	ID3D12PipelineState *CreateComputePipeline(const wchar_t *source, const wchar_t *entry_point, ID3D12RootSignature* root_sig)
	{
		MemoryScope temp;

		size_t defines_count = g_d3d.global_shader_defines.kv_count;
		DxcDefine *defines = RT_ArenaAllocArrayNoZero(temp, defines_count, DxcDefine);

		for (RT_ConfigIterator it = RT_IterateConfig(&g_d3d.global_shader_defines);
			 RT_ConfigIterValid(&it);
			 RT_ConfigIterNext(&it))
		{
			RT_ConfigKeyValue *kv = it.at;
			defines[it.index].Name  = Utf16FromUtf8(temp, kv->key);
			defines[it.index].Value = Utf16FromUtf8(temp, kv->value);
		}

		IDxcBlob *blob = CompileShader(source, entry_point, L"cs_6_5", (UINT)defines_count, defines);

		if (!blob)
			return nullptr;

		D3D12_SHADER_BYTECODE bytecode = 
		{
			blob->GetBufferPointer(),
			blob->GetBufferSize(),
		};

		D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc = {};
		pipeline_desc.pRootSignature = root_sig;
		pipeline_desc.CS             = bytecode;

		ID3D12PipelineState *result = nullptr;
		DX_CALL(g_d3d.device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&result)));

		return result;
	}

	ID3DBlob* CompileVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& versioned_root_sig_desc)
	{
		ID3DBlob* serialized_root_sig;
		ID3DBlob* error;

		DX_CALL(D3D12SerializeVersionedRootSignature(&versioned_root_sig_desc, &serialized_root_sig, &error));

		if (error)
		{
			RT_FATAL_ERROR((char*)error->GetBufferPointer());
		}

		SafeRelease(error);
		return serialized_root_sig;
	}

	void CreateLightsBuffer()
	{
		g_d3d.lights_buffer = RT_CreateReadOnlyBuffer(L"Lights Buffer", sizeof(RT_Light) * RT_MAX_LIGHTS);
	};

    void EnableDebugLayer()
    {
        if(std::wcsstr(GetCommandLineW(),L"-openvr-gpu-validation")) {
            ID3D12Debug* debug=nullptr;
            if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) { debug->EnableDebugLayer();debug->Release(); }
        }

#if defined(_DEBUG)
		{
			ID3D12Debug* debug0;

			DX_CALL(D3D12GetDebugInterface(IID_PPV_ARGS(&debug0)));
			debug0->EnableDebugLayer();

			if (GPU_VALIDATION_ENABLED)
			{
				ID3D12Debug1* debug1;

				DX_CALL(debug0->QueryInterface(IID_PPV_ARGS(&debug1)));
				debug1->SetEnableGPUBasedValidation(true);

				SafeRelease(debug1);
			}

			SafeRelease(debug0);
		}
#endif

		ID3D12DeviceRemovedExtendedDataSettings1 *dred_settings;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred_settings))))
		{
			dred_settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			dred_settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			SafeRelease(dred_settings);
		}
    }

    void CreateDevice()
    {
        // Create factory
        UINT createFactoryFlags = 0;
#if defined(_DEBUG)
        createFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif
        IDXGIFactory4* dxgiFactory4;
        DX_CALL(CreateDXGIFactory2(createFactoryFlags, IID_PPV_ARGS(&dxgiFactory4)));

        // Create adapter
        IDXGIAdapter1* dxgiAdapter1;
        SIZE_T maxDedicatedVideoMemory = 0;

        for (UINT i = 0; dxgiFactory4->EnumAdapters1(i, &dxgiAdapter1) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 dxgiAdapterDesc1;
            dxgiAdapter1->GetDesc1(&dxgiAdapterDesc1);

            if ((dxgiAdapterDesc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
                SUCCEEDED(D3D12CreateDevice(dxgiAdapter1, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)) &&
				dxgiAdapterDesc1.DedicatedVideoMemory > maxDedicatedVideoMemory)
            {
                maxDedicatedVideoMemory = dxgiAdapterDesc1.DedicatedVideoMemory;
                DX_CALL(dxgiAdapter1->QueryInterface(&g_d3d.dxgi_adapter4));
            }
        }

        // Create device
        DX_CALL(D3D12CreateDevice(g_d3d.dxgi_adapter4, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_d3d.device)));
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 featureOptions;
        DX_CALL(g_d3d.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &featureOptions, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS5)););

        if (featureOptions.RaytracingTier < D3D12_RAYTRACING_TIER_1_0)
        {
			RT_FATAL_ERROR("Raytracing is not supported on this system.");
        }

#if defined(_DEBUG)
        ID3D12DebugDevice1* d3d12DebugDevice1;
        if (GPU_VALIDATION_ENABLED)
        {
            // Set up GPU validation
            DX_CALL(g_d3d.device->QueryInterface(IID_PPV_ARGS(&d3d12DebugDevice1)));

            D3D12_DEBUG_DEVICE_GPU_BASED_VALIDATION_SETTINGS debugValidationSettings = {};
            debugValidationSettings.MaxMessagesPerCommandList = 10;
            debugValidationSettings.DefaultShaderPatchMode = D3D12_GPU_BASED_VALIDATION_SHADER_PATCH_MODE_GUARDED_VALIDATION;
            debugValidationSettings.PipelineStateCreateFlags = D3D12_GPU_BASED_VALIDATION_PIPELINE_STATE_CREATE_FLAG_FRONT_LOAD_CREATE_GUARDED_VALIDATION_SHADERS;
            DX_CALL(d3d12DebugDevice1->SetDebugParameter(D3D12_DEBUG_DEVICE_PARAMETER_GPU_BASED_VALIDATION_SETTINGS, &debugValidationSettings, sizeof(D3D12_DEBUG_DEVICE_GPU_BASED_VALIDATION_SETTINGS)));

			SafeRelease(d3d12DebugDevice1);
        }

        ID3D12InfoQueue* d3d12InfoQueue;
        // Set up info queue with filters
        if (SUCCEEDED(g_d3d.device->QueryInterface(&d3d12InfoQueue)))
        {
            d3d12InfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            d3d12InfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
            d3d12InfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);

            // Suppress messages based on their severity level
            D3D12_MESSAGE_SEVERITY severities[] =
            {
                D3D12_MESSAGE_SEVERITY_INFO
            };

            // Suppress individual messages by their ID
            D3D12_MESSAGE_ID denyIds[] =
            {
                D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
                D3D12_MESSAGE_ID_MAP_INVALID_NULLRANGE,
                D3D12_MESSAGE_ID_UNMAP_INVALID_NULLRANGE,
                // This is a temporary fix for Windows 11 due to a bug that has not been fixed yet
                D3D12_MESSAGE_ID_RESOURCE_BARRIER_MISMATCHING_COMMAND_LIST_TYPE,
            };

            D3D12_INFO_QUEUE_FILTER newFilter = {};
            newFilter.DenyList.NumSeverities = _countof(severities);
            newFilter.DenyList.pSeverityList = severities;
            newFilter.DenyList.NumIDs = _countof(denyIds);
            newFilter.DenyList.pIDList = denyIds;

            DX_CALL(d3d12InfoQueue->PushStorageFilter(&newFilter));
        }

        SafeRelease(d3d12InfoQueue);
#endif

        SafeRelease(dxgiAdapter1);
        SafeRelease(dxgiFactory4);
    }

    void CreateCommandQueues()
    {
		// TODO(Justin): This is a relic from the past. Why does this call new? Idk
		// Also we want to refactor command lists and have multiple command queues potentially, but we might not end up having time for this.
        g_d3d.command_queue_direct = new CommandQueue(D3D12_COMMAND_LIST_TYPE_DIRECT);
    }

    void CreateSwapChain(HWND hWnd)
    {
        IDXGIFactory* dxgiFactory;
        IDXGIFactory5* dxgiFactory5;

        DX_CALL(g_d3d.dxgi_adapter4->GetParent(IID_PPV_ARGS(&dxgiFactory)));
        DX_CALL(dxgiFactory->QueryInterface(&dxgiFactory5));

        BOOL allowTearing = false;
        if (SUCCEEDED(dxgiFactory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(BOOL))))
        {
            g_d3d.tearing_supported = (allowTearing == TRUE);
        }

        RECT clientRect;
        ::GetClientRect(hWnd, &clientRect);

        DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
        swapChainDesc.Width = clientRect.right - clientRect.left;
        swapChainDesc.Height = clientRect.bottom - clientRect.top;
#ifdef D2_ENABLE_OPENVR
        if(OpenVRPrototype::FixedMirrorCanvas()) { swapChainDesc.Width=RT_VR_MIRROR_WIDTH;swapChainDesc.Height=RT_VR_MIRROR_HEIGHT; }
#endif
        swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDesc.Stereo = FALSE;
        swapChainDesc.SampleDesc = { 1, 0 };
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.BufferCount = BACK_BUFFER_COUNT_SWAP;
        swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        swapChainDesc.Flags = g_d3d.tearing_supported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

        IDXGISwapChain1* dxgiSwapChain1;

        DX_CALL(dxgiFactory5->CreateSwapChainForHwnd(g_d3d.command_queue_direct->GetD3D12CommandQueue(),
            hWnd, &swapChainDesc, nullptr, nullptr, &dxgiSwapChain1));
        DX_CALL(dxgiSwapChain1->QueryInterface(&g_d3d.dxgi_swapchain4));
        DX_CALL(dxgiFactory5->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER));

		for (UINT i = 0; i < BACK_BUFFER_COUNT_SWAP; ++i)
		{
			DX_CALL(g_d3d.dxgi_swapchain4->GetBuffer(i, IID_PPV_ARGS(&g_d3d.back_buffers[i])));
			RT_TRACK_RESOURCE(g_d3d.back_buffers[i], D3D12_RESOURCE_STATE_PRESENT);
#ifdef D2_ENABLE_OPENVR
            if(OpenVRPrototype::FixedMirrorCanvas()) {
                g_d3d.spectator_views[i]=g_d3d.rtv.Allocate(1);
                CreateTextureRTV(g_d3d.back_buffers[i],g_d3d.spectator_views[i].cpu,DXGI_FORMAT_R8G8B8A8_UNORM);
            }
#endif
		}

        g_d3d.current_back_buffer_index = g_d3d.dxgi_swapchain4->GetCurrentBackBufferIndex();
    }

    void CreateDescriptorHeaps()
    {
		// Since any texture could - in theory - be used as a render target, we will mimic the size of the CBV_SRV_UAV heap for the RTV heap
		g_d3d.rtv.Init(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, CBV_SRV_UAV_HEAP_SIZE, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
		g_d3d.dsv.Init(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
		g_d3d.cbv_srv_uav.Init(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, CBV_SRV_UAV_HEAP_SIZE, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
		// Desktop slots, plus two cockpit camera views per swap slot.
		UINT frameDescriptorSlots=BACK_BUFFER_COUNT+BACK_BUFFER_COUNT_SWAP*kCameraViews;
#ifdef D2_ENABLE_OPENVR
		// Plus the two VR eyes per swap slot.
		frameDescriptorSlots+=BACK_BUFFER_COUNT_SWAP*2;
#endif
		g_d3d.cbv_srv_uav_staging.Init(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3D12GlobalDescriptors_COUNT*frameDescriptorSlots, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
		g_d3d.cbv_srv_uav_mips.Init(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, CBV_SRV_UAV_HEAP_SIZE, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
	}

    void CreateRootSignatures()
    {
		// ------------------------------------------------------------------
		// Global root signature

		{
			D3D12_DESCRIPTOR_RANGE1 ranges[5] = {};
			
			ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
			ranges[0].NumDescriptors = D3D12GlobalDescriptors_SRV_START - D3D12GlobalDescriptors_UAV_START;
			ranges[0].OffsetInDescriptorsFromTableStart = D3D12GlobalDescriptors_UAV_START;
			ranges[0].BaseShaderRegister = 0;
			ranges[0].RegisterSpace = 0;
			ranges[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;

			ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			ranges[1].NumDescriptors = D3D12GlobalDescriptors_CBV_START - D3D12GlobalDescriptors_SRV_START;
			ranges[1].OffsetInDescriptorsFromTableStart = D3D12GlobalDescriptors_SRV_START;
			ranges[1].BaseShaderRegister = 0;
			ranges[1].RegisterSpace = 0;
			ranges[1].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE|D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

			ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
			ranges[2].NumDescriptors = D3D12GlobalDescriptors_UAV_RT_START - D3D12GlobalDescriptors_CBV_START;
			ranges[2].OffsetInDescriptorsFromTableStart = D3D12GlobalDescriptors_CBV_START;
			ranges[2].BaseShaderRegister = 0;
			ranges[2].RegisterSpace = 0;
			ranges[2].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;

			ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
			ranges[3].NumDescriptors = D3D12GlobalDescriptors_SRV_RT_START - D3D12GlobalDescriptors_UAV_RT_START;
			ranges[3].OffsetInDescriptorsFromTableStart = D3D12GlobalDescriptors_UAV_RT_START;
			ranges[3].BaseShaderRegister = 0;
			ranges[3].RegisterSpace = 999;
			ranges[3].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;

			ranges[4].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			ranges[4].NumDescriptors = D3D12GlobalDescriptors_COUNT - D3D12GlobalDescriptors_SRV_RT_START;
			ranges[4].OffsetInDescriptorsFromTableStart = D3D12GlobalDescriptors_SRV_RT_START;
			ranges[4].BaseShaderRegister = 0;
			ranges[4].RegisterSpace = 999;
			ranges[4].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE|D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

			D3D12_ROOT_PARAMETER1 root_parameters[RaytraceRootParameters_COUNT] = {};

			root_parameters[RaytraceRootParameters_MainDescriptorTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			root_parameters[RaytraceRootParameters_MainDescriptorTable].DescriptorTable.NumDescriptorRanges = RT_ARRAY_COUNT(ranges);
			root_parameters[RaytraceRootParameters_MainDescriptorTable].DescriptorTable.pDescriptorRanges = ranges;
			root_parameters[RaytraceRootParameters_MainDescriptorTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

			// Wavelet filter iteration
			root_parameters[RaytraceRootParameters_DenoiseIteration].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			root_parameters[RaytraceRootParameters_DenoiseIteration].Constants.Num32BitValues = 1;
			root_parameters[RaytraceRootParameters_DenoiseIteration].Constants.RegisterSpace  = 2;
			root_parameters[RaytraceRootParameters_DenoiseIteration].Constants.ShaderRegister = 0;
			root_parameters[RaytraceRootParameters_DenoiseIteration].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

			D3D12_DESCRIPTOR_RANGE1 bindless_ranges[2] = {};

			bindless_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			bindless_ranges[0].NumDescriptors = CBV_SRV_UAV_HEAP_SIZE;
			bindless_ranges[0].OffsetInDescriptorsFromTableStart = 0;
			bindless_ranges[0].BaseShaderRegister = 0;
			bindless_ranges[0].RegisterSpace = 3;
			bindless_ranges[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE|D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

			bindless_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			bindless_ranges[1].NumDescriptors = CBV_SRV_UAV_HEAP_SIZE;
			bindless_ranges[1].OffsetInDescriptorsFromTableStart = 0;
			bindless_ranges[1].BaseShaderRegister = 0;
			bindless_ranges[1].RegisterSpace = 4;
			bindless_ranges[1].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

			root_parameters[RaytraceRootParameters_BindlessSRVTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			root_parameters[RaytraceRootParameters_BindlessSRVTable].DescriptorTable.NumDescriptorRanges = 1;
			root_parameters[RaytraceRootParameters_BindlessSRVTable].DescriptorTable.pDescriptorRanges = &bindless_ranges[0];
			root_parameters[RaytraceRootParameters_BindlessSRVTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

			root_parameters[RaytraceRootParameters_BindlessTriangleBufferTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			root_parameters[RaytraceRootParameters_BindlessTriangleBufferTable].DescriptorTable.NumDescriptorRanges = 1;
			root_parameters[RaytraceRootParameters_BindlessTriangleBufferTable].DescriptorTable.pDescriptorRanges = &bindless_ranges[1];
			root_parameters[RaytraceRootParameters_BindlessTriangleBufferTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

			D3D12_STATIC_SAMPLER_DESC static_samplers[3] = {};
			// Anisotropic: SampleTextureAnisotropic passes ray-cone ellipse
			// gradients to SampleGrad. With plain trilinear filtering the GPU
			// picks the mip from the long axis, so walls/floors seen at grazing
			// angles (most of a Descent tunnel) fall to very blurry mips.
			// SampleLevel calls through this sampler are unaffected.
			static_samplers[0].Filter = D3D12_FILTER_ANISOTROPIC;
			static_samplers[0].MaxAnisotropy = 16;
			static_samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			static_samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			static_samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			static_samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
			static_samplers[0].MinLOD = 0.0f;
			static_samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
			static_samplers[0].MipLODBias = 0;
			static_samplers[0].ShaderRegister = 0;
			static_samplers[0].RegisterSpace = 0;
			static_samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

			static_samplers[1].Filter = D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;
			static_samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			static_samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			static_samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			static_samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
			static_samplers[1].MinLOD = 0.0f;
			static_samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
			static_samplers[1].MipLODBias = 0;
			static_samplers[1].ShaderRegister = 1;
			static_samplers[1].RegisterSpace = 0;
			static_samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

			static_samplers[2].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			static_samplers[2].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			static_samplers[2].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			static_samplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			static_samplers[2].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
			static_samplers[2].MinLOD = 0.0f;
			static_samplers[2].MaxLOD = D3D12_FLOAT32_MAX;
			static_samplers[2].MipLODBias = 0;
			static_samplers[2].ShaderRegister = 2;
			static_samplers[2].RegisterSpace = 0;
			static_samplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			
			D3D12_VERSIONED_ROOT_SIGNATURE_DESC global_root_sig_desc = {};
			global_root_sig_desc.Desc_1_1.NumParameters = RT_ARRAY_COUNT(root_parameters);
			global_root_sig_desc.Desc_1_1.pParameters = &root_parameters[0];
			global_root_sig_desc.Desc_1_1.NumStaticSamplers = RT_ARRAY_COUNT(static_samplers);
			global_root_sig_desc.Desc_1_1.pStaticSamplers = &static_samplers[0];
			global_root_sig_desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
			global_root_sig_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;

			ID3DBlob* serialized_root_sig = CompileVersionedRootSignature(global_root_sig_desc);
			DX_CALL(g_d3d.device->CreateRootSignature(0, serialized_root_sig->GetBufferPointer(), serialized_root_sig->GetBufferSize(), IID_PPV_ARGS(&g_d3d.global_root_sig)));
			g_d3d.global_root_sig->SetName(L"Global Root Signature");

			SafeRelease(serialized_root_sig);
		}
    }

	void CreatePixelDebugResources()
	{
		size_t buffer_size = g_d3d.render_width * g_d3d.render_height * sizeof(PixelDebugData);
		g_d3d.pixel_debug.uav_resource = RT_CreateReadWriteBuffer(L"Pixel Debug UAV Buffer", buffer_size);

#if RT_PIXEL_DEBUG
		for (size_t i = 0; i < BACK_BUFFER_COUNT; i++)
		{
			FrameData *frame = &g_d3d.frame_data[i];
			frame->pixel_debug_readback = RT_CreateReadbackBuffer(L"Pixel Debug Readback", buffer_size);
		}
#endif
	}

	// -----------------------------------------------------------------------------------------------------
	// Compute shaders

	void ReloadComputeShader(ComputeShader* cs, const wchar_t* file, const wchar_t* entry_point)
	{
		uint64_t timestamp = GetLastWriteTime(file);

		bool shader_file_updated = timestamp != cs->timestamp;
		bool shader_defines_updated = g_shader_defines.last_modified_time != g_d3d.global_shader_defines.last_modified_time;

		// NOTE(daniel): For reasons opaque to me, it seems sometimes the new file's timestamp
		// is less than the current. So this tested (timestamp > cs->timestamp) before, but
		// that turns out to be unreliable.
		if (shader_file_updated || shader_defines_updated)
		{
			ID3D12PipelineState* new_pso = CreateComputePipeline(file, entry_point, g_d3d.global_root_sig);
			if (new_pso)
			{
				RenderBackend::Flush();

				SafeRelease(cs->pso);
				cs->pso = new_pso;

				OutputDebugStringW(L"Reloaded shader ");
				OutputDebugStringW(file);
				OutputDebugStringW(L" - ");
				OutputDebugStringW(entry_point);
				OutputDebugStringW(L"\n");
			}
			else
			{
				OutputDebugStringW(L"SHADER RELOAD FAILED: ");
				OutputDebugStringW(file);
				OutputDebugStringW(L" - ");
				OutputDebugStringW(entry_point);
				OutputDebugStringW(L"\n");
			}
			cs->timestamp = timestamp;
		}
	}

	void ReloadComputeShadersIfThereAreNewOnes()
	{
		// The copy script writes a temporary "lock file" to indicate it is still copying, and we shouldn't try to reload yet
		// I am using GetLastWriteTime returning 0 as a test for whether a file exists.
		if (!GetLastWriteTime(L"assets/shaders/lock_file.temp"))
		{
			// ReloadComputeShader(&g_d3d.cs.restir_gen_candidates, L"assets/shaders/restir/gen_candidates.hlsl", L"ReSTIR_GenerateCandidates");

			ReloadComputeShader(&g_d3d.cs.svgf_prepass, L"assets/shaders/denoiser/prepass.hlsl", L"Denoise_Prepass");
			ReloadComputeShader(&g_d3d.cs.svgf_history_fix, L"assets/shaders/denoiser/history_fix.hlsl", L"Denoise_HistoryFix");
			ReloadComputeShader(&g_d3d.cs.svgf_resample, L"assets/shaders/denoiser/resample.hlsl", L"Denoise_Resample");
			ReloadComputeShader(&g_d3d.cs.svgf_post_resample, L"assets/shaders/denoiser/post_resample.hlsl", L"Denoise_PostResample");
			ReloadComputeShader(&g_d3d.cs.svgf_blur, L"assets/shaders/denoise.hlsl", L"DenoiseDirectCS");
#ifdef D2_ENABLE_OPENVR
            ReloadComputeShader(&g_d3d.cs.hybrid_primary, L"assets/shaders/primary_hybrid.hlsl", L"PrimaryRayInline");
            ReloadComputeShader(&g_d3d.cs.svgf_blur_vr, L"assets/shaders/denoise_vr.hlsl", L"DenoiseDirectCS");
            ReloadComputeShader(&g_d3d.cs.svgf_resample_vr, L"assets/shaders/denoiser/resample_vr.hlsl", L"Denoise_Resample");
#endif

			ReloadComputeShader(&g_d3d.cs.taa, L"assets/shaders/taa.hlsl", L"TemporalAntiAliasingCS");

			ReloadComputeShader(&g_d3d.cs.bloom_prepass, L"assets/shaders/bloom.hlsl", L"Bloom_Prepass");
			ReloadComputeShader(&g_d3d.cs.bloom_blur_horz, L"assets/shaders/bloom.hlsl", L"Bloom_BlurHorz");
			ReloadComputeShader(&g_d3d.cs.bloom_blur_vert, L"assets/shaders/bloom.hlsl", L"Bloom_BlurVert");

			ReloadComputeShader(&g_d3d.cs.vr_fsr_depth, L"assets/shaders/vr_fsr_depth.hlsl", L"VRFsrDepthCS");
			ReloadComputeShader(&g_d3d.cs.composite, L"assets/shaders/composite.hlsl", L"CompositeCS");
			ReloadComputeShader(&g_d3d.cs.post_process, L"assets/shaders/post_process.hlsl", L"PostProcessCS");
			ReloadComputeShader(&g_d3d.cs.resolve_final_color, L"assets/shaders/resolve_final_color.hlsl", L"ResolveFinalColorCS");
		}
	}

	void CreateGenMipMapComputeShader()
	{
		D3D12_DESCRIPTOR_RANGE1 ranges[2] = {};
		ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		ranges[0].NumDescriptors = 1;
		ranges[0].OffsetInDescriptorsFromTableStart = 0;
		ranges[0].BaseShaderRegister = 0;
		ranges[0].RegisterSpace = 0;
		ranges[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;

		ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		ranges[1].NumDescriptors = 4;
		ranges[1].OffsetInDescriptorsFromTableStart = 0;
		ranges[1].BaseShaderRegister = 0;
		ranges[1].RegisterSpace = 0;
		ranges[1].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;

		D3D12_ROOT_PARAMETER1 root_parameters[3] = {};
		root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		root_parameters[0].Constants.Num32BitValues = sizeof(GenMipMapSettings) / 4;
		root_parameters[0].Constants.ShaderRegister = 0;
		root_parameters[0].Constants.RegisterSpace = 0;
		root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		root_parameters[1].DescriptorTable.NumDescriptorRanges = 1;
		root_parameters[1].DescriptorTable.pDescriptorRanges = &ranges[0];
		root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		root_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		root_parameters[2].DescriptorTable.NumDescriptorRanges = 1;
		root_parameters[2].DescriptorTable.pDescriptorRanges = &ranges[1];
		root_parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		D3D12_STATIC_SAMPLER_DESC static_samplers[1] = {};
		static_samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		static_samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		static_samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		static_samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		static_samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		static_samplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
		static_samplers[0].MinLOD = 0.0f;
		static_samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
		static_samplers[0].MipLODBias = 0;
		static_samplers[0].ShaderRegister = 0;
		static_samplers[0].RegisterSpace = 0;
		static_samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		D3D12_VERSIONED_ROOT_SIGNATURE_DESC gen_mipmap_root_sig_desc = {};
		gen_mipmap_root_sig_desc.Desc_1_1.NumParameters = RT_ARRAY_COUNT(root_parameters);
		gen_mipmap_root_sig_desc.Desc_1_1.pParameters = &root_parameters[0];
		gen_mipmap_root_sig_desc.Desc_1_1.NumStaticSamplers = RT_ARRAY_COUNT(static_samplers);
		gen_mipmap_root_sig_desc.Desc_1_1.pStaticSamplers = &static_samplers[0];
		gen_mipmap_root_sig_desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
		gen_mipmap_root_sig_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;

		ID3DBlob* serialized_root_sig = CompileVersionedRootSignature(gen_mipmap_root_sig_desc);
		DX_CALL(g_d3d.device->CreateRootSignature(0, serialized_root_sig->GetBufferPointer(), serialized_root_sig->GetBufferSize(), IID_PPV_ARGS(&g_d3d.gen_mipmap_root_sig)));
		g_d3d.gen_mipmap_root_sig->SetName(L"Gen mip maps root signature");

		SafeRelease(serialized_root_sig);

		g_d3d.cs.gen_mipmaps.pso = CreateComputePipeline(L"assets/shaders/gen_mipmap.hlsl", L"GenMipMapCS", g_d3d.gen_mipmap_root_sig);
	}

#if RT_DISPATCH_RAYS
	void ReloadRaytracingShaders()
	{
		if (GetLastWriteTime(L"assets/shaders/lock_file.temp"))
		{
			return;
		}

		for (size_t shader_index = 0; shader_index < RT_ARRAY_COUNT(g_d3d.rt_shaders_all); shader_index++)
		{
			RaytracingShader *shader = &g_d3d.rt_shaders_all[shader_index];

			uint64_t timestamp = GetLastWriteTime(shader->source_file);
			if (shader->timestamp != timestamp)
			{
				shader->timestamp = timestamp;
				shader->dirty     = true; // this signals that dependent pipelines need to rebuild

				IDxcBlob *blob = CompileShader(shader->source_file, shader->name, L"lib_6_3");
				if (blob)
				{
					RenderBackend::Flush();

					if (shader->blob)
					{
						shader->blob->Release();
					}
					shader->blob = blob;
				}
			}
		}
	}

	void ReloadRaytracingPipeline(RaytracingPipeline *pipeline, const RaytracingPipelineParams &params)
	{
		MemoryScope temp;

		bool dirty = false;
		for (size_t shader_index = 0; shader_index < params.shader_count; shader_index++)
		{
			RaytracingShader *shader = params.shaders[shader_index];
			if (shader->dirty)
			{
				dirty = true;
				break;
			}
		}

		if (!dirty)
		{
			// Nothing to do.
			return;
		}

		constexpr UINT MAX_SUBOBJECTS = 64;

		struct Subobjects
		{
			UINT subobject_count;
			D3D12_STATE_SUBOBJECT subobjects[MAX_SUBOBJECTS];

			D3D12_STATE_SUBOBJECT *Add(D3D12_STATE_SUBOBJECT_TYPE type, const void *desc)
			{
				D3D12_STATE_SUBOBJECT *subobject = nullptr;

				if (ALWAYS(subobject_count < MAX_SUBOBJECTS))
				{
					subobject = &subobjects[subobject_count++];
					subobject->Type  = type;
					subobject->pDesc = desc;
				}

				return subobject;
			}
		};

		Subobjects subobjects = {};

		D3D12_RAYTRACING_SHADER_CONFIG config_desc = {};
		config_desc.MaxPayloadSizeInBytes   = params.max_payload_size;
		config_desc.MaxAttributeSizeInBytes = params.max_attribute_size;
		D3D12_STATE_SUBOBJECT *config = subobjects.Add(D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &config_desc);

		for (size_t i = 0; i < params.shader_count; i++)
		{
			IDxcBlob *blob = params.shaders[i]->blob;

			if (blob)
			{
				D3D12_EXPORT_DESC *export_desc = RT_ArenaAllocStruct(temp, D3D12_EXPORT_DESC);
				export_desc->Name           = params.shaders[i]->export_name;
				export_desc->ExportToRename = params.shaders[i]->name;

				D3D12_DXIL_LIBRARY_DESC *lib_desc = RT_ArenaAllocStruct(temp, D3D12_DXIL_LIBRARY_DESC);
				lib_desc->DXILLibrary.BytecodeLength  = blob->GetBufferSize();
				lib_desc->DXILLibrary.pShaderBytecode = blob->GetBufferPointer();
				lib_desc->NumExports                  = 1;
				lib_desc->pExports                    = export_desc;

				subobjects.Add(D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, lib_desc);
			}
		}

		for (size_t i = 0; i < params.hitgroup_count; i++)
		{
			const HitGroup *hit_group = &params.hitgroups[i];

			D3D12_HIT_GROUP_DESC *hit_group_desc = RT_ArenaAllocStruct(temp, D3D12_HIT_GROUP_DESC);

			if (hit_group->closest)
				hit_group_desc->ClosestHitShaderImport = hit_group->closest->export_name;

			if (hit_group->any)
				hit_group_desc->AnyHitShaderImport = hit_group->any->export_name;

			hit_group_desc->HitGroupExport = hit_group->export_name;
			hit_group_desc->Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;

			subobjects.Add(D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, hit_group_desc);
		}

		// ok this is dumb but whateeeeeeeeeeeeeeever
		
		UINT export_count = 0;
		LPCWSTR *exports = RT_ArenaAllocArray(temp, params.shader_count, LPCWSTR);

		for (size_t i = 0; i < params.shader_count; i++)
		{
			RaytracingShader *shader = params.shaders[i];

			bool shader_is_part_of_hit_group = false;

			for (size_t j = 0; j < params.hitgroup_count; j++)
			{
				const HitGroup *hit_group = &params.hitgroups[j];
				if (hit_group->closest == shader ||
					hit_group->any     == shader)
				{
					shader_is_part_of_hit_group = true;
					break;
				}
			}

			if (!shader_is_part_of_hit_group)
			{
				exports[export_count++] = shader->export_name;
			}
		}

		for (size_t i = 0; i < params.hitgroup_count; i++)
		{
			const HitGroup *hit_group = &params.hitgroups[i];
			exports[export_count++] = hit_group->export_name;
		}

		D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION assoc_desc = {};
		assoc_desc.NumExports            = export_count;
		assoc_desc.pExports              = exports;
		assoc_desc.pSubobjectToAssociate = config;

		subobjects.Add(D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &assoc_desc);

		// add global root signature
		D3D12_GLOBAL_ROOT_SIGNATURE global_root_sig = {};
		global_root_sig.pGlobalRootSignature = g_d3d.global_root_sig;

		subobjects.Add(D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global_root_sig);

		// add pipeline config
		D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config_desc = {};
		pipeline_config_desc.MaxTraceRecursionDepth = RT_MAX(1, params.max_recursion_depth);

		subobjects.Add(D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline_config_desc);

		// create rt pso

		D3D12_STATE_OBJECT_DESC pso_desc = {};
		pso_desc.Type          = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
		pso_desc.NumSubobjects = subobjects.subobject_count;
		pso_desc.pSubobjects   = subobjects.subobjects;

		ID3D12StateObject *pso = nullptr;
		DX_CALL(g_d3d.device->CreateStateObject(&pso_desc, IID_PPV_ARGS(&pso)));

		if (ALWAYS(pso))
		{
			if (pipeline->pso)
			{
				pipeline->pso->Release();
				pipeline->pso_properties->Release();
			}
			pipeline->pso = pso;
			DX_CALL(pipeline->pso->QueryInterface(IID_PPV_ARGS(&pipeline->pso_properties)));
		}
	}

    void ReloadShaderTables()
    {
		// ------------------------------------------------------------------
		// NOTE: this function is only something to call at initialization or
		// during raytracing state reloads.

		// ------------------------------------------------------------------
		// Create upload shader tables for raygen and miss shader records

		uint32_t num_raygen_records = 3;
		uint32_t num_miss_records = 4;
		uint32_t num_hitgroup_records = 4;
		ShaderTable raygen_shader_table_upload = CreateShaderTable(L"Raygen shader table upload", num_raygen_records, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
		ShaderTable miss_shader_table_upload = CreateShaderTable(L"Miss shader table upload", num_miss_records);
		ShaderTable hitgroups_shader_table_upload = CreateShaderTable(L"Hitgroups shader table upload", num_hitgroup_records);

		// ------------------------------------------------------------------
		// Fill out shader table
		
		// ------------------------------------------------------------------
		// Raygen shader table
		{
			// Primary raygen record
			ShaderRecord raygen_records[3] = {};
			void* primary_raygen_shader_identifier = g_d3d.rt_pipelines.primary.pso_properties->GetShaderIdentifier(primary_raygen_export_name);
			memcpy(raygen_records[0].identifier, primary_raygen_shader_identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

			// Direct lighting raygen record
			ShaderRecord direct_lighting_raygen_record = {};
			void* direct_lighting_raygen_shader_identifier = g_d3d.rt_pipelines.direct.pso_properties->GetShaderIdentifier(direct_lighting_raygen_export_name);
			memcpy(raygen_records[1].identifier, direct_lighting_raygen_shader_identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

			// Indirect lighting raygen record
			ShaderRecord indirect_lighting_raygen_record = {};
			void* indirect_lighting_raygen_shader_identifier = g_d3d.rt_pipelines.indirect.pso_properties->GetShaderIdentifier(indirect_lighting_raygen_export_name);
			memcpy(raygen_records[2].identifier, indirect_lighting_raygen_shader_identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
			
			AddEntryToShaderTable(&raygen_shader_table_upload, 3, raygen_records);
		}

		// ------------------------------------------------------------------
		// Miss shader table
		{
			// Primary miss record
			ShaderRecord miss_records[2] = {};
			void* primary_miss_identifier = g_d3d.rt_pipelines.primary.pso_properties->GetShaderIdentifier(primary_miss_export_name);
			memcpy(miss_records[0].identifier, primary_miss_identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

			// Occlusion miss record
			void* occlusion_miss_identifier = g_d3d.rt_pipelines.direct.pso_properties->GetShaderIdentifier(occlusion_miss_export_name);
			memcpy(miss_records[1].identifier, occlusion_miss_identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

			AddEntryToShaderTable(&miss_shader_table_upload, 2, miss_records);

			ShaderRecord test_records[2] = {};
			void* primary_miss_identifier_indirect = g_d3d.rt_pipelines.indirect.pso_properties->GetShaderIdentifier(primary_miss_export_name);
			memcpy(test_records[0].identifier, primary_miss_identifier_indirect, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
			
			void* occlusion_miss_identifier_indirect = g_d3d.rt_pipelines.indirect.pso_properties->GetShaderIdentifier(primary_miss_export_name);
			memcpy(test_records[1].identifier, occlusion_miss_identifier_indirect, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

			AddEntryToShaderTable(&miss_shader_table_upload, 2, test_records);
		}
		
		// ------------------------------------------------------------------
		// Hit group (Occlusion and Primary)
		{
			// Primary ray hitgroup
			ShaderRecord hitgroup_records[2] = {};
			void* primary_hitgroup_identifier_primary = g_d3d.rt_pipelines.primary.pso_properties->GetShaderIdentifier(primary_hitgroup_export_name);
			memcpy(hitgroup_records[0].identifier, primary_hitgroup_identifier_primary, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

			void* occlusion_hitgroup_identifier_direct = g_d3d.rt_pipelines.direct.pso_properties->GetShaderIdentifier(occlusion_hitgroup_export_name);
			memcpy(hitgroup_records[1].identifier, occlusion_hitgroup_identifier_direct, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

			AddEntryToShaderTable(&hitgroups_shader_table_upload, 2, hitgroup_records);

			// Why are the indirect pso identifiers different than the ones in the primary and direct psos?
			ShaderRecord test_records[2] = {};
			void* primary_hitgroup_identifier_indirect = g_d3d.rt_pipelines.indirect.pso_properties->GetShaderIdentifier(primary_hitgroup_export_name);
			memcpy(test_records[0].identifier, primary_hitgroup_identifier_indirect, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

			void* occlusion_hitgroup_identifier_indirect = g_d3d.rt_pipelines.indirect.pso_properties->GetShaderIdentifier(occlusion_hitgroup_export_name);
			memcpy(test_records[1].identifier, occlusion_hitgroup_identifier_indirect, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

			AddEntryToShaderTable(&hitgroups_shader_table_upload, 2, test_records);
		}

		// ------------------------------------------------------------------
		// Create shader tables, if they do not exist yet

		if (!g_d3d.raygen_shader_table)
			g_d3d.raygen_shader_table = RT_CreateReadOnlyBuffer(L"Raygen shader table", raygen_shader_table_upload.byte_size);
		if (!g_d3d.miss_shader_table)
			g_d3d.miss_shader_table = RT_CreateReadOnlyBuffer(L"Miss shader table", miss_shader_table_upload.byte_size);
		if (!g_d3d.hitgroups_shader_table)
			g_d3d.hitgroups_shader_table = RT_CreateReadOnlyBuffer(L"Hitgroups shader table", hitgroups_shader_table_upload.byte_size);

		// ------------------------------------------------------------------
		// Copy to GPU-side shader table

		CommandList &command_list = g_d3d.command_queue_direct->GetCommandList();
		CopyBufferRegion(command_list, g_d3d.raygen_shader_table, 0, raygen_shader_table_upload.resource, 0, raygen_shader_table_upload.byte_size);
		CopyBufferRegion(command_list, g_d3d.miss_shader_table, 0, miss_shader_table_upload.resource, 0, miss_shader_table_upload.byte_size);
		CopyBufferRegion(command_list, g_d3d.hitgroups_shader_table, 0, hitgroups_shader_table_upload.resource, 0, hitgroups_shader_table_upload.byte_size);

		RT_TRACK_TEMP_RESOURCE(raygen_shader_table_upload.resource, &command_list);
		RT_TRACK_TEMP_RESOURCE(miss_shader_table_upload.resource, &command_list);
		RT_TRACK_TEMP_RESOURCE(hitgroups_shader_table_upload.resource, &command_list);

		g_d3d.command_queue_direct->ExecuteCommandList(command_list);
    }

	void AddShader(RaytracingPipelineParams *params, RaytracingShader *shader)
	{
		if (ALWAYS(params->shader_count < MAX_SHADERS_PER_RAYTRACING_PIPELINE))
		{
			params->shaders[params->shader_count++] = shader;
		}
	}

	void AddHitGroup(RaytracingPipelineParams *params, const HitGroup &hitgroup)
	{
		if (ALWAYS(params->hitgroup_count < MAX_HITGROUPS_PER_RAYTRACING_PIPELINE))
		{
			params->hitgroups[params->hitgroup_count++] = hitgroup;
		}
	}
#endif

	void ReloadAllTheRaytracingStateAsNecessary(bool init)
	{
		// ------------------------------------------------------------------
		// Initialize raytracing shaders

#if RT_DISPATCH_RAYS
		if (init)
		{
			g_d3d.rt_shaders.primary_raygen  = { L"assets/shaders/primary_ray.hlsl", L"PrimaryRaygen", primary_raygen_export_name };
			g_d3d.rt_shaders.primary_closest = { L"assets/shaders/primary_ray.hlsl", L"PrimaryClosesthit", primary_closesthit_export_name };
			g_d3d.rt_shaders.primary_any     = { L"assets/shaders/primary_ray.hlsl", L"PrimaryAnyhit", primary_anyhit_export_name };
			g_d3d.rt_shaders.primary_miss    = { L"assets/shaders/primary_ray.hlsl", L"PrimaryMiss", primary_miss_export_name };

			g_d3d.rt_shaders.direct_raygen   = { L"assets/shaders/direct_lighting.hlsl", L"DirectLightingRaygen", direct_lighting_raygen_export_name };
			g_d3d.rt_shaders.indirect_raygen = { L"assets/shaders/indirect_lighting.hlsl", L"IndirectLightingRaygen", indirect_lighting_raygen_export_name };

			g_d3d.rt_shaders.occlusion_any   = { L"assets/shaders/occlusion.hlsl", L"OcclusionAnyhit", occlusion_anyhit_export_name };
			g_d3d.rt_shaders.occlusion_miss  = { L"assets/shaders/occlusion.hlsl", L"OcclusionMiss", occlusion_miss_export_name };
		}

		
		ReloadRaytracingShaders();

		// ------------------------------------------------------------------
		// Initialize raytracing pipelines

		HitGroup primary_hit_group = {};
		primary_hit_group.closest     = &g_d3d.rt_shaders.primary_closest;
		primary_hit_group.any         = &g_d3d.rt_shaders.primary_any;
		primary_hit_group.export_name = primary_hitgroup_export_name;

		HitGroup occlusion_hit_group = {};
		occlusion_hit_group.any         = &g_d3d.rt_shaders.occlusion_any;
		occlusion_hit_group.export_name = occlusion_hitgroup_export_name;

		RaytracingPipelineParams primary_params = {};
		primary_params.max_payload_size    = sizeof(PrimaryRayPayload);
		primary_params.max_attribute_size  = sizeof(RT_Vec2);
		primary_params.max_recursion_depth = 1;
		AddShader(&primary_params, &g_d3d.rt_shaders.primary_raygen);
		AddShader(&primary_params, &g_d3d.rt_shaders.primary_closest);
		AddShader(&primary_params, &g_d3d.rt_shaders.primary_any);
		AddShader(&primary_params, &g_d3d.rt_shaders.primary_miss);
		AddHitGroup(&primary_params, primary_hit_group);
		ReloadRaytracingPipeline(&g_d3d.rt_pipelines.primary, primary_params);

		RaytracingPipelineParams direct_params = {};
		direct_params.max_payload_size    = sizeof(OcclusionRayPayload);
		direct_params.max_attribute_size  = sizeof(RT_Vec2);
		direct_params.max_recursion_depth = 1;
		AddShader(&direct_params, &g_d3d.rt_shaders.direct_raygen);
		AddShader(&direct_params, &g_d3d.rt_shaders.occlusion_any);
		AddShader(&direct_params, &g_d3d.rt_shaders.occlusion_miss);
		AddHitGroup(&direct_params, occlusion_hit_group);
		ReloadRaytracingPipeline(&g_d3d.rt_pipelines.direct, direct_params);

		RaytracingPipelineParams indirect_params = {};
		indirect_params.max_payload_size    = sizeof(PrimaryRayPayload);
		indirect_params.max_attribute_size  = sizeof(RT_Vec2);
		indirect_params.max_recursion_depth = 1;
		AddShader(&indirect_params, &g_d3d.rt_shaders.indirect_raygen);
		AddShader(&indirect_params, &g_d3d.rt_shaders.primary_closest);
		AddShader(&indirect_params, &g_d3d.rt_shaders.primary_any);
		AddShader(&indirect_params, &g_d3d.rt_shaders.primary_miss);
		AddShader(&indirect_params, &g_d3d.rt_shaders.occlusion_any);
		AddShader(&indirect_params, &g_d3d.rt_shaders.occlusion_miss);
		AddHitGroup(&indirect_params, primary_hit_group);
		AddHitGroup(&indirect_params, occlusion_hit_group);
		ReloadRaytracingPipeline(&g_d3d.rt_pipelines.indirect, indirect_params);

		bool anything_reloaded = false;

		for (size_t i = 0; i < RT_ARRAY_COUNT(g_d3d.rt_shaders_all); i++)
		{
			anything_reloaded |= g_d3d.rt_shaders_all[i].dirty;
			g_d3d.rt_shaders_all[i].dirty = false;
		}

		if (anything_reloaded)
		{
			ReloadShaderTables();
		}
#elif RT_INLINE_RAYTRACING
		(void)init;
		if (!GetLastWriteTime(L"assets/shaders/lock_file.temp"))
		{
			ReloadComputeShader(&g_d3d.rt_shaders.primary_inline, L"assets/shaders/primary_ray_inline.hlsl", L"PrimaryRayInline");
			ReloadComputeShader(&g_d3d.rt_shaders.direct_lighting_inline, L"assets/shaders/direct_lighting_inline.hlsl",
			                    DirectReferenceGroups() ? L"DirectLightingInlineReference" : L"DirectLightingInline");
			ReloadComputeShader(&g_d3d.rt_shaders.indirect_lighting_inline, L"assets/shaders/indirect_lighting_inline.hlsl",
			                    IndirectReferenceGroups() ? L"IndirectLightingInlineReference" : L"IndirectLightingInline");
		}
#endif
	}

	// Creates the rasterization root signature and pipeline state for rendering game UI
	void CreateRasterState()
	{
		D3D12_RENDER_TARGET_BLEND_DESC rt_blend_desc = {};
		rt_blend_desc.BlendEnable = TRUE;
		rt_blend_desc.LogicOpEnable = FALSE;
		rt_blend_desc.SrcBlend = D3D12_BLEND_SRC_ALPHA;
		rt_blend_desc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
		rt_blend_desc.BlendOp = D3D12_BLEND_OP_ADD;
		// Accumulate coverage, rather than squaring alpha, on the VR UI layer.
		rt_blend_desc.SrcBlendAlpha = D3D12_BLEND_ONE;
		rt_blend_desc.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
		rt_blend_desc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		rt_blend_desc.LogicOp = D3D12_LOGIC_OP_NOOP;
		rt_blend_desc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

		{
			// -------------------------------------------------------------------------------------------------
			// Create triangle root sig, state, and buffers

			D3D12_DESCRIPTOR_RANGE1 descriptor_ranges[1] = {};
			descriptor_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			descriptor_ranges[0].BaseShaderRegister = 0;
			descriptor_ranges[0].RegisterSpace = 0;
			descriptor_ranges[0].NumDescriptors = 4096;
			descriptor_ranges[0].OffsetInDescriptorsFromTableStart = 0;
			descriptor_ranges[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

			D3D12_ROOT_PARAMETER1 root_parameters[1] = {};
			root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			root_parameters[0].DescriptorTable.NumDescriptorRanges = RT_ARRAY_COUNT(descriptor_ranges);
			root_parameters[0].DescriptorTable.pDescriptorRanges = &descriptor_ranges[0];
			root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

			D3D12_STATIC_SAMPLER_DESC static_samplers[1] = {};
			static_samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			static_samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			static_samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			static_samplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
			static_samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
			static_samplers[0].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
			static_samplers[0].MaxAnisotropy = 0;
			static_samplers[0].MinLOD = 0;
			static_samplers[0].MaxLOD = 0;
			static_samplers[0].MipLODBias = 0;
			static_samplers[0].ShaderRegister = 0;
			static_samplers[0].RegisterSpace = 0;
			static_samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

			D3D12_VERSIONED_ROOT_SIGNATURE_DESC graphics_root_sig_desc = {};
			graphics_root_sig_desc.Desc_1_1.NumParameters = RT_ARRAY_COUNT(root_parameters);
			graphics_root_sig_desc.Desc_1_1.pParameters = &root_parameters[0];
			graphics_root_sig_desc.Desc_1_1.NumStaticSamplers = RT_ARRAY_COUNT(static_samplers);
			graphics_root_sig_desc.Desc_1_1.pStaticSamplers = &static_samplers[0];
			graphics_root_sig_desc.Desc_1_1.Flags =
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
			graphics_root_sig_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;

			ID3DBlob* serialized_root_sig = CompileVersionedRootSignature(graphics_root_sig_desc);
			DX_CALL(g_d3d.device->CreateRootSignature(0, serialized_root_sig->GetBufferPointer(), serialized_root_sig->GetBufferSize(), IID_PPV_ARGS(&g_d3d_raster.tri_root_sig)));
			g_d3d_raster.tri_root_sig->SetName(L"Raster triangle root signature");
			SafeRelease(serialized_root_sig);

			// Create raster pipeline state
			D3D12_INPUT_ELEMENT_DESC input_elements[] = {
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "TEXTURE_INDEX", 0, DXGI_FORMAT_R32_UINT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
			};

			IDxcBlob* vertex_shader_blob = CompileShader(L"assets/shaders/raster_tri.hlsl", L"VertexShaderEntry", L"vs_6_3");
			IDxcBlob* pixel_shader_blob = CompileShader(L"assets/shaders/raster_tri.hlsl", L"PixelShaderEntry", L"ps_6_3");

			D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics_state_desc = {};
			graphics_state_desc.InputLayout.NumElements = RT_ARRAY_COUNT(input_elements);
			graphics_state_desc.InputLayout.pInputElementDescs = &input_elements[0];
			graphics_state_desc.VS.BytecodeLength = vertex_shader_blob->GetBufferSize();
			graphics_state_desc.VS.pShaderBytecode = vertex_shader_blob->GetBufferPointer();
			graphics_state_desc.PS.BytecodeLength = pixel_shader_blob->GetBufferSize();
			graphics_state_desc.PS.pShaderBytecode = pixel_shader_blob->GetBufferPointer();
			graphics_state_desc.NumRenderTargets = 1;
			graphics_state_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
			graphics_state_desc.DepthStencilState.DepthEnable = FALSE;
			graphics_state_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_NEVER;
			graphics_state_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
			graphics_state_desc.DepthStencilState.StencilEnable = FALSE;
			graphics_state_desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
			graphics_state_desc.BlendState.AlphaToCoverageEnable = FALSE;
			graphics_state_desc.BlendState.IndependentBlendEnable = FALSE;
			graphics_state_desc.BlendState.RenderTarget[0] = rt_blend_desc;
			graphics_state_desc.SampleDesc.Count = 1;
			graphics_state_desc.SampleDesc.Quality = 0;
			graphics_state_desc.SampleMask = UINT_MAX;
			graphics_state_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			graphics_state_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
			graphics_state_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			graphics_state_desc.NodeMask = 0;
			graphics_state_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
			graphics_state_desc.pRootSignature = g_d3d_raster.tri_root_sig;

			DX_CALL(g_d3d.device->CreateGraphicsPipelineState(&graphics_state_desc, IID_PPV_ARGS(&g_d3d_raster.tri_state)));
			g_d3d_raster.tri_state->SetName(L"Raster triangle state (Non-SRGB render target)");

			SafeRelease(vertex_shader_blob);
			SafeRelease(pixel_shader_blob);

			// XY position + tex coords + color
			g_d3d_raster.tri_vertex_buffer = RT_CreateUploadBuffer(L"Raster triangle vertex buffer", BACK_BUFFER_COUNT * MAX_RASTER_TRIANGLES * 3 * sizeof(RT_RasterTriVertex));
			char* ui_vert_buf_ptr;
			g_d3d_raster.tri_vertex_buffer->Map(0, nullptr, reinterpret_cast<void**>(&ui_vert_buf_ptr));
			g_d3d_raster.tri_vertex_buf_ptr = reinterpret_cast<RT_RasterTriVertex*>(ui_vert_buf_ptr);
		}

		{
			// -------------------------------------------------------------------------------------------------
			// Create line root sig, state, and buffers

			D3D12_VERSIONED_ROOT_SIGNATURE_DESC graphics_root_sig_desc = {};
			graphics_root_sig_desc.Desc_1_1.NumParameters = 0;
			graphics_root_sig_desc.Desc_1_1.pParameters = nullptr;
			graphics_root_sig_desc.Desc_1_1.NumStaticSamplers = 0;
			graphics_root_sig_desc.Desc_1_1.pStaticSamplers = nullptr;
			graphics_root_sig_desc.Desc_1_1.Flags =
				D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
				D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
				D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
				D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
			graphics_root_sig_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;

			ID3DBlob* serialized_root_sig = CompileVersionedRootSignature(graphics_root_sig_desc);
			DX_CALL(g_d3d.device->CreateRootSignature(0, serialized_root_sig->GetBufferPointer(), serialized_root_sig->GetBufferSize(), IID_PPV_ARGS(&g_d3d_raster.line_root_sig)));
			g_d3d_raster.line_root_sig->SetName(L"Raster line root signature");
			SafeRelease(serialized_root_sig);

			// Create raster pipeline state
			D3D12_INPUT_ELEMENT_DESC input_elements[] = {
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			};

			IDxcBlob* vertex_shader_blob = CompileShader(L"assets/shaders/raster_line.hlsl", L"VertexShaderEntry", L"vs_6_3");
			IDxcBlob* pixel_shader_blob = CompileShader(L"assets/shaders/raster_line.hlsl", L"PixelShaderEntry", L"ps_6_3");

			D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics_state_desc = {};
			graphics_state_desc.InputLayout.NumElements = RT_ARRAY_COUNT(input_elements);
			graphics_state_desc.InputLayout.pInputElementDescs = &input_elements[0];
			graphics_state_desc.VS.BytecodeLength = vertex_shader_blob->GetBufferSize();
			graphics_state_desc.VS.pShaderBytecode = vertex_shader_blob->GetBufferPointer();
			graphics_state_desc.PS.BytecodeLength = pixel_shader_blob->GetBufferSize();
			graphics_state_desc.PS.pShaderBytecode = pixel_shader_blob->GetBufferPointer();
			graphics_state_desc.NumRenderTargets = 1;
			graphics_state_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
			graphics_state_desc.DepthStencilState.DepthEnable = FALSE;
			graphics_state_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_NEVER;
			graphics_state_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
			graphics_state_desc.DepthStencilState.StencilEnable = FALSE;
			graphics_state_desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
			graphics_state_desc.BlendState.AlphaToCoverageEnable = FALSE;
			graphics_state_desc.BlendState.IndependentBlendEnable = FALSE;
			graphics_state_desc.BlendState.RenderTarget[0] = rt_blend_desc;
			graphics_state_desc.SampleDesc.Count = 1;
			graphics_state_desc.SampleDesc.Quality = 0;
			graphics_state_desc.SampleMask = UINT_MAX;
			graphics_state_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			graphics_state_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
			graphics_state_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
			graphics_state_desc.NodeMask = 0;
			graphics_state_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
			graphics_state_desc.pRootSignature = g_d3d_raster.line_root_sig;

			DX_CALL(g_d3d.device->CreateGraphicsPipelineState(&graphics_state_desc, IID_PPV_ARGS(&g_d3d_raster.line_state)));
			g_d3d_raster.line_state->SetName(L"Raster line graphics pipeline state");

			SafeRelease(vertex_shader_blob);
			SafeRelease(pixel_shader_blob);

			g_d3d_raster.line_vertex_buffer = RT_CreateUploadBuffer(L"Raster line vertex buffer", BACK_BUFFER_COUNT * MAX_RASTER_LINES * 2 * sizeof(RT_RasterLineVertex));
			char* line_buf_ptr;
			g_d3d_raster.line_vertex_buffer->Map(0, nullptr, reinterpret_cast<void**>(&line_buf_ptr));
			g_d3d_raster.line_vertex_buf_ptr = reinterpret_cast<RT_RasterLineVertex*>(line_buf_ptr);
		}

		{
			// -------------------------------------------------------------------------------------------------
			// Create debug line root sig, state, and buffers

			D3D12_ROOT_PARAMETER1 root_parameters[2] = {};
			root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			root_parameters[0].Constants.Num32BitValues = 16;
			root_parameters[0].Constants.ShaderRegister = 0;
			root_parameters[0].Constants.RegisterSpace = 0;
			root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

			root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			root_parameters[1].Constants.Num32BitValues = 1;
			root_parameters[1].Constants.ShaderRegister = 1;
			root_parameters[1].Constants.RegisterSpace = 0;
			root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

			D3D12_VERSIONED_ROOT_SIGNATURE_DESC graphics_root_sig_desc = {};
			graphics_root_sig_desc.Desc_1_1.NumParameters = RT_ARRAY_COUNT(root_parameters);
			graphics_root_sig_desc.Desc_1_1.pParameters = root_parameters;
			graphics_root_sig_desc.Desc_1_1.NumStaticSamplers = 0;
			graphics_root_sig_desc.Desc_1_1.pStaticSamplers = nullptr;
			graphics_root_sig_desc.Desc_1_1.Flags =
				D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
				D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
				D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
				D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
			graphics_root_sig_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;

			ID3DBlob* serialized_root_sig = CompileVersionedRootSignature(graphics_root_sig_desc);
			DX_CALL(g_d3d.device->CreateRootSignature(0, serialized_root_sig->GetBufferPointer(), serialized_root_sig->GetBufferSize(), IID_PPV_ARGS(&g_d3d_raster.debug_line_root_sig)));
			g_d3d_raster.debug_line_root_sig->SetName(L"Raster debug line root signature");
			SafeRelease(serialized_root_sig);

			// Create raster pipeline state
			D3D12_INPUT_ELEMENT_DESC input_elements[] = {
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			};

			IDxcBlob* vertex_shader_blob = CompileShader(L"assets/shaders/raster_line_world.hlsl", L"VertexShaderEntry", L"vs_6_3");
			IDxcBlob* pixel_shader_blob = CompileShader(L"assets/shaders/raster_line_world.hlsl", L"PixelShaderEntry", L"ps_6_3");

			D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics_state_desc = {};
			graphics_state_desc.InputLayout.NumElements = RT_ARRAY_COUNT(input_elements);
			graphics_state_desc.InputLayout.pInputElementDescs = &input_elements[0];
			graphics_state_desc.VS.BytecodeLength = vertex_shader_blob->GetBufferSize();
			graphics_state_desc.VS.pShaderBytecode = vertex_shader_blob->GetBufferPointer();
			graphics_state_desc.PS.BytecodeLength = pixel_shader_blob->GetBufferSize();
			graphics_state_desc.PS.pShaderBytecode = pixel_shader_blob->GetBufferPointer();
			graphics_state_desc.NumRenderTargets = 1;
			graphics_state_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
			graphics_state_desc.DepthStencilState.DepthEnable = FALSE;
			graphics_state_desc.DepthStencilState.StencilEnable = FALSE;
			graphics_state_desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
			graphics_state_desc.BlendState.AlphaToCoverageEnable = FALSE;
			graphics_state_desc.BlendState.IndependentBlendEnable = FALSE;
			graphics_state_desc.BlendState.RenderTarget[0] = rt_blend_desc;
			graphics_state_desc.SampleDesc.Count = 1;
			graphics_state_desc.SampleDesc.Quality = 0;
			graphics_state_desc.SampleMask = UINT_MAX;
			graphics_state_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			graphics_state_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
			graphics_state_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
			graphics_state_desc.NodeMask = 0;
			graphics_state_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
			graphics_state_desc.pRootSignature = g_d3d_raster.debug_line_root_sig;

			DX_CALL(g_d3d.device->CreateGraphicsPipelineState(&graphics_state_desc, IID_PPV_ARGS(&g_d3d_raster.debug_line_state)));
			g_d3d_raster.debug_line_state->SetName(L"Raster debug line graphics pipeline state");

			SafeRelease(vertex_shader_blob);
			SafeRelease(pixel_shader_blob);

			const DxcDefine defines[1] =
			{
				{ L"DEPTH_ENABLED" },
			};

			vertex_shader_blob = CompileShader(L"assets/shaders/raster_line_world.hlsl", L"VertexShaderEntry", L"vs_6_3", RT_ARRAY_COUNT(defines), defines);
			pixel_shader_blob = CompileShader(L"assets/shaders/raster_line_world.hlsl", L"PixelShaderEntry", L"ps_6_3", RT_ARRAY_COUNT(defines), defines);

			graphics_state_desc.DepthStencilState.DepthEnable = TRUE;
			graphics_state_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
			graphics_state_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
			graphics_state_desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

			graphics_state_desc.VS.BytecodeLength = vertex_shader_blob->GetBufferSize();
			graphics_state_desc.VS.pShaderBytecode = vertex_shader_blob->GetBufferPointer();
			graphics_state_desc.PS.BytecodeLength = pixel_shader_blob->GetBufferSize();
			graphics_state_desc.PS.pShaderBytecode = pixel_shader_blob->GetBufferPointer();

			DX_CALL(g_d3d.device->CreateGraphicsPipelineState(&graphics_state_desc, IID_PPV_ARGS(&g_d3d_raster.debug_line_state_depth)));
			g_d3d_raster.debug_line_state_depth->SetName(L"Raster debug line graphics pipeline state depth");

			SafeRelease(vertex_shader_blob);
			SafeRelease(pixel_shader_blob);

			g_d3d_raster.debug_line_vertex_buffer = RT_CreateUploadBuffer(L"Debug line vertex buffer", BACK_BUFFER_COUNT * MAX_DEBUG_LINES_WORLD * 2 * sizeof(RT_RasterLineVertex));
			char* debug_line_vertex_buf_ptr;
			g_d3d_raster.debug_line_vertex_buffer->Map(0, nullptr, reinterpret_cast<void**>(&debug_line_vertex_buf_ptr));
			g_d3d_raster.debug_line_vertex_buf_ptr = reinterpret_cast<RT_RasterLineVertex*>(debug_line_vertex_buf_ptr);
		}


		{
			// -------------------------------------------------------------------------------------------------
			// Create raster blit root signature and pipeline state

			D3D12_DESCRIPTOR_RANGE1 ranges[1] = {};
			ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			ranges[0].BaseShaderRegister = 0;
			ranges[0].RegisterSpace = 0;
			ranges[0].NumDescriptors = 1;
			ranges[0].OffsetInDescriptorsFromTableStart = 0;
			ranges[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;

			D3D12_ROOT_PARAMETER1 root_parameters[2] = {};
			root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			root_parameters[0].Constants.Num32BitValues = 7;
			root_parameters[0].Constants.ShaderRegister = 0;
			root_parameters[0].Constants.RegisterSpace = 0;
			root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

			root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE	;
			root_parameters[1].DescriptorTable.NumDescriptorRanges = 1;
			root_parameters[1].DescriptorTable.pDescriptorRanges = ranges;
			root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			
			D3D12_STATIC_SAMPLER_DESC static_samplers[1] = {};
			static_samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			static_samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			static_samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			static_samplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
			static_samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
			static_samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			static_samplers[0].MaxAnisotropy = 0;
			static_samplers[0].MinLOD = 0;
			static_samplers[0].MaxLOD = 0;
			static_samplers[0].MipLODBias = 0;
			static_samplers[0].ShaderRegister = 0;
			static_samplers[0].RegisterSpace = 0;
			static_samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

			D3D12_VERSIONED_ROOT_SIGNATURE_DESC graphics_root_sig_desc = {};
			graphics_root_sig_desc.Desc_1_1.NumParameters = RT_ARRAY_COUNT(root_parameters);
			graphics_root_sig_desc.Desc_1_1.pParameters = root_parameters;
			graphics_root_sig_desc.Desc_1_1.NumStaticSamplers = RT_ARRAY_COUNT(static_samplers);
			graphics_root_sig_desc.Desc_1_1.pStaticSamplers = static_samplers;
			graphics_root_sig_desc.Desc_1_1.Flags =
				D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
				D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
				D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
				D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
			graphics_root_sig_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;

			ID3DBlob* serialized_root_sig = CompileVersionedRootSignature(graphics_root_sig_desc);
			DX_CALL(g_d3d.device->CreateRootSignature(0, serialized_root_sig->GetBufferPointer(), serialized_root_sig->GetBufferSize(), IID_PPV_ARGS(&g_d3d_raster.blit_root_sig)));
			g_d3d_raster.blit_root_sig->SetName(L"Raster blit root signature");
			SafeRelease(serialized_root_sig);

			IDxcBlob* vertex_shader_blob = CompileShader(L"assets/shaders/raster_blit.hlsl", L"VertexShaderEntry", L"vs_6_3");
			IDxcBlob* pixel_shader_blob = CompileShader(L"assets/shaders/raster_blit.hlsl", L"PixelShaderEntry", L"ps_6_3");

			D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics_state_desc = {};
			graphics_state_desc.InputLayout.NumElements = 0;
			graphics_state_desc.InputLayout.pInputElementDescs = nullptr;
			graphics_state_desc.VS.BytecodeLength = vertex_shader_blob->GetBufferSize();
			graphics_state_desc.VS.pShaderBytecode = vertex_shader_blob->GetBufferPointer();
			graphics_state_desc.PS.BytecodeLength = pixel_shader_blob->GetBufferSize();
			graphics_state_desc.PS.pShaderBytecode = pixel_shader_blob->GetBufferPointer();
			graphics_state_desc.NumRenderTargets = 1;
			graphics_state_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
			graphics_state_desc.DepthStencilState.DepthEnable = FALSE;
			graphics_state_desc.DepthStencilState.StencilEnable = FALSE;
			graphics_state_desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
			graphics_state_desc.BlendState.AlphaToCoverageEnable = FALSE;
			graphics_state_desc.BlendState.IndependentBlendEnable = FALSE;
			graphics_state_desc.BlendState.RenderTarget[0] = rt_blend_desc;
			graphics_state_desc.SampleDesc.Count = 1;
			graphics_state_desc.SampleDesc.Quality = 0;
			graphics_state_desc.SampleMask = UINT_MAX;
			graphics_state_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			graphics_state_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
			graphics_state_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			graphics_state_desc.NodeMask = 0;
			graphics_state_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
			graphics_state_desc.pRootSignature = g_d3d_raster.blit_root_sig;

			DX_CALL(g_d3d.device->CreateGraphicsPipelineState(&graphics_state_desc, IID_PPV_ARGS(&g_d3d_raster.blit_state)));
			g_d3d_raster.blit_state->SetName(L"Raster blit graphics pipeline state");

			SafeRelease(vertex_shader_blob);
			SafeRelease(pixel_shader_blob);
		}

		RenderBackend::RasterSetViewport(0.0f, 0.0f, (float)g_d3d.render_width, (float)g_d3d.render_height);
	}

	void CreateRasterDepthTarget()
	{
		D3D12_CLEAR_VALUE clear_value = {};
		clear_value.Format = DXGI_FORMAT_D32_FLOAT;
		clear_value.DepthStencil.Depth = 1.0f;
		clear_value.DepthStencil.Stencil = 0;

		g_d3d_raster.depth_target = RT_CreateTexture(L"Raster depth target", DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
			g_d3d.render_width, g_d3d.render_height, D3D12_RESOURCE_STATE_DEPTH_WRITE, 1, &clear_value);
		CreateTextureDSV(g_d3d_raster.depth_target, g_d3d_raster.depth_target_dsv, DXGI_FORMAT_D32_FLOAT);
	}

	void CreateMaterialBuffers()
	{
		g_d3d.material_buffer = RT_CreateUploadBuffer(L"Material Buffer", sizeof(Material)*RT_MAX_MATERIALS);
		DX_CALL(g_d3d.material_buffer->Map(0, nullptr, reinterpret_cast<void **>(&g_d3d.material_buffer_cpu)));

		g_d3d.material_edges = RT_CreateReadOnlyBuffer(L"Material-edge Buffer", sizeof(RT_MaterialEdge) * RT_MAX_MATERIAL_EDGES);
		g_d3d.material_indices = RT_CreateReadOnlyBuffer(L"Material Index Buffer", sizeof(uint16_t) * RT_MAX_MATERIALS);
	}

    void CreateDxcCompilerState()
    {
		DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&g_d3d.dxc_compiler));
		DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&g_d3d.dxc_utils));
		g_d3d.dxc_utils->CreateDefaultIncludeHandler(&g_d3d.dxc_include_handler);
    }

	D3D12_RESOURCE_BARRIER AliasingBarrier(ID3D12Resource* resource_before, ID3D12Resource* resource_after)
	{
		D3D12_RESOURCE_BARRIER barrier = {};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
		barrier.Aliasing.pResourceBefore = resource_before;
		barrier.Aliasing.pResourceAfter = resource_after;

		return barrier;
	}

	uint32_t DivideUp(uint32_t x, uint32_t div)
	{
		return (x + div - 1) / div;
	}

	void GenerateMips(TextureResource* resource)
	{
		if (!resource->texture || resource->texture->GetDesc().MipLevels == 1)
			return;

		CommandList& command_list = *g_d3d.resource_upload_ring_buffer.command_list;

		ID3D12Resource* uav_resource = resource->texture;
		ID3D12Resource* aliased_resource = nullptr;

		// If the source texture does not allow unordered access, we will alias the source texture with another texture
		if ((resource->texture->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0)
		{
			D3D12_RESOURCE_DESC alias_resource_desc = resource->texture->GetDesc();
			alias_resource_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
			D3D12_RESOURCE_DESC uav_desc = alias_resource_desc;
			D3D12_RESOURCE_DESC alias_uav_descs[] = { alias_resource_desc, uav_desc };

			D3D12_RESOURCE_ALLOCATION_INFO alloc_info = g_d3d.device->GetResourceAllocationInfo(0, RT_ARRAY_COUNT(alias_uav_descs), alias_uav_descs);
			
			D3D12_HEAP_DESC heap_desc = {};
			heap_desc.SizeInBytes = alloc_info.SizeInBytes;
			heap_desc.Alignment = alloc_info.Alignment;
			heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
			heap_desc.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
			heap_desc.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
			heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;

			ID3D12Heap* heap;
			DX_CALL(g_d3d.device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)));
			heap->SetName(L"Temporary mipmap gen heap");
			RT_TRACK_TEMP_OBJECT(heap, &command_list);

			DX_CALL(g_d3d.device->CreatePlacedResource(
				heap, 0, &alias_resource_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&aliased_resource)
			));
			aliased_resource->SetName(L"Temporary aliasing resource mipmap gen");
			RT_TRACK_TEMP_RESOURCE(aliased_resource, &command_list);

			DX_CALL(g_d3d.device->CreatePlacedResource(
				heap, 0, &uav_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&uav_resource)
			));
			uav_resource->SetName(L"Temporary UAV resource mipmap gen");
			RT_TRACK_TEMP_RESOURCE(uav_resource, &command_list);

			D3D12_RESOURCE_BARRIER alias_barrier_before = AliasingBarrier(nullptr, aliased_resource);
			command_list->ResourceBarrier(1, &alias_barrier_before);
			CopyResource(command_list, aliased_resource, resource->texture);
			D3D12_RESOURCE_BARRIER alias_barrier_after = AliasingBarrier(aliased_resource, uav_resource);
			command_list->ResourceBarrier(1, &alias_barrier_after);
		}

		// TODO(Justin): These are temporary descriptor allocations and should ideally be freed when the mips are done
		// However, since descriptor heaps can be quite big in size, we won't care about this.
		DescriptorAllocation srv = g_d3d.cbv_srv_uav_mips.Allocate(1);
		DescriptorAllocation uavs = g_d3d.cbv_srv_uav_mips.Allocate(resource->texture->GetDesc().MipLevels - 1);

		command_list->SetPipelineState(g_d3d.cs.gen_mipmaps.pso);
		command_list->SetComputeRootSignature(g_d3d.gen_mipmap_root_sig);

		GenMipMapSettings gen_mipmap_settings = {};
		gen_mipmap_settings.is_srgb = resource->texture->GetDesc().Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

		for (uint32_t src_mip = 0; src_mip < ((uint32_t)resource->texture->GetDesc().MipLevels - 1);)
		{
			uint32_t src_width = (uint32_t)resource->texture->GetDesc().Width >> src_mip;
			uint32_t src_height = resource->texture->GetDesc().Height >> src_mip;
			uint32_t dst_width = uint32_t(src_width >> 1);
			uint32_t dst_height = src_height >> 1;
			
			gen_mipmap_settings.src_dim = (src_height & 1) << 1 | (src_width & 1);
			DWORD mip_count = 0;

			// Determine how many mips we want to generate now (max of 4)
			_BitScanForward(&mip_count, (dst_width == 1 ? dst_width : dst_height) |
				(dst_height == 1 ? dst_width : dst_height));

			mip_count = RT_MIN(4, mip_count + 1);
			mip_count = (src_mip + mip_count) >= resource->texture->GetDesc().MipLevels ?
				resource->texture->GetDesc().MipLevels - src_mip - 1 : mip_count;

			dst_width = RT_MAX(1u, dst_width);
			dst_height = RT_MAX(1u, dst_height);

			gen_mipmap_settings.src_mip = src_mip;
			gen_mipmap_settings.num_mips = mip_count;
			gen_mipmap_settings.texel_size.x = 1.0f / (float)dst_width;
			gen_mipmap_settings.texel_size.y = 1.0f / (float)dst_height;

			D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
			srv_desc.Format = resource->texture->GetDesc().Format;
			srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv_desc.Texture2D.MostDetailedMip = 0;
			srv_desc.Texture2D.MipLevels = resource->texture->GetDesc().MipLevels;

			g_d3d.device->CreateShaderResourceView(uav_resource, &srv_desc, srv.cpu);
			CreateTextureSRV(uav_resource, srv.cpu, resource->texture->GetDesc().Format);
			for (uint32_t mip = 0; mip < mip_count; ++mip)
			{
				D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
				uav_desc.Format = resource->texture->GetDesc().Format == DXGI_FORMAT_R8_UNORM ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
				uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
				uav_desc.Texture2D.MipSlice = src_mip + mip + 1;

				g_d3d.device->CreateUnorderedAccessView(uav_resource, nullptr, &uav_desc, uavs.GetCPUDescriptor(src_mip + mip));
			}

			ID3D12DescriptorHeap* heap = { g_d3d.cbv_srv_uav_mips.GetHeap() };
			command_list->SetDescriptorHeaps(1, &heap);
			// Set the constant buffer data for the mip map settings constant buffer
			command_list->SetComputeRoot32BitConstants(0, sizeof(GenMipMapSettings) / 4, &gen_mipmap_settings, 0);
			// Set both descriptor tables for the source texture SRV and the target texture UAVs
			command_list->SetComputeRootDescriptorTable(1, srv.GetGPUDescriptor(0));
			command_list->SetComputeRootDescriptorTable(2, uavs.GetGPUDescriptor(src_mip));

			command_list->Dispatch(DivideUp(dst_width, 8), DivideUp(dst_height, 8), 1);
			UAVBarrier(command_list, uav_resource);

			src_mip += mip_count;
		}

		if (aliased_resource)
		{
			D3D12_RESOURCE_BARRIER barriers[] = {
				AliasingBarrier(uav_resource, aliased_resource),
				GetTransitionBarrier(aliased_resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE),
				GetTransitionBarrier(resource->texture, g_d3d.resource_tracker.GetResourceState(resource->texture), D3D12_RESOURCE_STATE_COPY_DEST)
			};
			command_list->ResourceBarrier(3, barriers);
			command_list->CopyResource(resource->texture, aliased_resource);

			if (g_d3d.resource_tracker.GetResourceState(resource->texture) != D3D12_RESOURCE_STATE_COPY_DEST)
			{
				D3D12_RESOURCE_BARRIER barrier = GetTransitionBarrier(resource->texture,
					D3D12_RESOURCE_STATE_COPY_DEST, g_d3d.resource_tracker.GetResourceState(resource->texture));
				command_list->ResourceBarrier(1, &barrier);
			}
		}
	}

    void CreateIntermediateRendertargets()
    {
		auto CreateRenderTarget = [](const wchar_t *name, int reg, int scale_x, int scale_y, bool output_res, DXGI_FORMAT format, ID3D12Resource **result)
		{
			(void)reg;
			uint32_t width = (g_d3d.render_width + scale_x - 1) / scale_x;
			uint32_t height = (g_d3d.render_height + scale_y - 1) / scale_y;

			if (g_d3d.upscaling_aa_mode == UPSCALING_AA_MODE_AMD_FSR_2_2 && output_res)
			{
				width = (g_d3d.output_width + scale_x - 1) / scale_x;
				height = (g_d3d.output_height + scale_y - 1) / scale_y;
			}

			(*result) = RT_CreateTexture(name, format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
				, width, height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		};

#define RT_CREATE_RENDER_TARGETS(name, reg, scale_x, scale_y, output_res, type, format) \
		CreateRenderTarget(RT_PASTE(L, #name), reg, scale_x, scale_y, output_res, format, &g_d3d.render_targets[RT_PASTE(RenderTarget_, name)]); \
		g_d3d.render_target_formats[RT_PASTE(RenderTarget_, name)] = format; \
		CreateRenderTarget(RT_PASTE(L, #name) L"_blit", reg, scale_x * 4, scale_y * 4, true, format, &g_d3d.render_targets_blit[RT_PASTE(RenderTarget_, name)]); \
		CreateRenderTarget(RT_PASTE(L, #name) L"_blit_b", reg, scale_x * 4, scale_y * 4, true, format, &g_camera_blit_targets_b[RT_PASTE(RenderTarget_, name)]); \

		RT_RENDER_TARGETS(RT_CREATE_RENDER_TARGETS)

		CreateTextureRTV(g_d3d.rt.color_final, g_d3d.color_final_rtv.cpu, g_d3d.render_target_formats[RenderTarget_color_final]);
#ifdef D2_ENABLE_OPENVR
        if(OpenVRPrototype::FixedMirrorCanvas()) {
            if(g_d3d.spectator_source.IsNull()) g_d3d.spectator_source=g_d3d.cbv_srv_uav.Allocate(1);
            CreateTextureSRV(g_d3d.rt.color_final,g_d3d.spectator_source.cpu,DXGI_FORMAT_R8G8B8A8_UNORM);
        }
#endif
	}

	void InitDearImGui()
	{
		size_t descriptor_increment_size = g_d3d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		D3D12_CPU_DESCRIPTOR_HANDLE font_srv_cpu = { g_d3d.cbv_srv_uav.GetCPUBase().ptr + D3D12GlobalDescriptors_SRV_ImGui * descriptor_increment_size };
		D3D12_GPU_DESCRIPTOR_HANDLE font_srv_gpu = { g_d3d.cbv_srv_uav.GetGPUBase().ptr + D3D12GlobalDescriptors_SRV_ImGui * descriptor_increment_size };

		ImGui_ImplDX12_Init(g_d3d.device, 3, DXGI_FORMAT_R8G8B8A8_UNORM, g_d3d.cbv_srv_uav.GetHeap(), font_srv_cpu, font_srv_gpu);
	}

	void CreateQueryHeapAndBuffer()
	{
		D3D12_QUERY_HEAP_DESC query_heap_desc = {};
		query_heap_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
		query_heap_desc.Count = GPUProfiler::GPUTimer_NumTimers * 2 * BACK_BUFFER_COUNT;
		query_heap_desc.NodeMask = 0;
		DX_CALL(g_d3d.device->CreateQueryHeap(&query_heap_desc, IID_PPV_ARGS(&g_d3d.query_heap)));

		g_d3d.query_readback_buffer = RT_CreateReadbackBuffer(L"Timestamp query readback buffer", query_heap_desc.Count * sizeof(uint64_t));
	}

	void UploadBufferData(ID3D12GraphicsCommandList *command_list, size_t size, size_t align, void *src_data, ID3D12Resource *dst_resource, size_t dst_offset)
	{
		FrameData *frame = CurrentFrameData();

		D3D12_RESOURCE_STATES original_state = g_d3d.resource_tracker.GetResourceState(dst_resource);
		ResourceTransition(command_list, dst_resource, D3D12_RESOURCE_STATE_COPY_DEST);

		BufferAllocation upload_buffer_allocation = CopyIntoUploadBuffer(frame, (char *)src_data, size, align);
		command_list->CopyBufferRegion(dst_resource, dst_offset, upload_buffer_allocation.buffer, upload_buffer_allocation.offset, size);

		ResourceTransition(command_list, dst_resource, original_state);
	}

	struct CreateMeshBuffersResult
	{
		ID3D12Resource *triangle_buffer;
		DescriptorAllocation triangle_buffer_descriptor;
	};

	void CreateMeshBuffers(size_t triangle_count, RT_Triangle *triangles, CreateMeshBuffersResult *result)
	{
		RT_ZERO_STRUCT(result);

		size_t triangle_buffer_size = sizeof(RT_Triangle)*triangle_count;
		ID3D12Resource *triangle_buffer = RT_CreateReadOnlyBuffer(L"Triangle Buffer", triangle_buffer_size);

		RingBufferAllocation ring_buf_alloc = WriteToRingBuffer(&g_d3d.resource_upload_ring_buffer, triangle_buffer_size, alignof(RT_Triangle), triangles);
		CommandList& command_list = *ring_buf_alloc.command_list;
		CopyBufferRegion(command_list, triangle_buffer, 0, ring_buf_alloc.resource, ring_buf_alloc.byte_offset, triangle_buffer_size);

		DescriptorAllocation descriptor = g_d3d.cbv_srv_uav.Allocate(1);
		auto vertices_srv = descriptor.GetCPUDescriptor(0);
		CreateBufferSRV(triangle_buffer, vertices_srv, 0, (uint32_t)triangle_count, sizeof(RT_Triangle));

		result->triangle_buffer = triangle_buffer;
		result->triangle_buffer_descriptor = descriptor;
	}

	ID3D12Resource* BuildBLAS(size_t triangle_count, RT_Triangle *triangles, D3D12_RAYTRACING_GEOMETRY_FLAGS flags)
	{
		size_t vertices_size = triangle_count*3*sizeof(RT_Vec3);
		RingBufferAllocation ring_buf_alloc = AllocateFromRingBuffer(&g_d3d.resource_upload_ring_buffer, vertices_size, 4);

		RT_Vec3 *vertices = (RT_Vec3*)ring_buf_alloc.ptr;
		for (size_t i = 0; i < triangle_count; i++)
		{
			vertices[3*i + 0] = triangles[i].pos0;
			vertices[3*i + 1] = triangles[i].pos1;
			vertices[3*i + 2] = triangles[i].pos2;
		}

		D3D12_RAYTRACING_GEOMETRY_DESC geometry = {};
		geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
		geometry.Triangles.VertexBuffer.StartAddress  = ring_buf_alloc.resource->GetGPUVirtualAddress() + ring_buf_alloc.byte_offset;
		geometry.Triangles.VertexBuffer.StrideInBytes = sizeof(RT_Vec3);
		geometry.Triangles.VertexCount                = (UINT)triangle_count*3;
		geometry.Triangles.VertexFormat               = DXGI_FORMAT_R32G32B32_FLOAT;
		geometry.Flags = flags;

		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS build_flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS as_inputs = {};
		as_inputs.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
		as_inputs.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
		as_inputs.pGeometryDescs = &geometry;
		as_inputs.NumDescs       = 1;
		as_inputs.Flags          = build_flags;

		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO ASPreBuildInfo = {};
		g_d3d.device->GetRaytracingAccelerationStructurePrebuildInfo(&as_inputs, &ASPreBuildInfo);

		ASPreBuildInfo.ScratchDataSizeInBytes = RT_ALIGN_POW2(ASPreBuildInfo.ScratchDataSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
		ASPreBuildInfo.ResultDataMaxSizeInBytes = RT_ALIGN_POW2(ASPreBuildInfo.ResultDataMaxSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);

		// Bottom level scratch buffer will not be tracked as persistent, instead it will be tracked as a temporary buffer using the fence value
		ID3D12Resource* bottom_level_scratch = RT_CreateReadWriteBuffer(L"BLAS scratch", ASPreBuildInfo.ScratchDataSizeInBytes);
		ID3D12Resource* bottom_level_as = RT_CreateAccelerationStructureBuffer(L"BLAS", ASPreBuildInfo.ResultDataMaxSizeInBytes);

		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build_desc = {};
		build_desc.Inputs = as_inputs;
		build_desc.ScratchAccelerationStructureData = bottom_level_scratch->GetGPUVirtualAddress();
		build_desc.DestAccelerationStructureData = bottom_level_as->GetGPUVirtualAddress();

		CommandList& command_list = *ring_buf_alloc.command_list;
		command_list->BuildRaytracingAccelerationStructure(&build_desc, 0, nullptr);

		UAVBarrier(command_list, bottom_level_as);
		RT_TRACK_TEMP_RESOURCE(bottom_level_scratch, &command_list);

		return bottom_level_as;
	}

	void BuildTLAS(CommandList &command_list)
	{
		FrameData *frame = CurrentFrameData();

		if (!g_d3d.scene.freezeframe && g_d3d.tlas_instance_count > 0)
			CopyBufferRegion(command_list, g_d3d.tlas_instance_buffer, 0, frame->instance_descs.buffer, frame->instance_descs.offset, sizeof(D3D12_RAYTRACING_INSTANCE_DESC)*g_d3d.tlas_instance_count);

		// This should simply take the mapped instances from the tlas_instance_buffer
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlas_inputs = {};
		tlas_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
		tlas_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		tlas_inputs.InstanceDescs = g_d3d.tlas_instance_buffer->GetGPUVirtualAddress();
		tlas_inputs.NumDescs = g_d3d.tlas_instance_count;
		tlas_inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;

		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlas_prebuild_info = {};
		g_d3d.device->GetRaytracingAccelerationStructurePrebuildInfo(&tlas_inputs, &tlas_prebuild_info);

		if (tlas_prebuild_info.ResultDataMaxSizeInBytes > frame->tlas_size)
		{
			RT_RELEASE_RESOURCE(frame->top_level_as);
			frame->tlas_size = tlas_prebuild_info.ResultDataMaxSizeInBytes;
			frame->top_level_as = RT_CreateAccelerationStructureBuffer(L"TLAS", frame->tlas_size);
		}

		if (tlas_prebuild_info.ScratchDataSizeInBytes > frame->tlas_scratch_size)
		{
			RT_RELEASE_RESOURCE(frame->top_level_as_scratch);
			frame->tlas_scratch_size = tlas_prebuild_info.ScratchDataSizeInBytes;
			frame->top_level_as_scratch = RT_CreateReadWriteBuffer(L"TLAS scratch", frame->tlas_scratch_size);
		}

		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlas_build_desc = {};
		tlas_build_desc.Inputs = tlas_inputs;
		tlas_build_desc.ScratchAccelerationStructureData = frame->top_level_as_scratch->GetGPUVirtualAddress();
		tlas_build_desc.DestAccelerationStructureData = /*g_d3d.frame_data[g_d3d.current_back_buffer_index].*/frame->top_level_as->GetGPUVirtualAddress();

		command_list->BuildRaytracingAccelerationStructure(&tlas_build_desc, 0, nullptr);
		UAVBarrier(command_list, /*g_d3d.frame_data[g_d3d.current_back_buffer_index].*/frame->top_level_as);
	}

	void LoadBlueNoiseTextures()
	{
        static_assert(BLUE_NOISE_TEX_COUNT==16,"Atlas shader register/grid must match the texture count");
        constexpr unsigned atlas_edge=4*64;
        std::vector<uint8_t> atlas(atlas_edge*atlas_edge*4);
		for (int blue_noise_index = 0; blue_noise_index < BLUE_NOISE_TEX_COUNT; blue_noise_index++)
		{
			MemoryScope temp;
			const char *path = RT_ArenaPrintF(temp, "assets/textures/noise/LDR_RGBA_%d.png", blue_noise_index);

			RT_Image image = RT_LoadImageFromDisk(temp, path, 4, false);
            if(!image.pixels || image.width!=64 || image.height!=64) RT_FATAL_ERROR("Blue-noise atlas requires the existing 64x64 RGBA textures");
            for(unsigned y=0;y<64;++y) {
                size_t dst=((blue_noise_index/4*64+y)*atlas_edge+blue_noise_index%4*64)*4;
                std::memcpy(atlas.data()+dst,static_cast<const uint8_t*>(image.pixels)+y*64*4,64*4);
            }

			g_d3d.blue_noise_textures[blue_noise_index] = RT_CreateTexture(Utf16FromUtf8(temp, path), DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, (size_t)image.width, (uint32_t)image.height);
			UploadTextureData(g_d3d.blue_noise_textures[blue_noise_index], image.width, image.height, 32, image.mips, 1);

			for (size_t frame_index = 0; frame_index < BACK_BUFFER_COUNT; frame_index++)
			{
				FrameData *frame = &g_d3d.frame_data[frame_index];

				D3D12_CPU_DESCRIPTOR_HANDLE handle = frame->descriptors.GetCPUDescriptor(D3D12GlobalDescriptors_SRV_BlueNoiseFirst + blue_noise_index);
				CreateTextureSRV(g_d3d.blue_noise_textures[blue_noise_index], handle, DXGI_FORMAT_R8G8B8A8_UNORM);
			}
		}
        g_d3d.blue_noise_atlas=RT_CreateTexture(L"Exact blue-noise atlas",DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_FLAG_NONE,atlas_edge,atlas_edge);
        void* mips[]={atlas.data()};UploadTextureData(g_d3d.blue_noise_atlas,atlas_edge,atlas_edge,32,mips,1);
        for(size_t frame_index=0;frame_index<BACK_BUFFER_COUNT;++frame_index) {
            auto srv=g_d3d.frame_data[frame_index].descriptors.GetCPUDescriptor(D3D12GlobalDescriptors_SRV_BlueNoiseAtlas);
            CreateTextureSRV(g_d3d.blue_noise_atlas,srv,DXGI_FORMAT_R8G8B8A8_UNORM);
        }

	}

	void ResizeResolutionDependentResources()
	{
		for (size_t i = 0; i < RenderTarget_COUNT; i++)
		{
			RT_RELEASE_RESOURCE(g_d3d.render_targets[i]);
			g_d3d.render_targets[i] = nullptr;
			// The cockpit monitor targets are recreated below too; they used to
			// be leaked (a full set of GPU textures per resize).
			if (g_d3d.render_targets_blit[i]) RT_RELEASE_RESOURCE(g_d3d.render_targets_blit[i]);
			g_d3d.render_targets_blit[i] = nullptr;
			if (g_camera_blit_targets_b[i]) RT_RELEASE_RESOURCE(g_camera_blit_targets_b[i]);
			g_camera_blit_targets_b[i] = nullptr;
		}
		// Recreated targets start without history.
		for (auto& last : g_camera_last_frame) last = 0;
		RT_RELEASE_RESOURCE(g_d3d.pixel_debug.uav_resource);

#if RT_PIXEL_DEBUG
		for (size_t i = 0; i < BACK_BUFFER_COUNT; i++)
		{
			FrameData *frame = &g_d3d.frame_data[i];
			RT_RELEASE_RESOURCE(frame->pixel_debug_readback);
		}
#endif
		RT_RELEASE_RESOURCE(g_d3d_raster.depth_target);

		CreateIntermediateRendertargets();
		CreatePixelDebugResources();
		CreateRasterDepthTarget();

		g_d3d_raster.render_target = g_d3d.rt.color_final;
	}

	void InitializeFrameResources()
	{
		// ------------------------------------------------------------------
		// Reserve descriptors used in the frame

		for (size_t i = 0; i < BACK_BUFFER_COUNT; i++)
		{
			FrameData* frame = &g_d3d.frame_data[i];
			frame->descriptors = g_d3d.cbv_srv_uav.Allocate(D3D12GlobalDescriptors_COUNT);
			frame->non_shader_descriptors = g_d3d.cbv_srv_uav_staging.Allocate(D3D12GlobalDescriptors_COUNT);
		}

		// ------------------------------------------------------------------
		// Initialize frame resources

		for (uint32_t i = 0; i < BACK_BUFFER_COUNT; ++i)
		{
			FrameData* frame = &g_d3d.frame_data[i];

			// ------------------------------------------------------------------
			// Create upload arena

			MemoryScope temp;

			wchar_t* name = Utf16FromUtf8(temp, RT_ArenaPrintF(temp, "Upload Buffer (Frame: %u)", i));

			size_t upload_buffer_size = RT_MB(64);
			frame->upload_buffer = RT_CreateUploadBuffer(name, upload_buffer_size);

			D3D12_RANGE null_range = {};

			void* mapped;
			frame->upload_buffer->Map(0, &null_range, &mapped);

			RT_ArenaInitWithMemory(&frame->upload_buffer_arena, mapped, upload_buffer_size);

			// ------------------------------------------------------------------
			// Allocate per-frame resources

			size_t cb_align = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;

			frame->instance_descs = AllocateFromUploadBuffer(frame, sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * MAX_INSTANCES);
			frame->instance_data = AllocateFromUploadBuffer(frame, sizeof(InstanceData) * MAX_INSTANCES);
			frame->scene_cb = AllocateFromUploadBuffer(frame, RT_ALIGN_POW2(sizeof(GlobalConstantBuffer), cb_align), cb_align);
			frame->tweak_vars = AllocateFromUploadBuffer(frame, RT_ALIGN_POW2(sizeof(TweakVars), cb_align), cb_align);
			frame->lights = AllocateFromUploadBuffer(frame, sizeof(RT_Light) * RT_MAX_LIGHTS);
			frame->material_edges = AllocateFromUploadBuffer(frame, sizeof(RT_MaterialEdge) * RT_MAX_MATERIAL_EDGES);
			frame->material_indices = AllocateFromUploadBuffer(frame, sizeof(uint16_t) * RT_MAX_MATERIALS);

			frame->upload_buffer_arena_reset = RT_ArenaGetMarker(&frame->upload_buffer_arena);
		}
	}

	void CreateDefaultTextures()
	{
		// ------------------------------------------------------------------
		// Create white debug texture

		{
			uint32_t pixels[] = { 0xFFFFFFFF };

			RT_UploadTextureParams white_texture_params = {};
			RT_Image& image = white_texture_params.image;
			image.width  = 1;
			image.height = 1;
			image.pixels = pixels;
			image.format = RT_TextureFormat_RGBA8;
			white_texture_params.name = "White Texture";

			g_d3d.white_texture_handle = RenderBackend::UploadTexture(white_texture_params);
			g_d3d.white_texture = g_texture_slotmap.Find(g_d3d.white_texture_handle);
		}

		// ------------------------------------------------------------------
		// Create black debug texture

		{
			uint32_t pixels[] = { 0xFF000000 };

			RT_UploadTextureParams black_texture_params = {};
			RT_Image& image = black_texture_params.image;
			image.width  = 1;
			image.height = 1;
			image.pixels = pixels;
			image.format = RT_TextureFormat_RGBA8;
			black_texture_params.name = "Black Texture";

			g_d3d.black_texture_handle = RenderBackend::UploadTexture(black_texture_params);
			g_d3d.black_texture = g_texture_slotmap.Find(g_d3d.black_texture_handle);
		}

		// ------------------------------------------------------------------
		// Create default normal texture

		{
			uint8_t r = 127;
			uint8_t g = 127;
			uint8_t b = 255;
			uint8_t a = 255;
			uint32_t pixels = (a << 24) | (b << 16) | (g << 8) | (r << 0);

			RT_UploadTextureParams default_normal_params = {};
			RT_Image& image = default_normal_params.image;
			image.width  = 1;
			image.height = 1;
			image.pixels = &pixels;
			image.format = RT_TextureFormat_RGBA8;
			default_normal_params.name = "Default normal texture";
			RT_ResourceHandle default_normal_handle = RenderBackend::UploadTexture(default_normal_params);

			g_d3d.default_normal_texture = g_texture_slotmap.Find(default_normal_handle);
		}

		// ------------------------------------------------------------------
		// Create pink checkerboard texture

		{
			uint32_t pixels[] =
			{
				0xFFFF00FF, 0xFFFF00FF, 0xFF000000, 0xFF000000,
				0xFFFF00FF, 0xFFFF00FF, 0xFF000000, 0xFF000000,
				0xFF000000, 0xFF000000, 0xFFFF00FF, 0xFFFF00FF,
				0xFF000000, 0xFF000000, 0xFFFF00FF, 0xFFFF00FF,
			};

			RT_UploadTextureParams params = {};
			RT_Image& image = params.image;
			image.width  = 4;
			image.height = 4;
			image.pixels = pixels;
			image.format = RT_TextureFormat_RGBA8;
			params.name = "Default Missing Texture";
			g_d3d.pink_checkerboard_texture = RenderBackend::UploadTexture(params);
		}
	}
}

namespace RT { FrameData* g_vr_frame_override = nullptr; FrameData* g_vr_prev_frame_override = nullptr; }

void RT::AllocateFrameDataLike(FrameData* frame, const FrameData& source, const wchar_t* name, size_t extra)
{
	frame->descriptors = g_d3d.cbv_srv_uav.Allocate(D3D12GlobalDescriptors_COUNT);
	// The staging heap is a linear arena sized for these tables and cannot
	// free, so a table is kept across VR shutdown and reused on restart.
	if (frame->non_shader_descriptors.IsNull())
		frame->non_shader_descriptors = g_d3d.cbv_srv_uav_staging.Allocate(D3D12GlobalDescriptors_COUNT);
	// These SRVs are otherwise initialized only for the desktop frame slots.
	for (int noise = 0; noise < BLUE_NOISE_TEX_COUNT; ++noise)
		CreateTextureSRV(g_d3d.blue_noise_textures[noise], frame->descriptors.GetCPUDescriptor(D3D12GlobalDescriptors_SRV_BlueNoiseFirst + noise), DXGI_FORMAT_R8G8B8A8_UNORM);
	CreateTextureSRV(g_d3d.blue_noise_atlas, frame->descriptors.GetCPUDescriptor(D3D12GlobalDescriptors_SRV_BlueNoiseAtlas), DXGI_FORMAT_R8G8B8A8_UNORM);
	const size_t size = source.instance_descs.size + source.instance_data.size + source.scene_cb.size +
		source.tweak_vars.size + source.lights.size + source.material_edges.size + source.material_indices.size + extra;
	frame->upload_buffer = RT_CreateUploadBuffer(name, size);
	void* mapped = nullptr; D3D12_RANGE empty = {};
	DX_CALL(frame->upload_buffer->Map(0, &empty, &mapped));
	RT_ArenaInitWithMemory(&frame->upload_buffer_arena, mapped, size);
	frame->instance_descs = AllocateFromUploadBuffer(frame, source.instance_descs.size, 256);
	frame->instance_data = AllocateFromUploadBuffer(frame, source.instance_data.size, 256);
	frame->scene_cb = AllocateFromUploadBuffer(frame, source.scene_cb.size, 256);
	frame->tweak_vars = AllocateFromUploadBuffer(frame, source.tweak_vars.size, 256);
	frame->lights = AllocateFromUploadBuffer(frame, source.lights.size, 256);
	frame->material_edges = AllocateFromUploadBuffer(frame, source.material_edges.size, 256);
	frame->material_indices = AllocateFromUploadBuffer(frame, source.material_indices.size, 256);
	frame->upload_buffer_arena_reset = RT_ArenaGetMarker(&frame->upload_buffer_arena);
	if (source.pixel_debug_readback)
		frame->pixel_debug_readback = RT_CreateReadbackBuffer(L"Frame pixel debug", static_cast<size_t>(source.pixel_debug_readback->GetDesc().Width));
}

BufferAllocation RT::AllocateFromUploadBuffer(FrameData *frame, size_t size, size_t align)
{
	BufferAllocation result = {};
	result.cpu    = RT_ArenaAllocNoZero(&frame->upload_buffer_arena, size, align);
	result.offset = (char*)result.cpu - frame->upload_buffer_arena.buffer;
	result.gpu    = frame->upload_buffer->GetGPUVirtualAddress() + result.offset;
	result.size   = size;
	result.buffer = frame->upload_buffer;
	return result;
}

void RenderBackend::Init(const RT_RendererInitParams* render_init_params)
{
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);

	g_d3d.hWnd = reinterpret_cast<HWND>(render_init_params->window_handle);
	g_d3d.arena = render_init_params->arena;

	for (int i = 0; i < HALTON_SAMPLE_COUNT; i++)
	{
		g_d3d.halton_samples[i].x = Halton(i, 2) - 0.5f;
		g_d3d.halton_samples[i].y = Halton(i, 3) - 0.5f;
	}

	g_d3d.mesh_tracker.Init(g_d3d.arena);

	InitTweakVars();
	// These two are needed to track the current upscaling aa mode as well as the fsr2 mode
	// So that we can detect if it changes, and resize the required render targets accordingly
	g_d3d.upscaling_aa_mode = tweak_vars.upscaling_aa_mode;
	g_d3d.amd_fsr2_mode = tweak_vars.amd_fsr2_mode;

	RECT client_rect;
	GetClientRect(g_d3d.hWnd, &client_rect);

#ifdef D2_ENABLE_OPENVR
    if(OpenVRPrototype::FixedMirrorCanvas()) ::GetClientRect(g_d3d.hWnd,&client_rect);
#endif
    g_d3d.window_width=client_rect.right-client_rect.left;
    g_d3d.window_height=client_rect.bottom-client_rect.top;
	g_d3d.output_width = client_rect.right - client_rect.left;
	g_d3d.output_height = client_rect.bottom - client_rect.top;
#ifdef D2_ENABLE_OPENVR
    if(OpenVRPrototype::FixedMirrorCanvas()) { g_d3d.output_width=RT_VR_MIRROR_WIDTH;g_d3d.output_height=RT_VR_MIRROR_HEIGHT; }
#endif
	if (g_d3d.upscaling_aa_mode == UPSCALING_AA_MODE_AMD_FSR_2_2)
	{
		FSR2::AdjustRenderResolutionForFSRMode(g_d3d.output_width, g_d3d.output_height, g_d3d.render_width, g_d3d.render_height);
	}
	else
	{
		g_d3d.render_width = g_d3d.output_width;
		g_d3d.render_height = g_d3d.output_height;
	}

    EnableDebugLayer();
    CreateDevice();
	CreateQueryHeapAndBuffer();
	GPUProfiler::Init();

	CreateDescriptorHeaps();
	CreateCommandQueues();
	CreateSwapChain(g_d3d.hWnd);

	FSR2::Init();

    CreateDxcCompilerState();
	InitializeFrameResources();

	CreateRootSignatures();
    ReloadAllTheRaytracingStateAsNecessary(true);

	g_d3d.tlas_instance_buffer = RT_CreateReadOnlyBuffer(L"TLAS instance buffer", sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * MAX_INSTANCES);
	CreateLightsBuffer();
	CreateMaterialBuffers();

	g_d3d_raster.depth_target_dsv = g_d3d.dsv.Allocate(1).GetCPUDescriptor(0);
	CreateRasterState();
	CreateRasterDepthTarget();
	g_d3d.resource_upload_ring_buffer = CreateRingBuffer(RT_MB(256));

	g_d3d.instance_data_buffer = RT_CreateReadOnlyBuffer(L"Instance Data Buffer", sizeof(InstanceData) * MAX_INSTANCES);

	// Gotta allocate this one for creating the intermediate render targets
	g_d3d.color_final_rtv = g_d3d.rtv.Allocate(1);

    CreateIntermediateRendertargets();
	CreatePixelDebugResources();

	ReloadComputeShadersIfThereAreNewOnes();
	CreateGenMipMapComputeShader();

	LoadBlueNoiseTextures();
	CreateDefaultTextures();

	g_d3d_raster.render_target = g_d3d.rt.color_final;
	g_d3d_raster.rtv_handle = g_d3d.color_final_rtv.cpu;

	// ------------------------------------------------------------------
	// Initialize all materials to have default textures

	for (uint16_t material_index = 0; material_index < RT_MAX_MATERIALS; material_index++)
	{
		RT_Material material = {};
		material.albedo_texture = g_d3d.pink_checkerboard_texture;
		UpdateMaterial(material_index, &material);
	}

	// And some built-in materials

	{
		RT_Material default_white = {};
		default_white.albedo_texture = g_d3d.white_texture_handle;
		default_white.roughness      = 0.8f;
		UpdateMaterial(RT_MATERIAL_FLAT_WHITE, &default_white);
	}

	{
		RT_Material default_emissive = {};
		default_emissive.albedo_texture  = g_d3d.white_texture_handle;
		default_emissive.emissive_strength = 32.0f; // tuned for lasers, for now
		default_emissive.emissive_color = RT_Vec3FromScalar(1.0f);
		default_emissive.flags           = RT_MaterialFlag_BlackbodyRadiator;
		UpdateMaterial(RT_MATERIAL_EMISSIVE_WHITE, &default_emissive);
	}

	{
		RT_Material black = {};
		black.albedo_texture = g_d3d.black_texture_handle;
		black.flags          = RT_MaterialFlag_BlackbodyRadiator;
		UpdateMaterial(RT_MATERIAL_BLACK, &black);
	}

	// ------------------------------------------------------------------
	// Create billboard quad

	{
		RT_Vec3 vertices[] =
		{
			{ -1.0f, -1.0f, +0.0f },
			{ +1.0f, -1.0f, +0.0f },
			{ +1.0f, +1.0f, +0.0f },
			{ -1.0f, +1.0f, +0.0f },
		};

		RT_Vec2 uvs[] =
		{
			{ 1, 1 },
			{ 0, 1 },
			{ 0, 0 },
			{ 1, 0 },
		};

		RT_Triangle triangles[2] = {};
		triangles[0].segment = -1;
		triangles[0].segment_adjacent = -1;
		triangles[1].segment = -1;
		triangles[1].segment_adjacent = -1;

		triangles[0].pos0     = vertices[0];
		triangles[0].pos1     = vertices[1];
		triangles[0].pos2     = vertices[2];
		triangles[0].normal0  = { 0, 0, 1 };
		triangles[0].normal1  = { 0, 0, 1 };
		triangles[0].normal2  = { 0, 0, 1 };
		triangles[0].tangent0 = { 1, 0, 0, 1 };
		triangles[0].tangent1 = { 1, 0, 0, 1 };
		triangles[0].tangent2 = { 1, 0, 0, 1 };
		triangles[0].uv0      = uvs[0];
		triangles[0].uv1      = uvs[1];
		triangles[0].uv2      = uvs[2];
		triangles[0].color    = 0xFFFFFFFF;
		triangles[0].material_edge_index = RT_TRIANGLE_MATERIAL_INSTANCE_OVERRIDE;

		triangles[1].pos0     = vertices[0];
		triangles[1].pos1     = vertices[2];
		triangles[1].pos2     = vertices[3];
		triangles[1].normal0  = { 0, 0, 1 };
		triangles[1].normal1  = { 0, 0, 1 };
		triangles[1].normal2  = { 0, 0, 1 };
		triangles[1].tangent0 = { 1, 0, 0, 1 };
		triangles[1].tangent1 = { 1, 0, 0, 1 };
		triangles[1].tangent2 = { 1, 0, 0, 1 };
		triangles[1].uv0      = uvs[0];
		triangles[1].uv1      = uvs[2];
		triangles[1].uv2      = uvs[3];
		triangles[1].color    = 0xFFFFFFFF;
		triangles[1].material_edge_index = RT_TRIANGLE_MATERIAL_INSTANCE_OVERRIDE;

		RT_UploadMeshParams params = {};
		params.name           = "Billboard Quad";
		params.triangles      = triangles;
		params.triangle_count = 2;

		g_d3d.billboard_quad = UploadMesh(params);
	}

	// ------------------------------------------------------------------
	// Create "missing model" cube

	{
		RT_Vec3 positions[] = {
			// Front face
			{ -0.5, -0.5, 0.5,  },   // Vertex 0
			{ 0.5, -0.5, 0.5,   },   // Vertex 1
			{ 0.5, 0.5, 0.5,    },   // Vertex 2
			{ -0.5, 0.5, 0.5,   },   // Vertex 3

			// Back face
			{ -0.5, -0.5, -0.5, },  // Vertex 4
			{ 0.5, -0.5, -0.5,  },  // Vertex 5
			{ 0.5, 0.5, -0.5,   },  // Vertex 6
			{ -0.5, 0.5, -0.5,  },  // Vertex 7

			// Left face
			{ -0.5, -0.5, -0.5, },  // Vertex 8
			{ -0.5, -0.5, 0.5,  },  // Vertex 9
			{ -0.5, 0.5, 0.5,   },  // Vertex 10
			{ -0.5, 0.5, -0.5,  },  // Vertex 11

			// Right face
			{ 0.5, -0.5, -0.5,  },  // Vertex 12
			{ 0.5, -0.5, 0.5,   },  // Vertex 13
			{ 0.5, 0.5, 0.5,    },  // Vertex 14
			{ 0.5, 0.5, -0.5,   },  // Vertex 15

			// Top face
			{ -0.5, 0.5, 0.5,   },  // Vertex 16
			{ 0.5, 0.5, 0.5,    },  // Vertex 17
			{ 0.5, 0.5, -0.5,   },  // Vertex 18
			{ -0.5, 0.5, -0.5,  },  // Vertex 19

			// Bottom face
			{ -0.5, -0.5, 0.5,  },  // Vertex 20
			{ 0.5, -0.5, 0.5,   },  // Vertex 21
			{ 0.5, -0.5, -0.5,  },  // Vertex 22
			{ -0.5, -0.5, -0.5, },  // Vertex 23
		};

		RT_Vec3 normals[] = {
			// Front face
			{ 0.0, 0.0, 1.0, },     // Vertex 0
			{ 0.0, 0.0, 1.0, },     // Vertex 1
			{ 0.0, 0.0, 1.0, },     // Vertex 2
			{ 0.0, 0.0, 1.0, },     // Vertex 3

			// Back face
			{ 0.0, 0.0, -1.0, },    // Vertex 4
			{ 0.0, 0.0, -1.0, },    // Vertex 5
			{ 0.0, 0.0, -1.0, },    // Vertex 6
			{ 0.0, 0.0, -1.0, },    // Vertex 7

			// Left face
			{ -1.0, 0.0, 0.0, },   // Vertex 8
			{ -1.0, 0.0, 0.0, },   // Vertex 9
			{ -1.0, 0.0, 0.0, },   // Vertex 10
			{ -1.0, 0.0, 0.0, },   // Vertex 11

			// Right face
			{ 1.0, 0.0, 0.0, },    // Vertex 12
			{ 1.0, 0.0, 0.0, },    // Vertex 13
			{ 1.0, 0.0, 0.0, },    // Vertex 14
			{ 1.0, 0.0, 0.0, },    // Vertex 15

			// Top face
			{ 0.0, 1.0, 0.0, },    // Vertex 16
			{ 0.0, 1.0, 0.0, },    // Vertex 17
			{ 0.0, 1.0, 0.0, },    // Vertex 18
			{ 0.0, 1.0, 0.0, },    // Vertex 19

			// Bottom face
			{ 0.0, -1.0, 0.0, },   // Vertex 20
			{ 0.0, -1.0, 0.0, },   // Vertex 21
			{ 0.0, -1.0, 0.0, },   // Vertex 22
			{ 0.0, -1.0, 0.0, },   // Vertex 23
		};

		RT_Vec2 uvs[] = {
			// Front face
			{ 0.0, 0.0, },       // Vertex 0
			{ 1.0, 0.0, },       // Vertex 1
			{ 1.0, 1.0, },       // Vertex 2
			{ 0.0, 1.0, },       // Vertex 3

			// Back face
			{ 1.0, 0.0, },       // Vertex 4
			{ 0.0, 0.0, },       // Vertex 5
			{ 0.0, 1.0, },       // Vertex 6
			{ 1.0, 1.0, },       // Vertex 7

			// Left face
			{ 0.0, 0.0, },       // Vertex 8
			{ 1.0, 0.0, },       // Vertex 9
			{ 1.0, 1.0, },       // Vertex 10
			{ 0.0, 1.0, },       // Vertex 11

			// Right face
			{ 1.0, 0.0, },       // Vertex 12
			{ 0.0, 0.0, },       // Vertex 13
			{ 0.0, 1.0, },       // Vertex 14
			{ 1.0, 1.0, },       // Vertex 15

			// Top face
			{ 0.0, 1.0, },       // Vertex 16
			{ 1.0, 1.0, },       // Vertex 17
			{ 1.0, 0.0, },       // Vertex 18
			{ 0.0, 0.0, },       // Vertex 19

			// Bottom face
			{ 1.0, 1.0, },       // Vertex 20
			{ 0.0, 1.0, },       // Vertex 21
			{ 0.0, 0.0, },       // Vertex 22
			{ 1.0, 0.0, },       // Vertex 23
		};

		RT_Triangle triangles[12] = {};
		for (size_t face_index = 0; face_index < RT_ARRAY_COUNT(triangles) / 2; face_index += 2)
		{
			size_t i = 4*face_index;

			RT_Triangle *t0 = &triangles[face_index + 0];
			RT_Triangle *t1 = &triangles[face_index + 1];

			t0->pos0 = positions[i + 0];
			t0->pos1 = positions[i + 1];
			t0->pos2 = positions[i + 2];

			t1->pos0 = positions[i + 0];
			t1->pos1 = positions[i + 2];
			t1->pos2 = positions[i + 3];

			t0->normal0 = normals[i + 0];
			t0->normal1 = normals[i + 1];
			t0->normal2 = normals[i + 2];

			t1->normal0 = normals[i + 0];
			t1->normal1 = normals[i + 2];
			t1->normal2 = normals[i + 3];

			t0->uv0 = uvs[i + 0];
			t0->uv1 = uvs[i + 1];
			t0->uv2 = uvs[i + 2];

			t1->uv0 = uvs[i + 0];
			t1->uv1 = uvs[i + 2];
			t1->uv2 = uvs[i + 3];

			t0->color = RT_PackRGBA(RT_Vec4Make(1, 1, 1, 1));
			t1->color = RT_PackRGBA(RT_Vec4Make(1, 1, 1, 1));

			// 0 should be an unused, pink checkerboard material
			t0->material_edge_index = RT_TRIANGLE_MATERIAL_INSTANCE_OVERRIDE;
			t1->material_edge_index = RT_TRIANGLE_MATERIAL_INSTANCE_OVERRIDE;
		}

		RT_GenerateTangents(triangles, RT_ARRAY_COUNT(triangles));

		RT_UploadMeshParams params = {};
		params.name           = "Cube";
		params.triangles      = triangles;
		params.triangle_count = RT_ARRAY_COUNT(triangles);

		g_d3d.cube = UploadMesh(params);
	}

	InitDearImGui();
}

void RenderBackend::Exit()
{
	Flush();
#ifdef D2_ENABLE_OPENVR
    HybridVisibilityShutdown();
#endif

	GPUProfiler::Exit();

	FSR2::Exit();

	// The GPU is idle after Flush(); free everything still queued.
	ProcessDeferredReleases(0, true);

	//------------------------------------------------------------------------
	// Release any textures created with CreateTexture, any buffers created
	// with any of the CreateBuffer variants, and every other resource that
	// was tracked.
	g_d3d.resource_tracker.ReleaseAllResources();

	SafeRelease(g_d3d.global_root_sig);

	SafeRelease(g_d3d.dxc_include_handler);
	SafeRelease(g_d3d.dxc_utils);
	SafeRelease(g_d3d.dxc_compiler);

	SafeRelease(g_d3d.query_heap);

	delete g_d3d.command_queue_direct;

	SafeRelease(g_d3d.dxgi_swapchain4);
	SafeRelease(g_d3d.device);
	SafeRelease(g_d3d.dxgi_adapter4);

	g_d3d.rtv.Release();
	g_d3d.cbv_srv_uav.Release();
	g_d3d.cbv_srv_uav_staging.Release();
	g_d3d.cbv_srv_uav_mips.Release();
}

void RenderBackend::Flush()
{
	// Wait on all operations to finish
	g_d3d.command_queue_direct->Flush();
	// Note(Justin): Temp fix, will be revisited soon with the command list refactor
	g_d3d.resource_upload_ring_buffer.commands_recorded = true;
	FlushRingBuffer(&g_d3d.resource_upload_ring_buffer);
	g_d3d.cbv_srv_uav_mips.Reset();
}

static void DisplayPixelDebug(RT_Vec2i pixel_location)
{
	(void)pixel_location;
#if RT_PIXEL_DEBUG
	ImGuiIO &io = ImGui::GetIO();

	if (io.MouseDown[2])
	{
		pixel_location.x = RT_CLAMP(pixel_location.x, 0, (int)g_d3d.render_width - 1);
		pixel_location.y = RT_CLAMP(pixel_location.y, 0, (int)g_d3d.render_height - 1);

		FrameData *frame = CurrentFrameData();

		PixelDebugData *data;
		frame->pixel_debug_readback->Map(0, nullptr, reinterpret_cast<void **>(&data));

		PixelDebugData pixel = data[pixel_location.y*g_d3d.render_width + pixel_location.x];

		ImGui::BeginTooltip();
		ImGui::Text("Primitive Index: %u", pixel.primitive_id);
		ImGui::Text("uv:   %.02f, %.02f", pixel.uv_barycentrics.x, pixel.uv_barycentrics.y);
		ImGui::Text("bary: %.02f, %.02f", pixel.uv_barycentrics.z, pixel.uv_barycentrics.w);
		ImGui::Text("metallic roughness:  %f, %f", pixel.metallic_roughness.x, pixel.metallic_roughness.y);
		ImGui::Text("material edge index:  %u", pixel.material_edge_index);
		ImGui::Text("material index 1:  %u", pixel.material_index1);
		ImGui::Text("material index 2:  %u", pixel.material_index2);
		ImGui::Text("segment index:  %u", pixel.material_edge_index / RT_SIDES_PER_SEGMENT);
		ImGui::Text("segment side:  %u", pixel.material_edge_index - RT_SIDES_PER_SEGMENT*(pixel.material_edge_index / RT_SIDES_PER_SEGMENT));
		ImGui::EndTooltip();

		D3D12_RANGE null_range = {};
		frame->pixel_debug_readback->Unmap(0, &null_range);
	}
#endif
}

void RenderBackend::DoDebugMenus(const RT_DoRendererDebugMenuParams *params)
{
	ImGuiIO &io = ImGui::GetIO();

	if (params->ui_has_cursor_focus)
	{
		if (ImGui::Begin("Render Settings"))
		{
			static float loaded_timer = 0.0f;
			static float saved_timer = 0.0f;
			static char* preset_name;

			if (ImGui::Button("Load Settings"))
			{
				if (RT_DeserializeConfigFromFile(g_d3d.io.config, RT_RENDER_SETTINGS_CONFIG_FILE))
				{
					UpdateTweakvarsFromIOConfig();
					loaded_timer = 2.0f;
				}
			}

			ImGui::SameLine();

			if (ImGui::Button("Save Settings"))
			{
				RT_SerializeConfigToFile(g_d3d.io.config, RT_RENDER_SETTINGS_CONFIG_FILE);
				saved_timer = 2.0f;
			}

			ImGui::SameLine();

			if (ImGui::Button("Save as preset"))
			{
				ImGui::OpenPopup("Preset name");
				preset_name = (char*)malloc(128);
				memset(preset_name, 0, 128);
				saved_timer = 2.0f;
			}

			if (ImGui::BeginPopupModal("Preset name")) {
				ImGui::InputText("name", preset_name, 128);

				char path[23] = "assets/render_presets/";

				if (ImGui::Button("Done"))
				{
					ImGui::CloseCurrentPopup();
					char buff[157];
					strcpy(buff, path);
					strcat(buff, preset_name);
					strcat(buff, ".vars");
					RT_SerializeConfigToFile(g_d3d.io.config, buff);
					free(preset_name);
				}

				ImGui::EndPopup();
			}

			ImGui::SameLine();

			if (ImGui::Button("Reset Default Settings"))
			{
				ResetTweakvarsToDefault();
			}

			if (loaded_timer > 0.0f)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 1.0f, 0.5f, 1.0f));
				ImGui::Text("Loaded Renderer Settings");
				ImGui::PopStyleColor();

				loaded_timer -= 1.0f / 60.0f;
			}

			if (saved_timer > 0.0f)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 1.0f, 1.0f));
				ImGui::Text("Saved Renderer Settings");
				ImGui::PopStyleColor();

				saved_timer -= 1.0f / 60.0f;
			}

#if RT_PIXEL_DEBUG
			ImGui::Text("Pixel debug mode is enabled");
			ImGui::Text("Hold Middle Mouse Button to debug pixels");
#else
			ImGui::Text("Pixel debug mode is disabled");
#endif

			ImGui::Text("Lights: %u/%u", g_d3d.prev_lights_count, RT_MAX_LIGHTS);

			if (ImGui::Button("Clear Render Targets"))
			{
				g_d3d.accum_frame_index = 0;
			}

			ImGui::Text("Frame Index: %llu", g_d3d.accum_frame_index);
			ImGui::Text("Output resolution: %ux%u", g_d3d.output_width, g_d3d.output_height);
			ImGui::Text("Render resolution: %ux%u", g_d3d.render_width, g_d3d.render_height);

			static uint32_t fps = 0;
			static uint32_t fps_accum = 0;
			static float frame_time = 0.0f;
			static float frame_time_accum = 0.0f;
			static uint16_t num_accumulated = 0;

			fps_accum += (uint32_t)(1.0 / g_d3d.io.delta_time);
			frame_time_accum += g_d3d.io.delta_time;
			num_accumulated++;

			if (frame_time_accum >= 1.0f)
			{
				frame_time = frame_time_accum / num_accumulated;
				fps = (uint32_t)(1.0 / frame_time);

				fps_accum = 0;
				frame_time_accum = 0.0f;
				num_accumulated = 0;
			}
			ImGui::Text("FPS: %u", fps);
			ImGui::Text("Frame time: %.3fms", frame_time * 1000.0);

			int current_render_mode = g_d3d.io.debug_render_mode;

			char *render_modes[] = { 
				"Default", 
				"Debug",
				"Normals", 
				"Depth", 
				"Albedo", 
				"Emissive", 
				"Diffuse", 
				"Specular", 
				"Motion", 
				"Metallic Roughness",
				"History Length",
				"Materials",
				"First Moment",
				"Second Moment",
				"Variance",
				"Bloom0",
				"Bloom1",
				"Bloom2",
				"Bloom3",
				"Bloom4",
				"Bloom5",
				"Bloom6",
				"Bloom7",
				"Fsr2 reactive mask"
			};

			if (ImGui::BeginCombo("Debug render mode", render_modes[current_render_mode]))
			{
				for (int i = 0; i < RT_ARRAY_COUNT(render_modes); i++)
				{
					bool is_selected = (current_render_mode == i);

					if (ImGui::Selectable(render_modes[i], is_selected))
						current_render_mode = i;

					if (is_selected)
						ImGui::SetItemDefaultFocus(); // What does this do? Took it from the imgui demo.
				}
				ImGui::EndCombo();
			}
			ImGui::SliderFloat("Debug render blend factor", &tweak_vars.debug_render_blend_factor, 0.0f, 1.0f, "%.3f");

			g_d3d.io.debug_render_mode = current_render_mode;

			#define TWEAK_CATEGORY_BEGIN(name) if (ImGui::CollapsingHeader(name)) { ImGui::Indent(); ImGui::PushID(name); 
			#define TWEAK_CATEGORY_END() ImGui::Unindent(); ImGui::PopID(); }
			#define TWEAK_BOOL(name, var, value) { bool v = tweak_vars.var; ImGui::Checkbox(name, &v); tweak_vars.var = v; }
			#define TWEAK_INT(name, var, value, min, max) ImGui::SliderInt(name, &tweak_vars.var, min, max);
			#define TWEAK_FLOAT(name, var, value, min, max) ImGui::SliderFloat(name, &tweak_vars.var, (float)min, (float)max);
			#define TWEAK_COLOR(name, var, value) ImGui::ColorEdit3(name, (float *)&tweak_vars.var);
			#define TWEAK_OPTIONS(name, var, value, ...) { \
					char *options[] = { __VA_ARGS__ }; \
					if (ImGui::BeginCombo(name, options[tweak_vars.var])) { \
						for (int i = 0; i < RT_ARRAY_COUNT(options); i++) { \
							bool is_selected = tweak_vars.var == i; \
							if (ImGui::Selectable(options[i], is_selected)) tweak_vars.var = i; \
							if (is_selected) ImGui::SetItemDefaultFocus(); \
						} \
						ImGui::EndCombo(); \
					} \
				}

			#include "shared_tweakvars.hlsl.h"

			#undef TWEAK_CATEGORY_BEGIN
			#undef TWEAK_CATEGORY_END
			#undef TWEAK_BOOL
			#undef TWEAK_INT
			#undef TWEAK_FLOAT
			#undef TWEAK_COLOR
			#undef TWEAK_OPTIONS

			WriteTweakvarsToIOConfig();

			// NOTE(daniel): This is a bit half-baked because I just wanted to try the early out thing.
			if (ImGui::CollapsingHeader("Compile Time Shader Settings"))
			{
				ImGui::Indent();
				if (ImGui::Checkbox("Early Out when Out of Bounds in Compute Shaders", &g_shader_defines.early_out_of_bounds))
				{
					if (g_shader_defines.early_out_of_bounds)
					{
						RT_ConfigWriteString(&g_d3d.global_shader_defines, RT_StringLiteral("DO_EARLY_OUT_IN_COMPUTE_SHADERS"), RT_StringLiteral("1"));
					}
					else
					{
						RT_ConfigEraseKey(&g_d3d.global_shader_defines, RT_StringLiteral("DO_EARLY_OUT_IN_COMPUTE_SHADERS"));
					}
				}
				ImGui::Unindent();
			}
		} ImGui::End();

		RT_Vec2i pixel = { (int)io.MousePos.x, (int)io.MousePos.y };
		DisplayPixelDebug(pixel);
	}

	// GPU Profiler
	GPUProfiler::RenderImGuiMenus();
}

void RenderBackend::BeginFrame()
{
	ImGui_ImplDX12_NewFrame();
}

void RenderBackend::BeginScene(const RT_SceneSettings* scene_settings)
{
	g_d3d.render_width_override = scene_settings->render_width_override;
	g_d3d.render_height_override = scene_settings->render_height_override;
	if (g_d3d.upscaling_aa_mode == UPSCALING_AA_MODE_AMD_FSR_2_2)
	{
		FSR2::RescaleResolutionForFSRMode(g_d3d.render_width_override, g_d3d.render_height_override);
	}

	if (!RT_Vec3AreEqual(scene_settings->camera->position, g_d3d.scene.camera.position, 0.001f) ||
		!RT_Vec3AreEqual(scene_settings->camera->forward, g_d3d.scene.camera.forward, 0.001f) ||
		!RT_FloatAreEqual(scene_settings->camera->vfov, g_d3d.scene.camera.vfov, 0.001f))
	{
		g_d3d.scene.last_camera_update_frame = g_d3d.frame_index;
	}

	g_d3d.scene.freezeframe = false;
	g_d3d.scene.render_blit = scene_settings->render_blit;
	g_d3d.scene.render_segment = scene_settings->render_segment;
	g_d3d.scene.external = scene_settings->external;

	g_d3d.scene.freezeframe |= tweak_vars.freezeframe;
	g_d3d.scene.prev_camera = g_d3d.scene.camera;

	g_d3d.prev_lights_count = g_d3d.lights_count;

	if (!g_d3d.scene.freezeframe)
	{
		g_d3d.scene.camera = *scene_settings->camera;
		g_d3d.scene.camera.forward = RT_Vec3Normalize(g_d3d.scene.camera.forward);
		g_d3d.scene.camera.right = RT_Vec3Normalize(g_d3d.scene.camera.right);
		g_d3d.scene.camera.up = RT_Vec3Normalize(g_d3d.scene.camera.up);
		g_d3d.scene.camera.near_plane = 0.001f;
		g_d3d.scene.camera.far_plane = 10000.0f;
		g_d3d.tlas_instance_count = 0;
		g_d3d.lights_count = 0;
	}

	g_d3d.io.frame_frozen = g_d3d.scene.freezeframe;
}

void RenderBackend::EndScene()
{
	UpdateTweakvarsFromIOConfig();

	if (!g_d3d.scene.freezeframe)
	{
		// ------------------------------------------------------------------
		// Set up constant buffer

		FrameData* frame = CurrentFrameData();
		FrameData* prev_frame = PrevFrameData();

		GlobalConstantBuffer* scene_cb = frame->scene_cb.As<GlobalConstantBuffer>();
		GlobalConstantBuffer* prev_scene_cb = prev_frame->scene_cb.As<GlobalConstantBuffer>();
		uint32_t rt_render_width = 0;
		uint32_t rt_render_height = 0;
		{
			RT_Camera* camera = &g_d3d.scene.camera;

			RT_Mat4 view, view_inv, proj, proj_inv;
			{
				XMVECTOR up = XMVectorSet(camera->up.x, camera->up.y, camera->up.z, 0);
				XMVECTOR pos = XMVectorSet(camera->position.x, camera->position.y, camera->position.z, 0);
				XMVECTOR dir = XMVectorSet(camera->forward.x, camera->forward.y, camera->forward.z, 0);

				XMMATRIX xm_view = XMMatrixLookToLH(pos, dir, up);
				XMMATRIX xm_view_inv = XMMatrixInverse(nullptr, xm_view);

				// Prefer an explicit render_width_override when provided for small/mini renders
				uint32_t effective_render_width = (g_d3d.scene.render_blit ? g_d3d.output_width : g_d3d.render_width) / (g_d3d.scene.render_blit ? 4 : 1);
				uint32_t effective_render_height = (g_d3d.scene.render_blit ? g_d3d.output_height : g_d3d.render_height) / (g_d3d.scene.render_blit ? 4 : 1);

				rt_render_width = g_d3d.render_width_override == 0 || g_d3d.scene.render_blit ? effective_render_width : g_d3d.render_width_override;
				rt_render_height = g_d3d.render_height_override == 0 || g_d3d.scene.render_blit ? effective_render_height : g_d3d.render_height_override;
				float aspect_ratio = (float)rt_render_width / (float)rt_render_height;
				XMMATRIX xm_proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(camera->vfov), aspect_ratio, camera->near_plane, camera->far_plane);
#ifdef D2_ENABLE_OPENVR
				if (OpenVRPrototype::EyePass()) {
					float left, right, top, bottom;
					OpenVRPrototype::ProjectionTangents(&left, &right, &top, &bottom);
					const float nearZ = camera->near_plane;
					xm_proj = XMMatrixPerspectiveOffCenterLH(left * nearZ, right * nearZ,
						-bottom * nearZ, -top * nearZ, nearZ, camera->far_plane);
				}
#endif
				XMMATRIX xm_proj_inv = XMMatrixInverse(nullptr, xm_proj);

				// Silly thing to do, but who cares
				xm_view = XMMatrixTranspose(xm_view);
				xm_view_inv = XMMatrixTranspose(xm_view_inv);
				xm_proj = XMMatrixTranspose(xm_proj);
				xm_proj_inv = XMMatrixTranspose(xm_proj_inv);
				memcpy(&view, &xm_view, sizeof(XMMATRIX));
				memcpy(&view_inv, &xm_view_inv, sizeof(XMMATRIX));
				memcpy(&proj, &xm_proj, sizeof(XMMATRIX));
				memcpy(&proj_inv, &xm_proj_inv, sizeof(XMMATRIX));
			}

			uint32_t debug_flags = 0;

			scene_cb->debug_render_mode = (uint32_t)g_d3d.io.debug_render_mode;

			scene_cb->view = view;
			scene_cb->view_inv = view_inv;
			scene_cb->proj = proj;
			scene_cb->proj_inv = proj_inv;

			scene_cb->prev_view = prev_scene_cb->view;
			scene_cb->prev_view_inv = prev_scene_cb->view_inv;
			scene_cb->prev_proj = prev_scene_cb->proj;
			scene_cb->prev_proj_inv = prev_scene_cb->proj_inv;

			scene_cb->taa_jitter = g_d3d.halton_samples[g_d3d.frame_index % HALTON_SAMPLE_COUNT];
			// output_dim is the display-scaled output size. For mini-window 'render_blit'
			// passes we want output_dim to match the effective render target size so
			// post-process shaders compute UVs correctly. Use rt_render_width/height
			// when rendering into small cockpit windows.
			if (g_d3d.scene.render_blit) {
				scene_cb->output_dim = RT_Vec2iMake((int)rt_render_width, (int)rt_render_height);
			} else {
				scene_cb->output_dim = RT_Vec2iMake(g_d3d.output_width, g_d3d.output_height);
			}
				// Prefer explicit overrides for mini-window renders so projection matches the small RT size
				uint32_t effective_render_width2 = (g_d3d.scene.render_blit ? g_d3d.output_width : g_d3d.render_width) / (g_d3d.scene.render_blit ? 4 : 1);
				uint32_t effective_render_height2 = (g_d3d.scene.render_blit ? g_d3d.output_height : g_d3d.render_height) / (g_d3d.scene.render_blit ? 4 : 1);

				rt_render_width = g_d3d.render_width_override == 0 || g_d3d.scene.render_blit ? effective_render_width2 : g_d3d.render_width_override;
				rt_render_height = g_d3d.render_height_override == 0 || g_d3d.scene.render_blit ? effective_render_height2 : g_d3d.render_height_override;
			scene_cb->render_dim = RT_Vec2iMake(rt_render_width, rt_render_height);
			scene_cb->frame_index = (uint32_t)g_d3d.accum_frame_index;
			scene_cb->debug_flags = debug_flags;
			scene_cb->lights_count = g_d3d.lights_count;
			scene_cb->viewport_offset_y = g_d3d.viewport_offset_y;
			scene_cb->ray_segment = g_d3d.scene.render_segment;
			scene_cb->external = g_d3d.scene.external;
			scene_cb->screen_color_overlay = g_d3d.io.screen_overlay_color;

			D3D12_CPU_DESCRIPTOR_HANDLE cbv = frame->descriptors.GetCPUDescriptor(D3D12GlobalDescriptors_CBV_GlobalConstantBuffer);

			D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc = {};
			cbv_desc.BufferLocation = frame->scene_cb.gpu;
			cbv_desc.SizeInBytes = (UINT)frame->scene_cb.size;
			g_d3d.device->CreateConstantBufferView(&cbv_desc, cbv);
		}

		// Debug overlay: show render/viewport/proj dims to help diagnose off-center/stretched cockpit cameras
		if (g_d3d.io.debug_render_mode) {
			ImGui::Begin("Render Debug");
			ImGui::Text("g_d3d.output: %u x %u", g_d3d.output_width, g_d3d.output_height);
			ImGui::Text("g_d3d.render: %u x %u", g_d3d.render_width, g_d3d.render_height);
			ImGui::Text("rt_render (effective): %u x %u", rt_render_width, rt_render_height);
			ImGui::Text("scene_cb.output_dim: %d x %d", scene_cb->output_dim.x, scene_cb->output_dim.y);
			ImGui::Text("scene_cb.render_dim: %d x %d", scene_cb->render_dim.x, scene_cb->render_dim.y);
			ImGui::Text("g_d3d_raster.viewport: %.0f x %.0f", g_d3d_raster.viewport.Width, g_d3d_raster.viewport.Height);
			ImGui::Text("blit dst: %u,%u %u x %u", g_d3d_raster.blit_debug_dst_x, g_d3d_raster.blit_debug_dst_y, g_d3d_raster.blit_debug_dst_w, g_d3d_raster.blit_debug_dst_h);
			ImGui::Text("blit draw: %0.2f x %0.2f offset %0.2f x %0.2f", g_d3d_raster.blit_debug_draw_w, g_d3d_raster.blit_debug_draw_h, g_d3d_raster.blit_debug_offset_x, g_d3d_raster.blit_debug_offset_y);
			// print a couple proj matrix elements for sanity
			ImGui::Text("proj[0][0]=%f proj[1][1]=%f", scene_cb->proj.e[0][0], scene_cb->proj.e[1][1]);
			ImGui::End();
		}

		{
			TweakVars* frame_tweakvars = frame->tweak_vars.As<TweakVars>();
			memcpy(frame_tweakvars, &tweak_vars, sizeof(TweakVars));

			if (g_d3d.upscaling_aa_mode == UPSCALING_AA_MODE_AMD_FSR_2_2)
			{
				RT_Vec2 fsr2_mip_bias = FSR2::GetMipBiasForFSRMode(g_d3d.output_width, g_d3d.output_height, g_d3d.render_width, g_d3d.render_height);
				frame_tweakvars->mip_bias_u = fsr2_mip_bias.x;
				frame_tweakvars->mip_bias_v = fsr2_mip_bias.y;

				// FSR magnifies residual low-resolution path-tracing noise. Give
				// diffuse and specular lighting more temporal history only in the
				// upscaled main view; reactive effects still reject stale history.
				frame_tweakvars->svgf_max_hist_len_diff = RT_MAX(frame_tweakvars->svgf_max_hist_len_diff, 12);
				frame_tweakvars->svgf_max_hist_len_spec = RT_MAX(frame_tweakvars->svgf_max_hist_len_spec, 8);
			}

			D3D12_CPU_DESCRIPTOR_HANDLE cbv = frame->descriptors.GetCPUDescriptor(D3D12GlobalDescriptors_CBV_TweakVars);
			D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc = {};
			cbv_desc.BufferLocation = frame->tweak_vars.gpu;
			cbv_desc.SizeInBytes = (UINT)frame->tweak_vars.size;
			g_d3d.device->CreateConstantBufferView(&cbv_desc, cbv);
		}

		{
			// Written for every render (desktop, each VR eye, each cockpit
			// monitor) so every frame's descriptor table has a valid b2.
			constexpr size_t shield_cb_size = (sizeof(RT_ShieldEffects) + 255) & ~size_t(255);
			BufferAllocation shield_cb = AllocateFromUploadBuffer(frame, shield_cb_size, 256);
			RT_ShieldEffects* shields = shield_cb.As<RT_ShieldEffects>();
			*shields = g_shield_effects;
			if (g_d3d.scene.render_blit)
			{
				// The sweep and your own bubble belong to the pilot's own view,
				// not to cockpit monitors (the rear view sits inside the bubble).
				shields->sweep_progress = -1.0f;
				uint32_t kept = 0;
				for (uint32_t i = 0; i < shields->bubble_count; ++i)
					if (shields->bubbles[i].self_bubble == 0.0f)
						shields->bubbles[kept++] = shields->bubbles[i];
				shields->bubble_count = kept;
			}

			D3D12_CPU_DESCRIPTOR_HANDLE cbv = frame->descriptors.GetCPUDescriptor(D3D12GlobalDescriptors_CBV_ShieldEffects);
			D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc = {};
			cbv_desc.BufferLocation = shield_cb.gpu;
			cbv_desc.SizeInBytes = (UINT)shield_cb_size;
			g_d3d.device->CreateConstantBufferView(&cbv_desc, cbv);
		}
	}
}

void RenderBackend::EndFrame()
{
	g_shader_defines.last_modified_time = g_d3d.global_shader_defines.last_modified_time;
}

// Capture the completed desktop presentation, never an active eye-pass target.
static void CaptureDesktopScreenshot(ID3D12Resource* desktop=nullptr) {
    if(!g_d3d.queued_screenshot) return;
    if(!desktop) desktop=g_d3d.back_buffers[g_d3d.current_back_buffer_index];
    ID3D12Resource* captureTarget=desktop;
    ID3D12Resource* scaledTarget=nullptr;
    defer { if(scaledTarget) RT_RELEASE_RESOURCE(scaledTarget); };
    DescriptorAllocation sourceView={},targetView={};
    const UINT width=std::max(1u,g_d3d.window_width),height=std::max(1u,g_d3d.window_height);
    {
        // DXGI stretches VR's fixed mirror canvas to the client area. Reproduce
        // that presentation at the window's actual pixel size for the PNG.
        // Always read back an offscreen copy, keeping the presentable buffer
        // independent of the synchronous PNG readback, even at native size.
        captureTarget=RT_CreateTexture(L"Desktop Screenshot",DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,width,height,D3D12_RESOURCE_STATE_RENDER_TARGET);
        scaledTarget=captureTarget;
        sourceView=g_d3d.cbv_srv_uav.Allocate(1);
        if(g_d3d.screenshot_rtv.IsNull()) g_d3d.screenshot_rtv=g_d3d.rtv.Allocate(1);
        targetView=g_d3d.screenshot_rtv;
        CreateTextureSRV(desktop,sourceView.cpu,DXGI_FORMAT_R8G8B8A8_UNORM);
        CreateTextureRTV(captureTarget,targetView.cpu,DXGI_FORMAT_R8G8B8A8_UNORM);
        auto& commands=g_d3d.command_queue_direct->GetCommandList();
        const auto oldState=g_d3d.resource_tracker.GetResourceState(desktop);
        ResourceTransition(commands,desktop,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ID3D12DescriptorHeap* heaps[]={g_d3d.cbv_srv_uav.GetHeap()};commands->SetDescriptorHeaps(1,heaps);
        commands->SetPipelineState(g_d3d_raster.blit_state);commands->SetGraphicsRootSignature(g_d3d_raster.blit_root_sig);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_VIEWPORT viewport={0,0,(float)width,(float)height,0,1};
        const D3D12_RECT scissor={0,0,(LONG)width,(LONG)height};
        commands->RSSetViewports(1,&viewport);commands->RSSetScissorRects(1,&scissor);
        commands->OMSetRenderTargets(1,&targetView.cpu,FALSE,nullptr);
        const float constants[7]={0,0,(float)width,(float)height,0,(float)width,(float)height};
        commands->SetGraphicsRoot32BitConstants(0,7,constants,0);
        commands->SetGraphicsRootDescriptorTable(1,sourceView.gpu);
        commands->DrawInstanced(6,1,0,0);
        ResourceTransition(commands,desktop,oldState);
        g_d3d.command_queue_direct->ExecuteCommandList(commands);
    }
	if (g_d3d.queued_screenshot)
	{
		g_d3d.queued_screenshot = false;

		CommandList& screenshot_command_list = g_d3d.command_queue_direct->GetCommandList();

		D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;

		D3D12_RESOURCE_DESC src_desc = captureTarget->GetDesc();
		g_d3d.device->GetCopyableFootprints(&src_desc, 0, 1, 0, &fp, nullptr, nullptr, nullptr);

		D3D12_TEXTURE_COPY_LOCATION src = {};
		src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		src.SubresourceIndex = 0;
		src.pResource = captureTarget;

		ID3D12Resource* readback = RT_CreateReadbackBuffer(L"Screenshot Readback", fp.Footprint.RowPitch * fp.Footprint.Height);
		defer { RT_RELEASE_RESOURCE(readback); };

		D3D12_TEXTURE_COPY_LOCATION dst = {};
		dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		dst.pResource = readback;
		dst.PlacedFootprint = fp;

		D3D12_RESOURCE_STATES old_state = g_d3d.resource_tracker.GetResourceState(captureTarget);
		ResourceTransition(screenshot_command_list, captureTarget, D3D12_RESOURCE_STATE_COPY_SOURCE);

		screenshot_command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

		ResourceTransition(screenshot_command_list, captureTarget, old_state);

		uint64_t fence_value = g_d3d.command_queue_direct->ExecuteCommandList(screenshot_command_list);
		CurrentFrameData()->fence_value=fence_value;
		g_d3d.command_queue_direct->WaitForFenceValue(fence_value);

		uint32_t* pixels;
		DX_CALL(readback->Map(0, nullptr, (void**)&pixels));

		// PNG dimensions come from the desktop presentation resource.
		RT_WritePNGToDisk(g_d3d.queued_screenshot_name,
			(int)src_desc.Width, (int)src_desc.Height, 4, pixels, fp.Footprint.RowPitch);

		readback->Unmap(0, nullptr);
	}

    if(!sourceView.IsNull()) g_d3d.cbv_srv_uav.Free(sourceView);
}

static void CopyToMonitor(CommandList& commands) {
#ifdef D2_ENABLE_OPENVR
    if(OpenVRPrototype::FixedMirrorCanvas()) {
        const auto rect=RT_VRMirrorPresentationRect(static_cast<float>(g_d3d.window_width),static_cast<float>(g_d3d.window_height));
        auto* target=g_d3d.back_buffers[g_d3d.current_back_buffer_index];
        if(rect.x<0.01f && rect.y<0.01f) { CopyBuffer(commands,target,g_d3d.rt.color_final);return; }
        // Precompensate for DXGI's window scaling so non-16:9 windows have bars,
        // rather than distorting the spectator image or the menu.
        ResourceTransition(commands,g_d3d.rt.color_final,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ResourceTransition(commands,target,D3D12_RESOURCE_STATE_RENDER_TARGET);
        const auto view=g_d3d.spectator_views[g_d3d.current_back_buffer_index].cpu;
        const float black[4]={0,0,0,1};commands->ClearRenderTargetView(view,black,0,nullptr);
        ID3D12DescriptorHeap* heaps[]={g_d3d.cbv_srv_uav.GetHeap()};commands->SetDescriptorHeaps(1,heaps);
        commands->SetPipelineState(g_d3d_raster.blit_state);commands->SetGraphicsRootSignature(g_d3d_raster.blit_root_sig);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_VIEWPORT viewport={0,0,RT_VR_MIRROR_WIDTH,RT_VR_MIRROR_HEIGHT,0,1};
        const D3D12_RECT scissor={0,0,RT_VR_MIRROR_WIDTH,RT_VR_MIRROR_HEIGHT};
        commands->RSSetViewports(1,&viewport);commands->RSSetScissorRects(1,&scissor);commands->OMSetRenderTargets(1,&view,FALSE,nullptr);
        const float constants[7]={rect.x,rect.y,rect.width,rect.height,0,RT_VR_MIRROR_WIDTH,RT_VR_MIRROR_HEIGHT};
        commands->SetGraphicsRoot32BitConstants(0,7,constants,0);commands->SetGraphicsRootDescriptorTable(1,g_d3d.spectator_source.gpu);
        commands->DrawInstanced(6,1,0,0);ResourceTransition(commands,target,D3D12_RESOURCE_STATE_PRESENT);return;
    }
#endif
    CopyBuffer(commands,g_d3d.back_buffers[g_d3d.current_back_buffer_index],g_d3d.rt.color_final);
}
void RenderBackend::BlitVRMirror() {
    auto& commands=g_d3d.command_queue_direct->GetCommandList();CopyToMonitor(commands);g_d3d.command_queue_direct->ExecuteCommandList(commands);CaptureDesktopScreenshot();
}

void RenderBackend::SwapBuffers()
{
	// ------------------------------------------------------------------
	// Copy render target to backbuffer
	CommandList& command_list = g_d3d.command_queue_direct->GetCommandList();



#ifdef D2_ENABLE_OPENVR
	// Finish desktop/UI work before SteamVR consumes the eye textures and UI panel.
	g_d3d.command_queue_direct->ExecuteCommandList(command_list);
	OpenVRPrototype::Present();
	CommandList& present_commands = g_d3d.command_queue_direct->GetCommandList();
    CopyToMonitor(present_commands);
#else
    CopyBuffer(command_list,g_d3d.back_buffers[g_d3d.current_back_buffer_index],g_d3d.rt.color_final);
#endif
	// Update this frame's fence value
	FrameData* frame = CurrentFrameData();
#ifdef D2_ENABLE_OPENVR
	frame->fence_value = g_d3d.command_queue_direct->ExecuteCommandList(present_commands);
#else
	frame->fence_value = g_d3d.command_queue_direct->ExecuteCommandList(command_list);
#endif
	StampDeferredReleases(frame->fence_value);
	
#if defined(RT_FORCE_HEAVY_SYNCHRONIZATION)
	g_d3d.command_queue_direct->WaitForFenceValue(frame->fence_value);
#endif

#ifdef D2_ENABLE_OPENVR
	if(OpenVRPrototype::Active()) CaptureDesktopScreenshot();
#endif

	// ------------------------------------------------------------------
	// Swap buffers

	bool desktopVsync = tweak_vars.vsync;
#ifdef D2_ENABLE_OPENVR
	// SteamVR paces the headset. Do not also stall it on the desktop monitor.
	if (OpenVRPrototype::Active()) desktopVsync = false;
#endif
	uint32_t sync_interval = desktopVsync ? 1 : 0;
	uint32_t present_flags = g_d3d.tearing_supported && !desktopVsync ? DXGI_PRESENT_ALLOW_TEARING : 0;
	
	HRESULT hr = g_d3d.dxgi_swapchain4->Present(sync_interval, present_flags);

	// ------------------------------------------------------------------
	// Handle device removal

	if (hr == DXGI_ERROR_DEVICE_REMOVED)
	{
		OutputDebugStringA("Device Removal Detected\n");

		ID3D12DeviceRemovedExtendedData1* dred;
		if (SUCCEEDED(g_d3d.device->QueryInterface(IID_PPV_ARGS(&dred))))
		{
			MemoryScope temp; 

			D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs = {};
			DX_CALL(dred->GetAutoBreadcrumbsOutput1(&breadcrumbs));

			D3D12_DRED_PAGE_FAULT_OUTPUT1 page_fault = {};
			DX_CALL(dred->GetPageFaultAllocationOutput1(&page_fault));

			OutputDebugStringA("DRED Report:\n");

			for (const D3D12_AUTO_BREADCRUMB_NODE1 *node = breadcrumbs.pHeadAutoBreadcrumbNode;
				 node;
				 node = node->pNext)
			{
				OutputDebugStringA("-------------------------\n");
				OutputDebugStringA(RT_ArenaPrintF(temp, "Command List Debug Name: %s\n", node->pCommandListDebugNameA));
				OutputDebugStringA(RT_ArenaPrintF(temp, "Command Queue Debug Name: %s\n", node->pCommandQueueDebugNameA));

				for (size_t breadcrumb_index = 0; breadcrumb_index < node->BreadcrumbCount; breadcrumb_index++)
				{
					static const char *breadcrumb_names[] = 
					{
						"D3D12_AUTO_BREADCRUMB_OP_SETMARKER",
						"D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT",
						"D3D12_AUTO_BREADCRUMB_OP_ENDEVENT",
						"D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED",
						"D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED",
						"D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT",
						"D3D12_AUTO_BREADCRUMB_OP_DISPATCH",
						"D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION",
						"D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION",
						"D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE",
						"D3D12_AUTO_BREADCRUMB_OP_COPYTILES",
						"D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE",
						"D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW",
						"D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW",
						"D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW",
						"D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER",
						"D3D12_AUTO_BREADCRUMB_OP_EXECUTEBUNDLE",
						"D3D12_AUTO_BREADCRUMB_OP_PRESENT",
						"D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA",
						"D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION",
						"D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION",
						"D3D12_AUTO_BREADCRUMB_OP_DECODEFRAME",
						"D3D12_AUTO_BREADCRUMB_OP_PROCESSFRAMES",
						"D3D12_AUTO_BREADCRUMB_OP_ATOMICCOPYBUFFERUINT",
						"D3D12_AUTO_BREADCRUMB_OP_ATOMICCOPYBUFFERUINT64",
						"D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCEREGION",
						"D3D12_AUTO_BREADCRUMB_OP_WRITEBUFFERIMMEDIATE",
						"D3D12_AUTO_BREADCRUMB_OP_DECODEFRAME1",
						"D3D12_AUTO_BREADCRUMB_OP_SETPROTECTEDRESOURCESESSION",
						"D3D12_AUTO_BREADCRUMB_OP_DECODEFRAME2",
						"D3D12_AUTO_BREADCRUMB_OP_PROCESSFRAMES1",
						"D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE",
						"D3D12_AUTO_BREADCRUMB_OP_EMITRAYTRACINGACCELERATIONSTRUCTUREPOSTBUILDINFO",
						"D3D12_AUTO_BREADCRUMB_OP_COPYRAYTRACINGACCELERATIONSTRUCTURE",
						"D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS",
						"D3D12_AUTO_BREADCRUMB_OP_INITIALIZEMETACOMMAND",
						"D3D12_AUTO_BREADCRUMB_OP_EXECUTEMETACOMMAND",
						"D3D12_AUTO_BREADCRUMB_OP_ESTIMATEMOTION",
						"D3D12_AUTO_BREADCRUMB_OP_RESOLVEMOTIONVECTORHEAP",
						"D3D12_AUTO_BREADCRUMB_OP_SETPIPELINESTATE1",
						"D3D12_AUTO_BREADCRUMB_OP_INITIALIZEEXTENSIONCOMMAND",
						"D3D12_AUTO_BREADCRUMB_OP_EXECUTEEXTENSIONCOMMAND",
						"D3D12_AUTO_BREADCRUMB_OP_DISPATCHMESH",
						"D3D12_AUTO_BREADCRUMB_OP_ENCODEFRAME",
						"D3D12_AUTO_BREADCRUMB_OP_RESOLVEENCODEROUTPUTMETADATA",
					};

					D3D12_AUTO_BREADCRUMB_OP op = node->pCommandHistory[breadcrumb_index];

					const char *name = "unknown";
					if (op >= 0 && op < RT_ARRAY_COUNT(breadcrumb_names))
					{
						name = breadcrumb_names[op];
					}

					OutputDebugStringA(RT_ArenaPrintF(temp, "Breadcrumb: %s\n", name));
				}
			}

			__debugbreak();
			
			SafeRelease(dred);
		}
	}
	else
	{
		DX_CALL(hr);
	}

	// Update the previous and current back buffer indices
	g_d3d.prev_back_buffer_index = g_d3d.current_back_buffer_index;
	g_d3d.current_back_buffer_index = g_d3d.dxgi_swapchain4->GetCurrentBackBufferIndex();

	// Wait for the next back buffer to finish rendering, if it is in-flight
	frame = CurrentFrameData();
	g_d3d.command_queue_direct->WaitForFenceValue(frame->fence_value);
	// Release all stale temporary resources that have been tracked and reset the frame arena marker
	g_d3d.resource_tracker.ReleaseStaleTempResources(frame->fence_value);
	ProcessDeferredReleases(g_d3d.command_queue_direct->GetCompletedFenceValue(), false);
	RT_ArenaResetToMarker(&frame->upload_buffer_arena, frame->upload_buffer_arena_reset);
	g_d3d.cbv_srv_uav_mips.Reset();
	RT_ArenaResetAndDecommit(RT_ArenaGetThreadArena());

	g_d3d.frame_index++;
	g_d3d.accum_frame_index++;

	g_d3d_raster.tri_at = g_d3d.current_back_buffer_index * MAX_RASTER_TRIANGLES;
	g_d3d_raster.line_at = g_d3d.current_back_buffer_index * MAX_RASTER_LINES;
	g_d3d_raster.debug_line_at = g_d3d.current_back_buffer_index * MAX_DEBUG_LINES_WORLD;

	g_d3d.mesh_tracker.PruneOldEntries(g_d3d.frame_index);

    uint32_t observedWidth=g_d3d.output_width,observedHeight=g_d3d.output_height;
	RECT client_rect;
	GetClientRect(g_d3d.hWnd, &client_rect);
#ifdef D2_ENABLE_OPENVR
    if(OpenVRPrototype::FixedMirrorCanvas()) {
        ::GetClientRect(g_d3d.hWnd,&client_rect);observedWidth=g_d3d.window_width;observedHeight=g_d3d.window_height;
    }
#endif

	if (client_rect.right && client_rect.bottom &&
		((int)observedWidth != client_rect.right ||
		(int)observedHeight != client_rect.bottom))
	{
		OnWindowResize(client_rect.right, client_rect.bottom);
	}

	// Check if the fsr2 mode changed, if so, we need to adjust some of our render targets to match the output resolution
	if (tweak_vars.upscaling_aa_mode != g_d3d.upscaling_aa_mode ||
		tweak_vars.amd_fsr2_mode != g_d3d.amd_fsr2_mode)
	{
		g_d3d.upscaling_aa_mode = tweak_vars.upscaling_aa_mode;
		g_d3d.amd_fsr2_mode = tweak_vars.amd_fsr2_mode;

		Flush();

		if (g_d3d.upscaling_aa_mode == UPSCALING_AA_MODE_AMD_FSR_2_2)
		{
			FSR2::AdjustRenderResolutionForFSRMode(g_d3d.output_width, g_d3d.output_height, g_d3d.render_width, g_d3d.render_height);
		}
		else
		{
			g_d3d.render_width = g_d3d.output_width;
			g_d3d.render_height = g_d3d.output_height;
		}

		ResizeResolutionDependentResources();
	}
}

void RenderBackend::OnWindowResize(uint32_t width, uint32_t height)
{
    g_d3d.window_width=std::max(width,1u);g_d3d.window_height=std::max(height,1u);
#ifdef D2_ENABLE_OPENVR
    // DXGI stretches the fixed presentation buffers. A monitor resize must not
    // rebuild ray-tracing, FSR, eye, HUD, or reticle resources.
    if(OpenVRPrototype::FixedMirrorCanvas()) {
        uint32_t renderWidth=RT_VR_MIRROR_WIDTH,renderHeight=RT_VR_MIRROR_HEIGHT;
        if(g_d3d.upscaling_aa_mode==UPSCALING_AA_MODE_AMD_FSR_2_2)
            FSR2::AdjustRenderResolutionForFSRMode(RT_VR_MIRROR_WIDTH,RT_VR_MIRROR_HEIGHT,renderWidth,renderHeight);
        if(g_d3d.output_width!=RT_VR_MIRROR_WIDTH || g_d3d.output_height!=RT_VR_MIRROR_HEIGHT ||
           g_d3d.render_width!=renderWidth || g_d3d.render_height!=renderHeight) {
            Flush();g_d3d.output_width=RT_VR_MIRROR_WIDTH;g_d3d.output_height=RT_VR_MIRROR_HEIGHT;
            g_d3d.render_width=renderWidth;g_d3d.render_height=renderHeight;
            ResizeResolutionDependentResources();
        }
        return;
    }
#endif
	Flush();

	width = std::max(width, 1u);
	height = std::max(height, 1u);

	g_d3d.output_width = g_d3d.render_width = width;
	g_d3d.output_height = g_d3d.render_height = height;

	FSR2::OnWindowResize(g_d3d.output_width, g_d3d.output_height);

	if (g_d3d.upscaling_aa_mode == UPSCALING_AA_MODE_AMD_FSR_2_2)
	{
		FSR2::AdjustRenderResolutionForFSRMode(g_d3d.output_width, g_d3d.output_height, g_d3d.render_width, g_d3d.render_height);
	}
	else
	{
		g_d3d.render_width = width;
		g_d3d.render_height = height;
	}
	
	for (size_t i = 0; i < BACK_BUFFER_COUNT_SWAP; ++i)
	{
		RT_RELEASE_RESOURCE(g_d3d.back_buffers[i]);
		g_d3d.frame_data[i].fence_value = g_d3d.frame_data[g_d3d.current_back_buffer_index].fence_value;
	}

	DXGI_SWAP_CHAIN_DESC swap_chain_desc = {};
	DX_CALL(g_d3d.dxgi_swapchain4->GetDesc(&swap_chain_desc));
	DX_CALL(g_d3d.dxgi_swapchain4->ResizeBuffers(BACK_BUFFER_COUNT_SWAP, g_d3d.output_width, g_d3d.output_height,
		swap_chain_desc.BufferDesc.Format, swap_chain_desc.Flags));

	for (UINT i = 0; i < BACK_BUFFER_COUNT_SWAP; ++i)
	{
		DX_CALL(g_d3d.dxgi_swapchain4->GetBuffer(i, IID_PPV_ARGS(&g_d3d.back_buffers[i])));
		RT_TRACK_RESOURCE(g_d3d.back_buffers[i], D3D12_RESOURCE_STATE_PRESENT);
	}

	g_d3d.current_back_buffer_index = g_d3d.dxgi_swapchain4->GetCurrentBackBufferIndex();

	ResizeResolutionDependentResources();
}

RT_RendererIO *RenderBackend::GetIO()
{
	return &g_d3d.io;
}

int RenderBackend::CheckWindowMinimized()
{
	if (g_d3d.render_width <= 1 && g_d3d.render_height <= 1)
		return 1;

	return 0;
}

// Clear derived flags before a texture can be modified or its descriptor reused.
static void InvalidateTextureOpacity(TextureResource* texture) {
    if(!texture || !texture->known_opaque) return;
    texture->known_opaque=false;
    if(g_d3d.material_buffer && g_d3d.material_buffer_cpu) {
        for(unsigned i=0;i<RT_MAX_MATERIALS;++i) {
            uint32_t& flags=g_d3d.material_flags_shadow[i];
            if(g_d3d.material_albedo_shadow[i]==texture->descriptors.heap_offset && (flags&MaterialFlag_AlbedoOpaque)) {
                flags&=~MaterialFlag_AlbedoOpaque;
                g_d3d.material_buffer_cpu[i].flags=flags;
            }
        }
    }
}

RT_ResourceHandle RenderBackend::UploadTexture(const RT_UploadTextureParams& texture_params)
{
	RT::MemoryScope temp;

	RT_Image image = texture_params.image;

	if (!image.pixels)
		return g_d3d.pink_checkerboard_texture;

	TextureResource resource = {};
    resource.known_opaque=TextureHasOpaqueAlpha(image);

	char *texture_resource_name = RT_ArenaPrintF(temp, "Texture: %s", texture_params.name);
    static int n_textures_uploaded_please_remove_me_later = 0;
    printf("textures loaded: %d\n", ++n_textures_uploaded_please_remove_me_later);

	bool        is_dxt_format = false;
	DXGI_FORMAT format        = DXGI_FORMAT_R8G8B8A8_UNORM;
	switch (image.format)
	{
		case RT_TextureFormat_RGBA8:      format = DXGI_FORMAT_R8G8B8A8_UNORM;                              break;
		case RT_TextureFormat_RGBA8_SRGB: format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;                         break;
		case RT_TextureFormat_R8:         format = DXGI_FORMAT_R8_UNORM;                                    break;
		case RT_TextureFormat_BC1:        format = DXGI_FORMAT_BC1_UNORM;             is_dxt_format = true; break;
		case RT_TextureFormat_BC1_SRGB:   format = DXGI_FORMAT_BC1_UNORM_SRGB;        is_dxt_format = true; break;
		case RT_TextureFormat_BC2:        format = DXGI_FORMAT_BC2_UNORM;             is_dxt_format = true; break;
		case RT_TextureFormat_BC2_SRGB:   format = DXGI_FORMAT_BC2_UNORM_SRGB;        is_dxt_format = true; break;
		case RT_TextureFormat_BC3:        format = DXGI_FORMAT_BC3_UNORM;             is_dxt_format = true; break;
		case RT_TextureFormat_BC3_SRGB:   format = DXGI_FORMAT_BC3_UNORM_SRGB;        is_dxt_format = true; break;
		case RT_TextureFormat_BC4:        format = DXGI_FORMAT_BC4_UNORM;             is_dxt_format = true; break;
		case RT_TextureFormat_BC5:        format = DXGI_FORMAT_BC5_UNORM;             is_dxt_format = true; break;
		case RT_TextureFormat_BC7:        format = DXGI_FORMAT_BC7_UNORM;             is_dxt_format = true; break;
		case RT_TextureFormat_BC7_SRGB:   format = DXGI_FORMAT_BC7_UNORM_SRGB;        is_dxt_format = true; break;
	}

	size_t bits_per_pixel = DDSBitsPerPixel(format);

	if (!image.pitch)
	{
		image.pitch = (uint32_t)( ( image.width * bits_per_pixel ) / 8 );
	}

	uint32_t resource_mip_count = RT_MAX(1, image.mip_count);
	uint32_t upload_mip_count   = RT_MAX(1, image.mip_count);

	bool generate_mips = !is_dxt_format && upload_mip_count <= 1 && !(texture_params.flags & RT_TextureFlag_NoMips);

	if (generate_mips)
	{
		resource_mip_count = RT_U32Log2(RT_MIN(image.width, image.height));
	}
	else if (is_dxt_format)
	{
		uint32_t max_mip_count = RT_U32Log2(RT_MIN(image.width, image.height));
		uint32_t min_mip_level = 6; // log2(64) - (this is the minimum mip map size we want to use.  Below 64px dds files start to get packed inefficiently into memory and the visual difference is not noticeable.)

		if (max_mip_count > min_mip_level)
		{
			upload_mip_count = RT_MIN(upload_mip_count, max_mip_count - min_mip_level);
			resource_mip_count = upload_mip_count;
		}
	}

	resource.texture = RT_CreateTexture(
		Utf16FromUtf8(temp, texture_resource_name), 
		format,
		D3D12_RESOURCE_FLAG_NONE,
		(size_t)image.width, 
		(uint32_t)image.height,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, 
		(uint16_t)resource_mip_count);

	UploadTextureData(resource.texture, image.width, image.height, bits_per_pixel, image.mips, upload_mip_count);

	if (generate_mips)
	{
		GenerateMips(&resource);
	}

	resource.descriptors = g_d3d.cbv_srv_uav.Allocate(1);
	CreateTextureSRV(resource.texture, resource.descriptors.GetCPUDescriptor(0), format, resource_mip_count);

	return g_texture_slotmap.Insert(resource);
}

RT_ResourceHandle RenderBackend::UploadMesh(const RT_UploadMeshParams& mesh_params)
{
	MemoryScope temp_arena;

	CreateMeshBuffersResult result;
	CreateMeshBuffers(mesh_params.triangle_count, mesh_params.triangles, &result);

	static uint32_t triangle_buffer_index = 0;
	result.triangle_buffer->SetName(Utf16FromUtf8(temp_arena, RT_ArenaPrintF(temp_arena, "Triangle Buffer %u (%s)", triangle_buffer_index++, mesh_params.name)));

	ID3D12Resource* blas = BuildBLAS(mesh_params.triangle_count, mesh_params.triangles, D3D12_RAYTRACING_GEOMETRY_FLAG_NONE);

	static uint32_t blas_index = 0;
	blas->SetName(Utf16FromUtf8(temp_arena, RT_ArenaPrintF(temp_arena, "BLAS %u (%s)", blas_index++, mesh_params.name)));

	MeshResource resource = {};
	resource.blas = blas;
	resource.triangle_buffer = result.triangle_buffer;
	resource.triangle_buffer_descriptor = result.triangle_buffer_descriptor;

	return g_mesh_slotmap.Insert(resource);
}

void RenderBackend::ReleaseTexture(const RT_ResourceHandle texture_handle)
{
	if (texture_handle.value == g_d3d.black_texture_handle.value || texture_handle.value == g_d3d.white_texture_handle.value)
		return;

	TextureResource* texture_resource = g_texture_slotmap.Find(texture_handle);

    InvalidateTextureOpacity(texture_resource);
	if (texture_resource)
	{
		// Freed after the frames that may still sample it have finished.
		QueueDeferredRelease(texture_resource->texture, nullptr, &texture_resource->descriptors);
		g_texture_slotmap.Remove(texture_handle);
	}
}

void RenderBackend::ReleaseMesh(const RT_ResourceHandle mesh_handle)
{
	MeshResource* mesh_resource = g_mesh_slotmap.Find(mesh_handle);
	if (!mesh_resource)
		return;
	// Freed after the frames that may still trace it have finished.
	QueueDeferredRelease(mesh_resource->triangle_buffer, mesh_resource->blas, &mesh_resource->triangle_buffer_descriptor);
	g_mesh_slotmap.Remove(mesh_handle);
}

uint16_t RenderBackend::UpdateMaterial(uint16_t material_index, const RT_Material *material)
{
	uint16_t result = UINT16_MAX;

	if (ALWAYS(material_index < RT_MAX_MATERIALS))
	{
		TextureResource *albedo = g_texture_slotmap.Find(material->albedo_texture);

		if (!albedo)
			albedo = g_d3d.white_texture;

		TextureResource *normal = g_texture_slotmap.Find(material->normal_texture);

		if (!normal)
			normal = g_d3d.default_normal_texture;

		TextureResource *metalness = g_texture_slotmap.Find(material->metalness_texture);

		if (!metalness)
			metalness = g_d3d.white_texture;

		TextureResource *roughness = g_texture_slotmap.Find(material->roughness_texture);

		if (!roughness)
			roughness = g_d3d.white_texture;

		TextureResource *emissive = g_texture_slotmap.Find(material->emissive_texture);

		if (!emissive)
			emissive = g_d3d.black_texture;

		RT_Vec3 emissive_factor = material->emissive_color;
		emissive_factor = RT_Vec3Muls(emissive_factor, material->emissive_strength);

		Material *gpu_material = &g_d3d.material_buffer_cpu[material_index];
		gpu_material->albedo_index     = albedo->descriptors.heap_offset;
		gpu_material->normal_index     = normal->descriptors.heap_offset;
		gpu_material->metalness_index  = metalness->descriptors.heap_offset;
		gpu_material->roughness_index  = roughness->descriptors.heap_offset;
		gpu_material->emissive_index   = emissive->descriptors.heap_offset;
		gpu_material->metalness_factor = material->metalness;
		gpu_material->roughness_factor = material->roughness;
		gpu_material->emissive_factor  = RT_PackRGBE(emissive_factor);
		const uint32_t flags=(material->flags & ~MaterialFlag_AlbedoOpaque) | (albedo->known_opaque ? MaterialFlag_AlbedoOpaque : 0);
        g_d3d.material_flags_shadow[material_index]=flags;
        g_d3d.material_albedo_shadow[material_index]=albedo->descriptors.heap_offset;
        gpu_material->flags=flags;

		result = material_index;
	}

	return result;
}

RT_ResourceHandle RenderBackend::GetDefaultWhiteTexture()
{
	return g_d3d.white_texture_handle;
}

RT_ResourceHandle RenderBackend::GetDefaultBlackTexture()
{
	return g_d3d.black_texture_handle;
}

RT_ResourceHandle RenderBackend::GetBillboardMesh()
{
	return g_d3d.billboard_quad;
}

RT_ResourceHandle RenderBackend::GetCubeMesh()
{
	return g_d3d.cube;
}

void RenderBackend::RaytraceSubmitLights(size_t count, const RT_Light* lights)
{
	if (g_d3d.scene.freezeframe)
		return;

	if (NEVER(g_d3d.lights_count + count > RT_MAX_LIGHTS))
		count = RT_MAX_LIGHTS - g_d3d.lights_count;

	if (count > 0)
	{
		FrameData* frame = CurrentFrameData();

		RT_Light* dst_lights = frame->lights.As<RT_Light>() + g_d3d.lights_count;
		memcpy(dst_lights, lights, sizeof(RT_Light) * count);

		g_d3d.lights_count += (uint32_t)count;
	}
}

void RenderBackend::RaytraceSetVerticalOffset(const float new_offset) {
	g_d3d.viewport_offset_y = new_offset;
}

float RenderBackend::RaytraceGetVerticalOffset() {
	return g_d3d.viewport_offset_y;
}

uint32_t RenderBackend::RaytraceGetCurrentLightCount() {
	return g_d3d.lights_count;
}

uint32_t RenderBackend::RaytraceGetCurrentInstanceCount() {
	return g_d3d.tlas_instance_count;
}

uint32_t RenderBackend::RaytraceGetInstanceCapacity() {
	return MAX_INSTANCES;
}

void RenderBackend::RaytraceMesh(const RT_RenderMeshParams& params)
{
	if (g_d3d.scene.freezeframe)
		return;

	if (ALWAYS(g_d3d.tlas_instance_count < MAX_INSTANCES))
	{
		FrameData *frame = CurrentFrameData();
		MeshResource* mesh_resource = g_mesh_slotmap.Find(params.mesh_handle);

		if (!mesh_resource)
		{
			mesh_resource = g_mesh_slotmap.Find(g_d3d.cube);
		}

		// ------------------------------------------------------------------
		// Add instance description and data

		uint32_t instance_index = g_d3d.tlas_instance_count++;
#ifdef D2_ENABLE_OPENVR
        frame->raster_vertex_counts[instance_index]=static_cast<uint32_t>(mesh_resource->triangle_buffer->GetDesc().Width/sizeof(RT_Triangle))*3;
#endif

		D3D12_RAYTRACING_INSTANCE_DESC *instance_desc = frame->instance_descs.As<D3D12_RAYTRACING_INSTANCE_DESC>() + instance_index;
		instance_desc->InstanceMask = params.instance_mask ? params.instance_mask : RT_RENDER_MASK_OBJECTS;
		memcpy(instance_desc->Transform, params.transform, sizeof(float)*12);
		instance_desc->AccelerationStructure = mesh_resource->blas->GetGPUVirtualAddress();
		instance_desc->InstanceContributionToHitGroupIndex = 0;
		instance_desc->Flags = 0;

		if (params.flags & RT_RenderMeshFlags_ReverseCulling)
		{
			instance_desc->Flags |= D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE;
		}

		TrackedMeshData *tracked = nullptr;
		bool tracked_data_valid = g_d3d.mesh_tracker.GetTrackedMeshData(params.key.value, g_d3d.frame_index, &tracked);

		InstanceData *instance_data = frame->instance_data.As<InstanceData>() + instance_index;
		instance_data->object_to_world = *params.transform;
		instance_data->world_to_object = RT_Mat4Inverse(*params.transform);

		if (params.prev_transform)
		{
			instance_data->object_to_world_prev = *params.prev_transform;
		}
		else
		{
			if (tracked_data_valid && !(params.flags & RT_RenderMeshFlags_Teleport))
			{
				instance_data->object_to_world_prev = tracked->prev_transform;
			}
			else
			{
				instance_data->object_to_world_prev = *params.transform;
			}
		}

		instance_data->material_override = params.material_override;
		instance_data->material_color = params.color;
		instance_data->triangle_buffer_idx = mesh_resource->triangle_buffer_descriptor.heap_offset;
		instance_data->instance_mask = instance_desc->InstanceMask;

		tracked->prev_transform = *params.transform;
	}
}

void RenderBackend::RaytraceBillboardColored(uint16_t material_index, RT_Vec3 color, RT_Vec2 dim, RT_Vec3 pos, RT_Vec3 prev_pos)

{
	RaytraceBillboardColoredAlpha(material_index, color, 1.0f, dim, pos, prev_pos);
}

void RenderBackend::RaytraceBillboardColoredAlpha(uint16_t material_index, RT_Vec3 color, float alpha, RT_Vec2 dim, RT_Vec3 pos, RT_Vec3 prev_pos)
{
	RT_RenderMeshParams params = {};
	RT_Mat4 transform;
	{
		RT_Vec3 x = RT_Vec3Muls(g_d3d.scene.camera.right, dim.x);
		RT_Vec3 y = RT_Vec3Muls(g_d3d.scene.camera.up, dim.y);
		RT_Vec3 z = RT_Vec3Negate(g_d3d.scene.camera.forward);
		transform = RT_Mat4FromBasisVectors(x, y, z);
		transform = RT_Mat4Mul(RT_Mat4FromTranslation(pos), transform);
	}

	RT_Mat4 prev_transform;
	{
		RT_Vec3 prev_x = RT_Vec3Muls(g_d3d.scene.prev_camera.right, dim.x);
		RT_Vec3 prev_y = RT_Vec3Muls(g_d3d.scene.prev_camera.up, dim.y);
		RT_Vec3 prev_z = RT_Vec3Negate(g_d3d.scene.prev_camera.forward);
		prev_transform = RT_Mat4FromBasisVectors(prev_x, prev_y, prev_z);
		prev_transform = RT_Mat4Mul(RT_Mat4FromTranslation(prev_pos), prev_transform);
	}

	RT_Vec4 color4 = RT_Vec4Make(color.x, color.y, color.z, alpha);

	params.mesh_handle = g_d3d.billboard_quad;
	params.transform = &transform;
	params.prev_transform = &prev_transform;
	params.color = RT_PackRGBA(color4);
	params.material_override = material_index;
	RaytraceMesh(params);
}

void RenderBackend::RaytraceRod(uint16_t material_index, RT_Vec3 bot_p, RT_Vec3 top_p, float width)
{
	RT_Mat4 transform;
	{
		RT_Vec3 z = RT_Vec3Negate(g_d3d.scene.camera.forward);

		RT_Vec3 y = RT_Vec3Muls(RT_Vec3Sub(top_p, bot_p), 0.5f);
		RT_Vec3 x = RT_Vec3Normalize(RT_Vec3Cross(y, z));
		x = RT_Vec3Muls(x, width);

		z = RT_Vec3Normalize(RT_Vec3Cross(x, y));

		RT_Vec3 pos = RT_Vec3Muls(RT_Vec3Add(bot_p, top_p), 0.5f);

		transform = RT_Mat4FromBasisVectors(x, y, z);
		transform = RT_Mat4Mul(RT_Mat4FromTranslation(pos), transform);
	}

	RT_RenderMeshParams params = {};
	params.mesh_handle = g_d3d.billboard_quad;
	params.transform = &transform;
	params.prev_transform = &transform;
	params.material_override = material_index;	
	params.color = 0xFFFFFFFF;

	RaytraceMesh(params);
}

void RenderBackend::RaytraceRender()
{
    // Non-VR captures retain the established previous-desktop-frame timing.
    // Eye passes must leave the request pending until monitor composition.
#ifdef D2_ENABLE_OPENVR
    if(!OpenVRPrototype::Active() && !g_d3d.scene.render_blit)
#else
    if(!g_d3d.scene.render_blit)
#endif
        CaptureDesktopScreenshot(g_d3d.rt.color_final);

	FrameData* frame = CurrentFrameData();
	auto& rt = g_d3d.scene.render_blit ? g_d3d.rt_blit : g_d3d.rt;
	auto* render_targets = g_d3d.scene.render_blit ? g_d3d.render_targets_blit : g_d3d.render_targets;

	// Need to flush the ring buffer here for any pending copies
	FlushRingBuffer(&g_d3d.resource_upload_ring_buffer);

	// Shader hot-reload stats ~30 files on disk. Doing that for every view
	// (each cockpit camera too) costs CPU every frame; twice a second is
	// plenty for live editing. Changed shader defines still reload at once.
	static ULONGLONG last_shader_check = 0;
	const ULONGLONG shader_check_now = GetTickCount64();
	const bool shader_check_due = last_shader_check == 0 || shader_check_now - last_shader_check >= 500 ||
		g_shader_defines.last_modified_time != g_d3d.global_shader_defines.last_modified_time;
#ifdef D2_ENABLE_OPENVR
	if (shader_check_due && (!OpenVRPrototype::EyePass() || OpenVRPrototype::FirstEye()))
#else
	if (shader_check_due)
#endif
	{
		last_shader_check = shader_check_now;
		ReloadComputeShadersIfThereAreNewOnes();
		ReloadAllTheRaytracingStateAsNecessary(false);
	}

	CommandList& command_list = g_d3d.command_queue_direct->GetCommandList();
#ifdef D2_ENABLE_OPENVR
	OpenVRPrototype::BeginEyeGpuTiming(command_list.GetD3D12CommandList());
	const bool reuseStereoScene = OpenVRPrototype::ReuseScene();
#else
	const bool reuseStereoScene = false;
#endif
	GPUProfiler::BeginTimestampQuery(command_list, GPUProfiler::GPUTimer_FrameTime);
	ResourceTransition(command_list, rt.color_final, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	// ------------------------------------------------------------------

	GPUProfiler::BeginTimestampQuery(command_list, GPUProfiler::GPUTimer_BuildTLAS);
	if (!reuseStereoScene) BuildTLAS(command_list);
	GPUProfiler::EndTimestampQuery(command_list, GPUProfiler::GPUTimer_BuildTLAS);

	// ------------------------------------------------------------------
	// Copy this frame's dynamic material state:

	if (!reuseStereoScene) {
	memcpy(frame->material_edges.cpu, g_rt_material_edges, sizeof(RT_MaterialEdge) * RT_MAX_MATERIAL_EDGES);
	memcpy(frame->material_indices.cpu, g_rt_material_indices, sizeof(uint16_t) * RT_MAX_MATERIALS);

	CopyBufferRegion(command_list, g_d3d.material_edges, 0, frame->material_edges.buffer, frame->material_edges.offset, frame->material_edges.size);
	CopyBufferRegion(command_list, g_d3d.material_indices, 0, frame->material_indices.buffer, frame->material_indices.offset, frame->material_indices.size);
	}
	

	// ------------------------------------------------------------------
	// Create unordered access views for render targets

	D3D12GlobalDescriptors clearDescriptors[RenderTarget_COUNT]={};
	auto CreateRenderTargetUAV = [&](RenderTarget rt, D3D12GlobalDescriptors descriptor)
	{
        clearDescriptors[rt]=descriptor;
		{
			D3D12_CPU_DESCRIPTOR_HANDLE uav = frame->descriptors.GetCPUDescriptor(descriptor);
			CreateTextureUAV(render_targets[rt], uav, g_d3d.render_target_formats[rt]);
		}

		{
			// this is required for ClearUnorderedAccessViewFloat, only need it for clearing color right now
			D3D12_CPU_DESCRIPTOR_HANDLE uav = frame->non_shader_descriptors.GetCPUDescriptor(descriptor);
			CreateTextureUAV(render_targets[rt], uav, g_d3d.render_target_formats[rt]);
		}
	};

	// alternate which textures is used for current and previous

	// TODO(daniel): It would be nice to figure out a way to do this automatically from the
	// macro, with the ping-ponging handled cleanly.

	uint64_t history_index = g_d3d.frame_index;
#ifdef D2_ENABLE_OPENVR
	// Each eye owns its history targets and only advances when it is rendered.
	// The global frame index also advances on menu/paused frames without a
	// stereo render, which would leave an eye reading a two-frame-old history.
	if (OpenVRPrototype::EyePass()) history_index = OpenVRPrototype::EyeHistoryIndex();
#endif
	// Same for each cockpit monitor, which has its own target set.
	if (g_active_camera_view >= 0) history_index = g_camera_history[g_active_camera_view];
	uint32_t a = (uint32_t)((history_index + 0) & 1);
	uint32_t b = (uint32_t)((history_index + 1) & 1);

	RenderTarget rt_specular[2] = { RenderTarget_spec,         RenderTarget_spec_hist };
	RenderTarget rt_normal[2] = { RenderTarget_normal, RenderTarget_normal_prev };
	RenderTarget rt_view_dir[2] = { RenderTarget_view_dir, RenderTarget_view_dir_prev };
	RenderTarget rt_depth[2] = { RenderTarget_depth, RenderTarget_depth_prev };
	RenderTarget rt_metallic[2] = { RenderTarget_metallic,	   RenderTarget_metallic_prev };
	RenderTarget rt_roughness[2] = { RenderTarget_roughness,	   RenderTarget_roughness_prev };
	RenderTarget rt_material[2] = { RenderTarget_material,	   RenderTarget_material_prev };
	RenderTarget rt_moments[2] = { RenderTarget_moments,      RenderTarget_moments_hist };
	RenderTarget rt_taa_result[2] = { RenderTarget_taa_result,   RenderTarget_taa_history };
	RenderTarget rt_diff_stable[2] = { RenderTarget_diff_stable,  RenderTarget_diff_stable_hist };
	RenderTarget rt_spec_stable[2] = { RenderTarget_spec_stable,  RenderTarget_spec_stable_hist };

	CreateRenderTargetUAV(RenderTarget_color, D3D12GlobalDescriptors_UAV_color);
	CreateRenderTargetUAV(RenderTarget_albedo, D3D12GlobalDescriptors_UAV_albedo);
	CreateRenderTargetUAV(RenderTarget_emissive, D3D12GlobalDescriptors_UAV_emissive);
	CreateRenderTargetUAV(RenderTarget_diff, D3D12GlobalDescriptors_UAV_diff);
	CreateRenderTargetUAV(RenderTarget_diff_hist, D3D12GlobalDescriptors_UAV_diff_hist);
	CreateRenderTargetUAV(RenderTarget_diff_denoise_ping, D3D12GlobalDescriptors_UAV_diff_denoise_ping);
	CreateRenderTargetUAV(RenderTarget_diff_denoise_pong, D3D12GlobalDescriptors_UAV_diff_denoise_pong);
	CreateRenderTargetUAV(rt_diff_stable[a], D3D12GlobalDescriptors_UAV_diff_stable);
	CreateRenderTargetUAV(rt_diff_stable[b], D3D12GlobalDescriptors_UAV_diff_stable_hist);
	CreateRenderTargetUAV(RenderTarget_spec, D3D12GlobalDescriptors_UAV_spec);
	CreateRenderTargetUAV(RenderTarget_spec_hist, D3D12GlobalDescriptors_UAV_spec_hist);
	CreateRenderTargetUAV(RenderTarget_spec_denoise_ping, D3D12GlobalDescriptors_UAV_spec_denoise_ping);
	CreateRenderTargetUAV(RenderTarget_spec_denoise_pong, D3D12GlobalDescriptors_UAV_spec_denoise_pong);
	CreateRenderTargetUAV(rt_spec_stable[a], D3D12GlobalDescriptors_UAV_spec_stable);
	CreateRenderTargetUAV(rt_spec_stable[b], D3D12GlobalDescriptors_UAV_spec_stable_hist);
	CreateRenderTargetUAV(rt_normal[a], D3D12GlobalDescriptors_UAV_normal);
	CreateRenderTargetUAV(rt_normal[b], D3D12GlobalDescriptors_UAV_normal_prev);
	CreateRenderTargetUAV(rt_view_dir[a], D3D12GlobalDescriptors_UAV_view_dir);
	CreateRenderTargetUAV(rt_view_dir[b], D3D12GlobalDescriptors_UAV_view_dir_prev);
	CreateRenderTargetUAV(rt_depth[a], D3D12GlobalDescriptors_UAV_depth);
	CreateRenderTargetUAV(rt_depth[b], D3D12GlobalDescriptors_UAV_depth_prev);
	CreateRenderTargetUAV(RenderTarget_restir_yM, D3D12GlobalDescriptors_UAV_restir_yM);
	CreateRenderTargetUAV(RenderTarget_restir_weights, D3D12GlobalDescriptors_UAV_restir_weights);
	CreateRenderTargetUAV(RenderTarget_visibility_prim, D3D12GlobalDescriptors_UAV_visibility_prim);
	CreateRenderTargetUAV(RenderTarget_visibility_bary, D3D12GlobalDescriptors_UAV_visibility_bary);
	CreateRenderTargetUAV(rt_metallic[a], D3D12GlobalDescriptors_UAV_metallic);
	CreateRenderTargetUAV(rt_metallic[b], D3D12GlobalDescriptors_UAV_metallic_prev);
	CreateRenderTargetUAV(rt_roughness[a], D3D12GlobalDescriptors_UAV_roughness);
	CreateRenderTargetUAV(rt_roughness[b], D3D12GlobalDescriptors_UAV_roughness_prev);
	CreateRenderTargetUAV(rt_material[a], D3D12GlobalDescriptors_UAV_material);
	CreateRenderTargetUAV(rt_material[b], D3D12GlobalDescriptors_UAV_material_prev);
	CreateRenderTargetUAV(RenderTarget_history_length, D3D12GlobalDescriptors_UAV_history_length);
	CreateRenderTargetUAV(RenderTarget_motion, D3D12GlobalDescriptors_UAV_motion);
	CreateRenderTargetUAV(rt_moments[a], D3D12GlobalDescriptors_UAV_moments);
	CreateRenderTargetUAV(rt_moments[b], D3D12GlobalDescriptors_UAV_moments_hist);
	CreateRenderTargetUAV(RenderTarget_moments_denoise_ping, D3D12GlobalDescriptors_UAV_moments_denoise_ping);
	CreateRenderTargetUAV(RenderTarget_moments_denoise_pong, D3D12GlobalDescriptors_UAV_moments_denoise_pong);
	CreateRenderTargetUAV(RenderTarget_denoise_pong, D3D12GlobalDescriptors_UAV_denoise_pong);
	CreateRenderTargetUAV(rt_taa_result[a], D3D12GlobalDescriptors_UAV_taa_result);
	CreateRenderTargetUAV(rt_taa_result[b], D3D12GlobalDescriptors_UAV_taa_history);
	CreateRenderTargetUAV(RenderTarget_bloom_pong, D3D12GlobalDescriptors_UAV_bloom_pong);
	CreateRenderTargetUAV(RenderTarget_bloom_prepass, D3D12GlobalDescriptors_UAV_bloom_prepass);
	CreateRenderTargetUAV(RenderTarget_bloom0, D3D12GlobalDescriptors_UAV_bloom0);
	CreateRenderTargetUAV(RenderTarget_bloom1, D3D12GlobalDescriptors_UAV_bloom1);
	CreateRenderTargetUAV(RenderTarget_bloom2, D3D12GlobalDescriptors_UAV_bloom2);
	CreateRenderTargetUAV(RenderTarget_bloom3, D3D12GlobalDescriptors_UAV_bloom3);
	CreateRenderTargetUAV(RenderTarget_bloom4, D3D12GlobalDescriptors_UAV_bloom4);
	CreateRenderTargetUAV(RenderTarget_bloom5, D3D12GlobalDescriptors_UAV_bloom5);
	CreateRenderTargetUAV(RenderTarget_bloom6, D3D12GlobalDescriptors_UAV_bloom6);
	CreateRenderTargetUAV(RenderTarget_bloom7, D3D12GlobalDescriptors_UAV_bloom7);
	CreateRenderTargetUAV(RenderTarget_postfx, D3D12GlobalDescriptors_UAV_postfx);
	CreateRenderTargetUAV(RenderTarget_resolve, D3D12GlobalDescriptors_UAV_resolve);
	CreateRenderTargetUAV(RenderTarget_color_reference, D3D12GlobalDescriptors_UAV_color_reference);
	CreateRenderTargetUAV(RenderTarget_color_final, D3D12GlobalDescriptors_UAV_color_final);
	CreateRenderTargetUAV(RenderTarget_debug, D3D12GlobalDescriptors_UAV_debug);
	CreateRenderTargetUAV(RenderTarget_fsr2_reactive_mask, D3D12GlobalDescriptors_UAV_fsr2_reactive_mask);
    CreateRenderTargetUAV(RenderTarget_fsr_depth,D3D12GlobalDescriptors_UAV_fsr_depth);

	auto CreateRenderTargetSRV = [&](RenderTarget rt, D3D12GlobalDescriptors descriptor)
	{
		RT_ASSERT((descriptor >= D3D12GlobalDescriptors_SRV_START && descriptor < D3D12GlobalDescriptors_CBV_START) ||
			(descriptor >= D3D12GlobalDescriptors_SRV_RT_START && descriptor < D3D12GlobalDescriptors_COUNT));
		CreateTextureSRV(render_targets[rt], frame->descriptors.GetCPUDescriptor(descriptor), g_d3d.render_target_formats[rt]);
	};

	CreateRenderTargetSRV(rt_taa_result[a], D3D12GlobalDescriptors_SRV_taa_result);
	CreateRenderTargetSRV(rt_taa_result[b], D3D12GlobalDescriptors_SRV_taa_history);
	CreateRenderTargetSRV(rt_diff_stable[b], D3D12GlobalDescriptors_SRV_diff_stable_hist);
	CreateRenderTargetSRV(rt_spec_stable[b], D3D12GlobalDescriptors_SRV_spec_stable_hist);
	CreateRenderTargetSRV(RenderTarget_bloom0, D3D12GlobalDescriptors_SRV_bloom0);
	CreateRenderTargetSRV(RenderTarget_bloom1, D3D12GlobalDescriptors_SRV_bloom1);
	CreateRenderTargetSRV(RenderTarget_bloom2, D3D12GlobalDescriptors_SRV_bloom2);
	CreateRenderTargetSRV(RenderTarget_bloom3, D3D12GlobalDescriptors_SRV_bloom3);
	CreateRenderTargetSRV(RenderTarget_bloom4, D3D12GlobalDescriptors_SRV_bloom4);
	CreateRenderTargetSRV(RenderTarget_bloom5, D3D12GlobalDescriptors_SRV_bloom5);
	CreateRenderTargetSRV(RenderTarget_bloom6, D3D12GlobalDescriptors_SRV_bloom6);
	CreateRenderTargetSRV(RenderTarget_bloom7, D3D12GlobalDescriptors_SRV_bloom7);
    CreateRenderTargetSRV(RenderTarget_fsr_depth,D3D12GlobalDescriptors_SRV_fsr_depth);
	ResourceTransition(command_list, rt.resolve, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	// ------------------------------------------------------------------
	// Create unordered access view for the pixel debug buffer

	{
		D3D12_CPU_DESCRIPTOR_HANDLE uav = frame->descriptors.GetCPUDescriptor(D3D12GlobalDescriptors_UAV_PixelDebugBuffer);
		UINT element_count = static_cast<UINT>(g_d3d.pixel_debug.uav_resource->GetDesc().Width/sizeof(PixelDebugData));
		CreateBufferUAV(g_d3d.pixel_debug.uav_resource, uav, 0, element_count, sizeof(PixelDebugData));
	}

	// ------------------------------------------------------------------
	// Create shader resource view for the acceleration structure

	{
		D3D12_CPU_DESCRIPTOR_HANDLE srv = frame->descriptors.GetCPUDescriptor(D3D12GlobalDescriptors_SRV_AccelerationStructure);
		ID3D12Resource* tlas=frame->top_level_as;
#ifdef D2_ENABLE_OPENVR
		if (reuseStereoScene) tlas=OpenVRPrototype::SharedTLAS();
#endif
		CreateAccelerationStructureSRV(tlas, srv);
	}

	// ------------------------------------------------------------------
	// Create shader resource view for the lights structure and copy lights

	{
		if (!reuseStereoScene && !g_d3d.scene.freezeframe && g_d3d.lights_count > 0)
			CopyBufferRegion(command_list, g_d3d.lights_buffer, 0, frame->lights.buffer, frame->lights.offset, sizeof(RT_Light) * g_d3d.lights_count);

		D3D12_CPU_DESCRIPTOR_HANDLE srv = frame->descriptors.GetCPUDescriptor(D3D12GlobalDescriptors_SRV_LightBuffer);
		CreateBufferSRV(g_d3d.lights_buffer, srv, 0, RT_MAX_LIGHTS, sizeof(RT_Light));
	}

	// ------------------------------------------------------------------
	// Create SRVs for materials

	{
		D3D12_CPU_DESCRIPTOR_HANDLE srv = frame->descriptors.GetCPUDescriptor(D3D12GlobalDescriptors_SRV_Materials);
		CreateBufferSRV(g_d3d.material_buffer, srv, 0, RT_MAX_MATERIALS, sizeof(Material));
	}

	{
		D3D12_CPU_DESCRIPTOR_HANDLE srv = frame->descriptors.GetCPUDescriptor(D3D12GlobalDescriptors_SRV_MaterialEdges);

		// used as ByteAddressBuffer
		UINT raw_size = RT_ALIGN_POW2(sizeof(RT_MaterialEdge) * RT_MAX_MATERIAL_EDGES, 4) / 4;
		CreateBufferSRV(g_d3d.material_edges, srv, 0, raw_size, 0);
	}

	{
		D3D12_CPU_DESCRIPTOR_HANDLE srv = frame->descriptors.GetCPUDescriptor(D3D12GlobalDescriptors_SRV_MaterialIndices);

		// used as ByteAddressBuffer
		UINT raw_size = RT_ALIGN_POW2(sizeof(uint16_t) * RT_MAX_MATERIALS, 4) / 4;
		CreateBufferSRV(g_d3d.material_indices, srv, 0, raw_size, 0);
	}

	// ------------------------------------------------------------------
	// Create SRV for instance data buffer and copy

	{
		if (!reuseStereoScene && !g_d3d.scene.freezeframe && g_d3d.tlas_instance_count > 0)
			CopyBufferRegion(command_list, g_d3d.instance_data_buffer, 0, frame->instance_data.buffer, frame->instance_data.offset, sizeof(InstanceData) * g_d3d.tlas_instance_count);

		D3D12_CPU_DESCRIPTOR_HANDLE srv = frame->descriptors.GetCPUDescriptor(D3D12GlobalDescriptors_SRV_InstanceDataBuffer);
		CreateBufferSRV(g_d3d.instance_data_buffer, srv, 0, MAX_INSTANCES, sizeof(InstanceData));
	}

	// ------------------------------------------------------------------
	// Set descriptor heap

	ID3D12DescriptorHeap* heaps[] = { g_d3d.cbv_srv_uav.GetHeap() };
	command_list->SetDescriptorHeaps(1, heaps);

	// ------------------------------------------------------------------
	// Clear render targets on first frame to avoid weird uninitialized
	// render targets, and also when moving the camera in reference mode

	auto ClearUAV = [&](ID3D12Resource* resource, D3D12GlobalDescriptors descriptor, RT_Vec4 color)
	{
		float clear_color[4] = { color.x, color.y, color.z, color.w };
		command_list->ClearUnorderedAccessViewFloat(frame->descriptors.GetGPUDescriptor(descriptor),
			frame->non_shader_descriptors.GetCPUDescriptor(descriptor),
			resource,
			clear_color,
			0,
			nullptr);
	};

	bool frame_dirty = (g_d3d.accum_frame_index == 0 || g_d3d.io.scene_transition);
	// A cockpit monitor that was off last frame has stale history of its own.
	const bool view_history_reset = g_camera_history_reset;
	g_camera_history_reset = false;

	if (frame_dirty || view_history_reset)
	{
		if (frame_dirty)
			g_d3d.accum_frame_index = 0;

		for (size_t rt_index = 0; rt_index < RenderTarget_COUNT; rt_index++)
		{
			D3D12GlobalDescriptors descriptor_id = clearDescriptors[rt_index];
			ClearUAV(render_targets[rt_index], descriptor_id, RT_Vec4Make(0, 0, 0, 0));
		}
	}

	g_d3d.io.scene_transition = false;

	// ------------------------------------------------------------------
	// Determine dispatch dimensions

	// Cockpit camera passes use native quarter-output buffers and do not enter
	// the main view's temporal upscaler/history.
	uint32_t render_width = (g_d3d.scene.render_blit ? g_d3d.output_width : g_d3d.render_width) / (g_d3d.scene.render_blit ? 4 : 1);
	uint32_t render_height = (g_d3d.scene.render_blit ? g_d3d.output_height : g_d3d.render_height) / (g_d3d.scene.render_blit ? 4 : 1);

	uint32_t output_width = g_d3d.output_width / (g_d3d.scene.render_blit ? 4 : 1);
	uint32_t output_height = g_d3d.output_height / (g_d3d.scene.render_blit ? 4 : 1);

	uint32_t rt_render_width = g_d3d.render_width_override == 0 || g_d3d.scene.render_blit ? render_width : g_d3d.render_width_override;
	uint32_t rt_render_height = g_d3d.render_height_override == 0 || g_d3d.scene.render_blit ? render_height : g_d3d.render_height_override;

	#if 0
	if (g_d3d.scene.render_blit) {
		rt_render_width = g_d3d.output_width / 4;
		rt_render_height = g_d3d.output_height / 4;
	}

	render_width = rt_render_width;
	render_height = rt_render_height;
	output_width = rt_render_width;
	output_height = rt_render_height;
	#endif

#if RT_INLINE_RAYTRACING
	uint32_t rt_dispatch_w = RT_MAX((rt_render_width + GROUP_X - 1) / GROUP_X, 1);
	uint32_t rt_dispatch_h = RT_MAX((rt_render_height + GROUP_Y - 1) / GROUP_Y, 1);
#endif

	uint32_t dispatch_w = RT_MAX((render_width + GROUP_X - 1) / GROUP_X, 1);
	uint32_t dispatch_h = RT_MAX((render_height + GROUP_Y - 1) / GROUP_Y, 1);

	// ------------------------------------------------------------------
	// Dispatch Rays / Inline raytracing

	/*

		Shader table layouts:
		Raygen shader table:
		- Primary raygen
		- Direct lighting raygen
		- Indirect lighting raygen
		Miss shader table:
		- Primary miss
		- Occlusion miss
		Hitgroups shader table:
		- Primary hitgroup 0
		- Occlusion hitgroup 0
		- Primary hitgroup 1
		- Occlusion hitgroup 1
		- ...
		- ...
		- Primary hitgroup instance count - 1
		- Occlusion hitgroup instance count - 1

	*/

#if RT_DISPATCH_RAYS
	size_t hitgroup_record_stride = RT_ALIGN_POW2(sizeof(ShaderRecord), D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
#endif

	{
		// ------------------------------------------------------------------
		// Primary ray pass (G-Buffering)

		GPUProfiler::BeginTimestampQuery(command_list, GPUProfiler::GPUTimer_PrimaryRay);
#ifdef D2_ENABLE_OPENVR
        const bool hybrid=OpenVRPrototype::EyePass() && OpenVRPrototype::HybridVisibility();
        if(hybrid) RasterizeHybridVisibility(command_list,frame,rt,rt_render_width,rt_render_height);
#endif

#if RT_DISPATCH_RAYS
		D3D12_DISPATCH_RAYS_DESC desc = {};
		desc.RayGenerationShaderRecord.StartAddress = GetShaderTableGPUPtr(g_d3d.raygen_shader_table, 0);
		desc.RayGenerationShaderRecord.SizeInBytes = sizeof(ShaderRecord);

		desc.MissShaderTable.StartAddress = GetShaderTableGPUPtr(g_d3d.miss_shader_table, 0);
		desc.MissShaderTable.StrideInBytes = sizeof(ShaderRecord);
		desc.MissShaderTable.SizeInBytes = 2 * desc.MissShaderTable.StrideInBytes;

		desc.HitGroupTable.StartAddress = GetShaderTableGPUPtr(g_d3d.hitgroups_shader_table, 0);
		desc.HitGroupTable.StrideInBytes = hitgroup_record_stride;
		desc.HitGroupTable.SizeInBytes = 2 * hitgroup_record_stride;

		desc.Width = rt_render_width;
		desc.Height = rt_render_height;
		desc.Depth = 1;

		command_list->SetPipelineState1(g_d3d.rt_pipelines.primary.pso);
		command_list->SetComputeRootSignature(g_d3d.global_root_sig);
		command_list->SetComputeRootDescriptorTable(RaytraceRootParameters_MainDescriptorTable, frame->descriptors.gpu);
		command_list->SetComputeRootDescriptorTable(RaytraceRootParameters_BindlessSRVTable, g_d3d.cbv_srv_uav.GetGPUBase());
		command_list->SetComputeRootDescriptorTable(RaytraceRootParameters_BindlessTriangleBufferTable, g_d3d.cbv_srv_uav.GetGPUBase());
		command_list->DispatchRays(&desc);
#elif RT_INLINE_RAYTRACING
#ifdef D2_ENABLE_OPENVR
        command_list->SetPipelineState(hybrid ? g_d3d.cs.hybrid_primary.pso : g_d3d.rt_shaders.primary_inline.pso);
#else
		command_list->SetPipelineState(g_d3d.rt_shaders.primary_inline.pso);
#endif
		command_list->SetComputeRootSignature(g_d3d.global_root_sig);
		command_list->SetComputeRootDescriptorTable(RaytraceRootParameters_MainDescriptorTable, frame->descriptors.gpu);
		command_list->SetComputeRootDescriptorTable(RaytraceRootParameters_BindlessSRVTable, g_d3d.cbv_srv_uav.GetGPUBase());
		command_list->SetComputeRootDescriptorTable(RaytraceRootParameters_BindlessTriangleBufferTable, g_d3d.cbv_srv_uav.GetGPUBase());
		command_list->Dispatch(rt_dispatch_w, rt_dispatch_h, 1);
#endif

		// UAV barriers for all render targets of the primary ray dispatch
		// Note(Justin): This is not correct since the ping-pong'd render targets are not taken into account here
		ID3D12Resource* uav_render_targets[] =
		{
			rt.albedo, rt.emissive,
			rt.normal, rt.depth,
			rt.motion, rt.view_dir,
			rt.metallic, rt.roughness, rt.material,
			rt.visibility_prim, rt.visibility_bary
		};
		UAVBarriers(command_list, RT_ARRAY_COUNT(uav_render_targets), uav_render_targets);
		// normal/depth/metallic/roughness/material/view_dir are ping-ponged by
		// frame parity, so on odd frames the pass wrote the "_prev" resources,
		// which the list above does not cover. A null-resource barrier orders
		// every UAV write before the lighting passes read the G-buffer.
		UAVBarrier(command_list, nullptr);

		GPUProfiler::EndTimestampQuery(command_list, GPUProfiler::GPUTimer_PrimaryRay);
	}

#if 1
	{
		// ------------------------------------------------------------------
		// Direct lighting pass

		GPUProfiler::BeginTimestampQuery(command_list, GPUProfiler::GPUTimer_DirectLighting);

#if RT_DISPATCH_RAYS
		D3D12_DISPATCH_RAYS_DESC desc = {};
		desc.RayGenerationShaderRecord.StartAddress = GetShaderTableGPUPtr(g_d3d.raygen_shader_table, 1);
		desc.RayGenerationShaderRecord.SizeInBytes = sizeof(ShaderRecord);

		desc.MissShaderTable.StartAddress = GetShaderTableGPUPtr(g_d3d.miss_shader_table, 0);
		desc.MissShaderTable.StrideInBytes = sizeof(ShaderRecord);
		desc.MissShaderTable.SizeInBytes = 2 * desc.MissShaderTable.StrideInBytes;

		desc.HitGroupTable.StartAddress = GetShaderTableGPUPtr(g_d3d.hitgroups_shader_table, 0);
		desc.HitGroupTable.StrideInBytes = hitgroup_record_stride;
		desc.HitGroupTable.SizeInBytes = 2 * hitgroup_record_stride;

		desc.Width = rt_render_width;
		desc.Height = rt_render_height;
		desc.Depth = 1;

		command_list->SetPipelineState1(g_d3d.rt_pipelines.direct.pso);
		command_list->SetComputeRootSignature(g_d3d.global_root_sig);
		command_list->SetComputeRootDescriptorTable(RaytraceRootParameters_MainDescriptorTable, frame->descriptors.gpu);
		command_list->SetComputeRootDescriptorTable(RaytraceRootParameters_BindlessSRVTable, g_d3d.cbv_srv_uav.GetGPUBase());
		command_list->SetComputeRootDescriptorTable(RaytraceRootParameters_BindlessTriangleBufferTable, g_d3d.cbv_srv_uav.GetGPUBase());
		command_list->DispatchRays(&desc);
#elif RT_INLINE_RAYTRACING
		command_list->SetPipelineState(g_d3d.rt_shaders.direct_lighting_inline.pso);
		command_list->SetComputeRootSignature(g_d3d.global_root_sig);
		command_list->SetComputeRootDescriptorTable(RaytraceRootParameters_MainDescriptorTable, frame->descriptors.gpu);
		command_list->SetComputeRootDescriptorTable(RaytraceRootParameters_BindlessSRVTable, g_d3d.cbv_srv_uav.GetGPUBase());
		command_list->SetComputeRootDescriptorTable(RaytraceRootParameters_BindlessTriangleBufferTable, g_d3d.cbv_srv_uav.GetGPUBase());
		const uint32_t direct_group_x = DirectReferenceGroups() ? GROUP_X : DIRECT_GROUP_X;
		const uint32_t direct_group_y = DirectReferenceGroups() ? GROUP_Y : DIRECT_GROUP_Y;
		command_list->Dispatch((rt_render_width + direct_group_x - 1) / direct_group_x,
		                      (rt_render_height + direct_group_y - 1) / direct_group_y, 1);
#endif

		// UAV barriers for all render targets of the direct lighting dispatch
		ID3D12Resource* uav_render_targets[] =
		{
			rt.diff, rt.spec, rt.emissive
		};
		UAVBarriers(command_list, RT_ARRAY_COUNT(uav_render_targets), uav_render_targets);

		GPUProfiler::EndTimestampQuery(command_list, GPUProfiler::GPUTimer_DirectLighting);
	}
#else
	{
		GPUProfiler::BeginTimestampQuery(command_list, GPUProfiler::GPUTimer_DirectLighting);

		uint32_t dispatch_w = (render_width + GROUP_X - 1) / GROUP_X;
		uint32_t dispatch_h = (render_height + GROUP_Y - 1) / GROUP_Y;

		command_list->SetPipelineState(g_d3d.cs.restir_gen_candidates.pso);
		command_list->Dispatch(dispatch_w, dispatch_h, 1);

		ID3D12Resource* uav_render_targets[] =
		{
			rt.diff, rt.spec,
		};
		UAVBarriers(command_list, RT_ARRAY_COUNT(uav_render_targets), uav_render_targets);


		GPUProfiler::EndTimestampQuery(command_list, GPUProfiler::GPUTimer_DirectLighting);
	}
#endif

	{
		// ------------------------------------------------------------------
		// Indirect lighting pass

		GPUProfiler::BeginTimestampQuery(command_list, GPUProfiler::GPUTimer_IndirectLighting);
		bool trace_indirect = tweak_vars.enable_pathtracing;
#ifdef D2_ENABLE_OPENVR
		// Cockpit camera monitors are small; in VR their bounce lighting costs
		// more than it shows. D2_VR_CAMERA_INDIRECT=1 restores it.
		static const bool vr_camera_indirect = []{
			char value[8] = {};
			return GetEnvironmentVariableA("D2_VR_CAMERA_INDIRECT", value, sizeof(value)) > 0 && value[0] == '1';
		}();
		if (g_d3d.scene.render_blit && OpenVRPrototype::Active() && !vr_camera_indirect)
			trace_indirect = false;
#endif
		if (trace_indirect)
		{

#if RT_DISPATCH_RAYS
			D3D12_DISPATCH_RAYS_DESC desc = {};
			desc.RayGenerationShaderRecord.StartAddress = GetShaderTableGPUPtr(g_d3d.raygen_shader_table, 2);
			desc.RayGenerationShaderRecord.SizeInBytes = sizeof(ShaderRecord);

			desc.MissShaderTable.StartAddress = GetShaderTableGPUPtr(g_d3d.miss_shader_table, 1);
			desc.MissShaderTable.StrideInBytes = sizeof(ShaderRecord);
			desc.MissShaderTable.SizeInBytes = 2 * desc.MissShaderTable.StrideInBytes;

			desc.HitGroupTable.StartAddress = GetShaderTableGPUPtr(g_d3d.hitgroups_shader_table, 1);
			desc.HitGroupTable.StrideInBytes = hitgroup_record_stride;
			desc.HitGroupTable.SizeInBytes = 2 * hitgroup_record_stride;

			desc.Width = rt_render_width;
			desc.Height = rt_render_height;
			desc.Depth = 1;

			command_list->SetPipelineState1(g_d3d.rt_pipelines.indirect.pso);
			command_list->SetComputeRootSignature(g_d3d.global_root_sig);
			command_list->SetComputeRootDescriptorTable(RaytraceRootParameters_MainDescriptorTable, frame->descriptors.gpu);
			command_list->SetComputeRootDescriptorTable(RaytraceRootParameters_BindlessSRVTable, g_d3d.cbv_srv_uav.GetGPUBase());
			command_list->SetComputeRootDescriptorTable(RaytraceRootParameters_BindlessTriangleBufferTable, g_d3d.cbv_srv_uav.GetGPUBase());
			command_list->DispatchRays(&desc);
#elif RT_INLINE_RAYTRACING
			command_list->SetPipelineState(g_d3d.rt_shaders.indirect_lighting_inline.pso);
			command_list->SetComputeRootSignature(g_d3d.global_root_sig);
			command_list->SetComputeRootDescriptorTable(RaytraceRootParameters_MainDescriptorTable, frame->descriptors.gpu);
			command_list->SetComputeRootDescriptorTable(RaytraceRootParameters_BindlessSRVTable, g_d3d.cbv_srv_uav.GetGPUBase());
			command_list->SetComputeRootDescriptorTable(RaytraceRootParameters_BindlessTriangleBufferTable, g_d3d.cbv_srv_uav.GetGPUBase());
			const uint32_t indirect_group_x = IndirectReferenceGroups() ? GROUP_X : INDIRECT_GROUP_X;
			const uint32_t indirect_group_y = IndirectReferenceGroups() ? GROUP_Y : INDIRECT_GROUP_Y;
			command_list->Dispatch((rt_render_width + indirect_group_x - 1) / indirect_group_x,
			                      (rt_render_height + indirect_group_y - 1) / indirect_group_y, 1);
#endif

			// UAV barriers for all render targets of the indirect lighting dispatch
			ID3D12Resource* uav_render_targets[] =
			{
				rt.diff, rt.spec
			};
			UAVBarriers(command_list, RT_ARRAY_COUNT(uav_render_targets), uav_render_targets);

		}
		GPUProfiler::EndTimestampQuery(command_list, GPUProfiler::GPUTimer_IndirectLighting);
	}

	// ------------------------------------------------------------------
	// Copy pixel debug buffer to readback

#if RT_PIXEL_DEBUG
	// TODO(daniel): How to make this not slow
	ResourceTransition(command_list, g_d3d.pixel_debug.uav_resource, D3D12_RESOURCE_STATE_COPY_SOURCE);
	command_list->CopyResource(frame->pixel_debug_readback, g_d3d.pixel_debug.uav_resource);
	ResourceTransition(command_list, g_d3d.pixel_debug.uav_resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
#endif

	// ------------------------------------------------------------------
	// Denoise

	{
		GPUProfiler::BeginTimestampQuery(command_list, GPUProfiler::GPUTimer_DenoiseResample);

        bool fastSpatial=false;
#ifdef D2_ENABLE_OPENVR
        fastSpatial=OpenVRPrototype::EyePass() && OpenVRPrototype::FastSpatial() &&
            tweak_vars.svgf_jitter==0 && !tweak_vars.svgf_stabilize &&
            g_d3d.io.debug_render_mode==0; // Debug history/moment views retain the full denoiser.
        command_list->SetPipelineState(fastSpatial ? g_d3d.cs.svgf_resample_vr.pso : g_d3d.cs.svgf_resample.pso);
#else
        command_list->SetPipelineState(g_d3d.cs.svgf_resample.pso);
        (void)fastSpatial;
#endif
		command_list->Dispatch(dispatch_w, dispatch_h, 1);

		ID3D12Resource* uav_render_targets[] =
		{
			rt.diff_denoise_ping, rt.spec_denoise_ping,
			rt.moments, rt.history_length
		};
		UAVBarriers(command_list, RT_ARRAY_COUNT(uav_render_targets), uav_render_targets);
		UAVBarrier(command_list, nullptr); // moments is ping-ponged (see primary pass)

		GPUProfiler::EndTimestampQuery(command_list, GPUProfiler::GPUTimer_DenoiseResample);

		GPUProfiler::BeginTimestampQuery(command_list, GPUProfiler::GPUTimer_DenoiseSVGF);

		if (tweak_vars.svgf_enabled)
		{
			for (UINT i = 0; i < 5; i++)
			{
				command_list->SetComputeRoot32BitConstant(RaytraceRootParameters_DenoiseIteration, i, 0);
#ifdef D2_ENABLE_OPENVR
                command_list->SetPipelineState(fastSpatial ? g_d3d.cs.svgf_blur_vr.pso : g_d3d.cs.svgf_blur.pso);
#else
                command_list->SetPipelineState(g_d3d.cs.svgf_blur.pso);
#endif
				command_list->Dispatch(dispatch_w, dispatch_h, 1);

				switch (i)
				{
				case 0:
				{
					ID3D12Resource* resources[] =
					{
						rt.diff_denoise_pong,
						rt.spec_denoise_pong,
						rt.moments_hist,
					};
					UAVBarriers(command_list, RT_ARRAY_COUNT(resources), resources);
				} break;

				case 1:
				{
					ID3D12Resource* resources[] =
					{
						rt.diff_hist,
						rt.spec_hist,
						rt.moments_denoise_pong,
					};
					UAVBarriers(command_list, RT_ARRAY_COUNT(resources), resources);
				} break;

				case 2:
				{
					ID3D12Resource* resources[] =
					{
						rt.diff_denoise_ping,
						rt.spec_denoise_ping,
						rt.moments_denoise_ping,
					};
					UAVBarriers(command_list, RT_ARRAY_COUNT(resources), resources);
				} break;

				case 3:
				{
					ID3D12Resource* resources[] =
					{
						rt.diff_denoise_pong,
						rt.spec_denoise_pong,
						rt.moments_denoise_pong,
					};
					UAVBarriers(command_list, RT_ARRAY_COUNT(resources), resources);
				} break;

				case 4:
				{
					ID3D12Resource* resources[] =
					{
						rt.diff_denoise_ping,
						rt.spec_denoise_ping,
						rt.moments_denoise_ping,
					};
					UAVBarriers(command_list, RT_ARRAY_COUNT(resources), resources);
				} break;
				}
			}
		}
		else
		{
			CopyBuffer(command_list, rt.diff_hist, rt.diff_denoise_ping);
			CopyBuffer(command_list, rt.spec_hist, rt.spec_denoise_ping);
		}
	}

	{
		if (tweak_vars.svgf_stabilize)
		{
			ResourceTransition(command_list, render_targets[rt_diff_stable[b]], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			ResourceTransition(command_list, render_targets[rt_spec_stable[b]], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

			command_list->SetPipelineState(g_d3d.cs.svgf_post_resample.pso);
			command_list->Dispatch(dispatch_w, dispatch_h, 1);

			ResourceTransition(command_list, render_targets[rt_diff_stable[b]], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			ResourceTransition(command_list, render_targets[rt_spec_stable[b]], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			ID3D12Resource* resources[] =
			{
				render_targets[rt_diff_stable[a]],
				render_targets[rt_spec_stable[a]],
				rt.history_length,
			};
			UAVBarriers(command_list, RT_ARRAY_COUNT(resources), resources);
		}

		GPUProfiler::EndTimestampQuery(command_list, GPUProfiler::GPUTimer_DenoiseSVGF);
	}

	// ------------------------------------------------------------------
	// Composite lighting with albedo

	GPUProfiler::BeginTimestampQuery(command_list, GPUProfiler::GPUTimer_Composite);

	command_list->SetPipelineState(g_d3d.cs.composite.pso);
	command_list->Dispatch(dispatch_w, dispatch_h, 1);

	UAVBarrier(command_list, rt.color);
	GPUProfiler::EndTimestampQuery(command_list, GPUProfiler::GPUTimer_Composite);

	// ------------------------------------------------------------------
	// Temporal anti-aliasing (if enabled), AMD FSR 3.1 (if enabled)

	GPUProfiler::BeginTimestampQuery(command_list, GPUProfiler::GPUTimer_TAA);

	if (g_d3d.upscaling_aa_mode != UPSCALING_AA_MODE_OFF && !g_d3d.scene.render_blit)
	{
		if (g_d3d.upscaling_aa_mode == UPSCALING_AA_MODE_TAA)
		{
			ResourceTransition(command_list, render_targets[rt_taa_result[b]], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

			command_list->SetPipelineState(g_d3d.cs.taa.pso);
			command_list->Dispatch(dispatch_w, dispatch_h, 1);
			ResourceTransition(command_list, render_targets[rt_taa_result[b]], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			UAVBarrier(command_list, render_targets[rt_taa_result[a]]);
		}
		else if (g_d3d.upscaling_aa_mode == UPSCALING_AA_MODE_AMD_FSR_2_2)
		{
            // FSR needs projected depth. rt.depth is distance along the ray in
            // world units, which the desktop path used to pass straight through,
            // so FSR saw almost everything at or past the far plane and could not
            // detect disocclusions (ghosting). Both desktop and VR now convert.
            command_list->SetPipelineState(g_d3d.cs.vr_fsr_depth.pso);
            command_list->Dispatch(dispatch_w,dispatch_h,1);
            UAVBarrier(command_list,rt.fsr_depth);
            ID3D12Resource* fsrDepth=rt.fsr_depth;
			ResourceTransition(command_list,rt.color,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ResourceTransition(command_list,fsrDepth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ResourceTransition(command_list,rt.motion,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ResourceTransition(command_list,rt.fsr2_reactive_mask,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ResourceTransition(command_list,render_targets[rt_taa_result[a]],D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

			GlobalConstantBuffer* scene_cb = frame->scene_cb.As<GlobalConstantBuffer>();
			FSR2::Dispatch(command_list,
				rt.color, fsrDepth, rt.motion, rt.fsr2_reactive_mask, render_targets[rt_taa_result[a]],
				render_width, render_height, scene_cb->taa_jitter.x, scene_cb->taa_jitter.y,
				g_d3d.scene.camera.near_plane, g_d3d.scene.camera.far_plane, RT_RadiansFromDegrees(g_d3d.scene.camera.vfov),
				g_d3d.io.delta_time * 1000.0f, FSR2::GetSharpnessForFSRMode(), g_d3d.io.scene_transition
			);

			ResourceTransition(command_list,rt.color,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ResourceTransition(command_list,fsrDepth,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ResourceTransition(command_list,rt.motion,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ResourceTransition(command_list,rt.fsr2_reactive_mask,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			// Set the descriptor heap, root sig and root descriptor tables again, since FSR 3.1 will set its own
			command_list->SetDescriptorHeaps(1, heaps);
			command_list->SetComputeRootSignature(g_d3d.global_root_sig);
			command_list->SetComputeRootDescriptorTable(RaytraceRootParameters_MainDescriptorTable, frame->descriptors.gpu);
			command_list->SetComputeRootDescriptorTable(RaytraceRootParameters_BindlessSRVTable, g_d3d.cbv_srv_uav.GetGPUBase());
			command_list->SetComputeRootDescriptorTable(RaytraceRootParameters_BindlessTriangleBufferTable, g_d3d.cbv_srv_uav.GetGPUBase());
		}
	}
	else
	{
		CopyBuffer(command_list, render_targets[rt_taa_result[a]], rt.color);
	}

	GPUProfiler::EndTimestampQuery(command_list, GPUProfiler::GPUTimer_TAA);

	// ------------------------------------------------------------------
	// Do bloom

	uint32_t output_dispatch_w = RT_MAX((output_width + GROUP_X - 1) / GROUP_X, 1);
	uint32_t output_dispatch_h = RT_MAX((output_height + GROUP_Y - 1) / GROUP_Y, 1);

	GPUProfiler::BeginTimestampQuery(command_list, GPUProfiler::GPUTimer_Bloom);

	command_list->SetPipelineState(g_d3d.cs.bloom_prepass.pso);
	command_list->Dispatch(output_dispatch_w / 2, output_dispatch_h / 2, 1);

	UAVBarrier(command_list, rt.bloom_prepass);

	ID3D12Resource* bloom_render_targets[]
	{
		render_targets[RenderTarget_bloom0],
		render_targets[RenderTarget_bloom1],
		render_targets[RenderTarget_bloom2],
		render_targets[RenderTarget_bloom3],
		render_targets[RenderTarget_bloom4],
		render_targets[RenderTarget_bloom5],
		render_targets[RenderTarget_bloom6],
		render_targets[RenderTarget_bloom7],
	};

	for (int i = 0; i < 8; i++)
	{
		command_list->SetComputeRoot32BitConstant(RaytraceRootParameters_DenoiseIteration, i, 0);

		int scale1 = 1 << (i + 1);
		int scale2 = 1 << (i + 2);

		int h1 = RT_MAX((output_height / scale1 + GROUP_Y - 1) / GROUP_Y, 1);

		int w2 = RT_MAX((output_width / scale2 + GROUP_X - 1) / GROUP_X, 1);
		int h2 = RT_MAX((output_height / scale2 + GROUP_Y - 1) / GROUP_Y, 1);

		command_list->SetPipelineState(g_d3d.cs.bloom_blur_horz.pso);
		command_list->Dispatch(w2, h1, 1);

		UAVBarrier(command_list, rt.bloom_pong);

		command_list->SetPipelineState(g_d3d.cs.bloom_blur_vert.pso);
		command_list->Dispatch(w2, h2, 1);

		UAVBarrier(command_list, bloom_render_targets[i]);
	}

	GPUProfiler::EndTimestampQuery(command_list, GPUProfiler::GPUTimer_Bloom);

	// ------------------------------------------------------------------
	// Do tonemapping / gamma correction / other post processing effects

	UAVBarrier(command_list, rt.debug);
	GPUProfiler::BeginTimestampQuery(command_list, GPUProfiler::GPUTimer_PostProcess);

	ResourceTransition(command_list, render_targets[rt_taa_result[a]], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	ResourceTransitions(command_list, RT_ARRAY_COUNT(bloom_render_targets), bloom_render_targets, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	command_list->SetPipelineState(g_d3d.cs.post_process.pso);
	command_list->Dispatch(output_dispatch_w, output_dispatch_h, 1);

	ResourceTransition(command_list, render_targets[rt_taa_result[a]], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	ResourceTransitions(command_list, RT_ARRAY_COUNT(bloom_render_targets), bloom_render_targets, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	UAVBarrier(command_list, rt.color_final);
	GPUProfiler::EndTimestampQuery(command_list, GPUProfiler::GPUTimer_PostProcess);

	// ------------------------------------------------------------------
	// Resolve to SDR render target
	// TODO(daniel): Put this in post_process.hlsl

	command_list->SetPipelineState(g_d3d.cs.resolve_final_color.pso);
	command_list->Dispatch(output_dispatch_w, output_dispatch_h, 1);

	UAVBarrier(command_list, rt.resolve);

	if (!g_d3d.scene.render_blit)
	{
		CopyResource(command_list, rt.color_final, rt.resolve);
	}

	GPUProfiler::EndTimestampQuery(command_list, GPUProfiler::GPUTimer_FrameTime);
	GPUProfiler::ResolveTimestampQueries(command_list);
#ifdef D2_ENABLE_OPENVR
	OpenVRPrototype::EndEyeGpuTiming(command_list.GetD3D12CommandList());
#endif
	const uint64_t submitted_fence = g_d3d.command_queue_direct->ExecuteCommandList(command_list);
#ifdef D2_ENABLE_OPENVR
	if (!OpenVRPrototype::EyePass())
#endif
		g_shared_scene_data_fence = submitted_fence;
}

int RenderBackend::BeginCameraView(int view)
{
#ifdef D2_ENABLE_OPENVR
	if (OpenVRPrototype::EyePass())
		return RT_CAMERA_VIEW_UNAVAILABLE;
#endif
	if (g_active_camera_frame || g_active_camera_view >= 0 || view < 0 || view >= kCameraViews || !g_camera_blit_targets_b[0])
		return RT_CAMERA_VIEW_UNAVAILABLE;

	// Frames since this view last rendered (0 = never / targets recreated).
	const uint64_t gap = g_camera_last_frame[view] ? g_d3d.frame_index + 1 - g_camera_last_frame[view] : 0;

	// VR: each monitor renders every other frame, the two staggered so they never
	// render on the same frame; the skipped frame shows the view's last image.
	// Monitors cost up to ~2 ms of GPU in busy areas, enough to push SteamVR into
	// half-rate reprojection. D2_VR_CAMERA_HALF_RATE=0 restores full rate.
	bool half_rate = false;
#ifdef D2_ENABLE_OPENVR
	static const bool half_rate_enabled = []() {
		char value[8] = {};
		return !(GetEnvironmentVariableA("D2_VR_CAMERA_HALF_RATE", value, sizeof(value)) > 0 && value[0] == '0');
	}();
	half_rate = half_rate_enabled && OpenVRPrototype::Active();
#endif
	if (half_rate && gap == 1 && ((g_d3d.frame_index + (uint64_t)view) & 1))
	{
		if (view == 1)
			for (size_t i = 0; i < RenderTarget_COUNT; ++i) std::swap(g_d3d.render_targets_blit[i], g_camera_blit_targets_b[i]);
		// The blit must use the monitor frame slot's descriptors, not the main view's.
		g_camera_saved_render_blit = g_d3d.scene.render_blit;
		g_d3d.scene.render_blit = true;
		g_camera_reusing = true;
		g_active_camera_view = view;
		return RT_CAMERA_VIEW_REUSE;
	}

	// Alternate between the view's two frame data sets by its own render count,
	// so the previous render's data is intact at any update rate.
	const uint64_t render_index = ++g_camera_history[view];
	FrameData* frame = &g_camera_frames[render_index & 1][view];
	if (!frame->upload_buffer)
	{
		// Same layout as the monitor frame slot; the headroom holds the view's
		// raster and material uploads.
		AllocateFrameDataLike(frame, g_d3d.frame_data[2], L"Cockpit camera upload", RT_MB(24));
	}
	// Last used two renders ago, so this normally does not block.
	if (frame->fence_value)
		g_d3d.command_queue_direct->WaitForFenceValue(frame->fence_value);
	RT_ArenaResetToMarker(&frame->upload_buffer_arena, frame->upload_buffer_arena_reset);

	// History is valid if this view rendered within the last two frames.
	const bool continuous = gap >= 1 && gap <= 2;
	g_camera_last_frame[view] = g_d3d.frame_index + 1;
	g_camera_history_reset = !continuous;
	g_vr_prev_frame_override = continuous ? &g_camera_frames[(render_index - 1) & 1][view] : frame;
	g_vr_frame_override = frame;

	if (view == 1)
		for (size_t i = 0; i < RenderTarget_COUNT; ++i) std::swap(g_d3d.render_targets_blit[i], g_camera_blit_targets_b[i]);

	g_active_camera_frame = frame;
	g_active_camera_view = view;
	return RT_CAMERA_VIEW_RENDER;
}

void RenderBackend::EndCameraView()
{
	if (g_active_camera_view < 0)
		return;
	if (g_active_camera_frame)
	{
		// Covers the view's ray tracing and its blit into the cockpit/HUD.
		g_active_camera_frame->fence_value = g_d3d.command_queue_direct->Signal();
	}
	if (g_camera_reusing)
	{
		g_d3d.scene.render_blit = g_camera_saved_render_blit;
		g_camera_reusing = false;
	}
	if (g_active_camera_view == 1)
		for (size_t i = 0; i < RenderTarget_COUNT; ++i) std::swap(g_d3d.render_targets_blit[i], g_camera_blit_targets_b[i]);
	g_active_camera_frame = nullptr;
	g_active_camera_view = -1;
	g_camera_history_reset = false;
	g_vr_frame_override = nullptr;
	g_vr_prev_frame_override = nullptr;
}

void RenderBackend::WaitForSharedSceneData()
{
#ifdef D2_ENABLE_OPENVR
	// In VR the only other users of the shared scene data are earlier cockpit
	// camera views. A full Flush() here also waited for both eye images
	// (~8-10 ms) before every camera, so the frame reached SteamVR late.
	if (OpenVRPrototype::Active())
	{
		if (g_shared_scene_data_fence)
			g_d3d.command_queue_direct->WaitForFenceValue(g_shared_scene_data_fence);
		return;
	}
#endif
	Flush();
}

void RenderBackend::SetShieldEffects(const RT_ShieldEffects* effects)
{
	if (effects)
	{
		g_shield_effects = *effects;
		if (g_shield_effects.bubble_count > RT_MAX_SHIELD_BUBBLES)
			g_shield_effects.bubble_count = RT_MAX_SHIELD_BUBBLES;
	}
	else
	{
		g_shield_effects = {};
		g_shield_effects.sweep_progress = -1.0f;
	}
}

void RenderBackend::RaytraceSetSkyColors(const RT_Vec3 top, const RT_Vec3 bottom)
{
	CurrentFrameData()->scene_cb.As<GlobalConstantBuffer>()->sky_color_top = top;
	CurrentFrameData()->scene_cb.As<GlobalConstantBuffer>()->sky_color_bottom = bottom;
}

bool RenderBackend::GetRetraceRays()
{
	return tweak_vars.retrace_rays;
}

void RenderBackend::RasterSetViewport(float x, float y, float width, float height)
{
	g_d3d_raster.viewport = { x, y, width, height, 0.1f, 5000.0f };
}

void RenderBackend::RasterSetRenderTarget(RT_ResourceHandle texture, bool clear_target)
{
	if (RT_RESOURCE_HANDLE_VALID(texture))
	{
		TextureResource* texture_resource = g_texture_slotmap.Find(texture);
        InvalidateTextureOpacity(texture_resource);
		if (ALWAYS(texture_resource))
		{
			CommandList& command_list = g_d3d.command_queue_direct->GetCommandList();

			// Check texture flags if render target usage is allowed
			if (!(texture_resource->texture->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))
			{
				// Get name of the texture
				wchar_t texture_name[128] = {};
				UINT name_size = (UINT)RT_ARRAY_COUNT(texture_name);
				texture_resource->texture->GetPrivateData(WKPDID_D3DDebugObjectNameW, &name_size, texture_name);

				// Use the same resource desc to create the render target clone, plus add the render target allow flag
				D3D12_RESOURCE_DESC render_target_desc = texture_resource->texture->GetDesc();
				render_target_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
				render_target_desc.MipLevels = 1;

				D3D12_CLEAR_VALUE clear_value = {};
				float clear_color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
				memcpy(clear_value.Color, &clear_color, sizeof(float) * 4);
				clear_value.Format = render_target_desc.Format;

				ID3D12Resource* rt_texture = RT_CreateTexture(texture_name, &render_target_desc, &clear_value);
				
				// Track the old resource as a temporary resource, which means it will be released once all pending commands with that resource are finished
				RT_TRACK_TEMP_RESOURCE(texture_resource->texture, &command_list);

				// Set texture_resource->texture to the new render target version of the d3d12 resource
				// Also allocate a render target descriptor for this new texture in the TextureResource
				texture_resource->texture = rt_texture;
				texture_resource->rtv_descriptor = g_d3d.rtv.Allocate(1);

				// Create the RTV, and also re-create the SRV, since it now needs to point to the new resource
				CreateTextureSRV(texture_resource->texture, texture_resource->descriptors.cpu, texture_resource->texture->GetDesc().Format);
				CreateTextureRTV(texture_resource->texture, texture_resource->rtv_descriptor.cpu, DXGI_FORMAT_R8G8B8A8_UNORM);
			}

			g_d3d_raster.render_target = texture_resource->texture;
			g_d3d_raster.rtv_handle = texture_resource->rtv_descriptor.cpu;

			ResourceTransition(command_list, g_d3d_raster.render_target, D3D12_RESOURCE_STATE_RENDER_TARGET);

			if (clear_target)
			{
				float clear_color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
				command_list->ClearRenderTargetView(g_d3d_raster.rtv_handle, clear_color, 0, nullptr);
			}
			g_d3d.command_queue_direct->ExecuteCommandList(command_list);
			
			return;
		}
	}

	g_d3d_raster.render_target = g_d3d.rt.color_final;
	g_d3d_raster.rtv_handle = g_d3d.color_final_rtv.cpu;
#ifdef D2_ENABLE_OPENVR
	OpenVRPrototype::UIRenderTarget(&g_d3d_raster.render_target, &g_d3d_raster.rtv_handle);
#endif
}

void RenderBackend::RasterTriangles(RT_RasterTrianglesParams* params, uint32_t num_params)
{
	for (uint32_t i = 0; i < num_params; ++i)
	{
		TextureResource* texture_resource = g_texture_slotmap.Find(params[i].texture_handle);
		if (!RT_RESOURCE_HANDLE_VALID(params[i].texture_handle))
		{
			texture_resource = g_d3d.white_texture;
		}

		uint32_t texture_heap_index = texture_resource->descriptors.heap_offset;

		size_t vertex_idx = (g_d3d_raster.tri_at + g_d3d_raster.tri_count) * 3;
		RT_ASSERT(vertex_idx < (BACK_BUFFER_COUNT* MAX_RASTER_TRIANGLES * 3));

		for (size_t v = 0; v < params[i].num_vertices; ++v)
		{
			g_d3d_raster.tri_vertex_buf_ptr[vertex_idx + v] = params[i].vertices[v];
			g_d3d_raster.tri_vertex_buf_ptr[vertex_idx + v].texture_index = texture_heap_index;
		}

		g_d3d_raster.tri_count += params[i].num_vertices / 3;
	}
}

void RenderBackend::RasterLines(RT_RasterLineVertex* vertices, uint32_t num_vertices)
{
	size_t vertex_idx = (g_d3d_raster.line_at + g_d3d_raster.line_count) * 2;
	RT_ASSERT(vertex_idx < (BACK_BUFFER_COUNT * MAX_RASTER_LINES * 2));

	for (uint32_t v = 0; v < num_vertices; ++v)
	{
		g_d3d_raster.line_vertex_buf_ptr[vertex_idx + v] = vertices[v];
	}

	g_d3d_raster.line_count += num_vertices / 2;
}

void RenderBackend::RasterLinesWorld(RT_RasterLineVertex* vertices, uint32_t num_vertices)
{
	size_t vertex_idx = (g_d3d_raster.debug_line_at + g_d3d_raster.debug_line_count) * 2;
	RT_ASSERT(vertex_idx < (BACK_BUFFER_COUNT* MAX_DEBUG_LINES_WORLD * 2));

	for (uint32_t v = 0; v < num_vertices; ++v)
	{
		g_d3d_raster.debug_line_vertex_buf_ptr[vertex_idx + v] = vertices[v];
	}

	g_d3d_raster.debug_line_count += num_vertices / 2;
}

void RenderBackend::RasterRender()
{
	// ------------------------------------------------------------------
	// Render quads
	
	// Skip if no work needs to be done so that we do not record any useless command lists
	if (g_d3d_raster.tri_count == 0 && g_d3d_raster.line_count == 0)
		return;

	bool mirrorUI = false;
#ifdef D2_ENABLE_OPENVR
	ID3D12Resource* uiResource = nullptr;
	D3D12_CPU_DESCRIPTOR_HANDLE uiHandle = {};
	mirrorUI = OpenVRPrototype::UIRenderTarget(&uiResource, &uiHandle) &&
		g_d3d_raster.render_target == uiResource && !OpenVRPrototype::CapturingReticle();
#endif

	CommandList& command_list = g_d3d.command_queue_direct->GetCommandList();
	// Flush the ring buffer for any pending resource uploads
	FlushRingBuffer(&g_d3d.resource_upload_ring_buffer);

	ResourceTransition(command_list, g_d3d_raster.render_target, D3D12_RESOURCE_STATE_RENDER_TARGET);
	if (mirrorUI)
		ResourceTransition(command_list, g_d3d.rt.color_final, D3D12_RESOURCE_STATE_RENDER_TARGET);
	auto DrawUI = [&](UINT vertices) {
		command_list->DrawInstanced(vertices, 1, 0, 0);
		if (mirrorUI) {
			command_list->OMSetRenderTargets(1, &g_d3d.color_final_rtv.cpu, FALSE, nullptr);
			command_list->DrawInstanced(vertices, 1, 0, 0);
			command_list->OMSetRenderTargets(1, &g_d3d_raster.rtv_handle, FALSE, nullptr);
		}
	};

	command_list->RSSetViewports(1, &g_d3d_raster.viewport);
	D3D12_RECT scissor_rect = { 0, 0, LONG_MAX, LONG_MAX };
	command_list->RSSetScissorRects(1, &scissor_rect);
	command_list->OMSetRenderTargets(1, &g_d3d_raster.rtv_handle, FALSE, nullptr);

	size_t vertex_buffer_offset = 0;

	if (g_d3d_raster.tri_count)
	{
		command_list->SetGraphicsRootSignature(g_d3d_raster.tri_root_sig);
		command_list->SetPipelineState(g_d3d_raster.tri_state);

		command_list->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		vertex_buffer_offset = g_d3d_raster.tri_at * 3 * sizeof(RT_RasterTriVertex);

		// Setup vertex and index buffer views
		D3D12_VERTEX_BUFFER_VIEW vertex_vbv = {};
		vertex_vbv.BufferLocation = g_d3d_raster.tri_vertex_buffer->GetGPUVirtualAddress() + vertex_buffer_offset;
		vertex_vbv.SizeInBytes = (UINT)(g_d3d_raster.tri_count * 3 * sizeof(RT_RasterTriVertex));
		vertex_vbv.StrideInBytes = sizeof(RT_RasterTriVertex);
		command_list->IASetVertexBuffers(0, 1, &vertex_vbv);

		// Setup bindless SRV descriptor table
		ID3D12DescriptorHeap* const heaps[] = { g_d3d.cbv_srv_uav.GetHeap() };
		command_list->SetDescriptorHeaps(1, heaps);
		command_list->SetGraphicsRootDescriptorTable(0, g_d3d.cbv_srv_uav.GetGPUBase());

		// Draw
		DrawUI((UINT)(g_d3d_raster.tri_count * 3));

		g_d3d_raster.tri_at += g_d3d_raster.tri_count;
		g_d3d_raster.tri_count = 0;
	}

	// ------------------------------------------------------------------
	// Render lines

	if (g_d3d_raster.line_count)
	{
		command_list->SetGraphicsRootSignature(g_d3d_raster.line_root_sig);
		command_list->SetPipelineState(g_d3d_raster.line_state);
		command_list->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_LINELIST);

		vertex_buffer_offset = g_d3d_raster.line_at * 2 * sizeof(RT_RasterLineVertex);

		D3D12_VERTEX_BUFFER_VIEW vbv = {};
		vbv.BufferLocation = g_d3d_raster.line_vertex_buffer->GetGPUVirtualAddress() + vertex_buffer_offset;
		vbv.SizeInBytes = (UINT)(g_d3d_raster.line_count * 2 * sizeof(RT_RasterLineVertex));
		vbv.StrideInBytes = sizeof(RT_RasterLineVertex);

		command_list->IASetVertexBuffers(0, 1, &vbv);
		DrawUI((UINT)(g_d3d_raster.line_count * 2));

		g_d3d_raster.line_at += g_d3d_raster.line_count;
		g_d3d_raster.line_count = 0;
	}

	g_d3d.command_queue_direct->ExecuteCommandList(command_list);
}

void RenderBackend::RasterRenderDebugLines()
{
	if (g_d3d_raster.debug_line_count == 0)
		return;

	CommandList& command_list = g_d3d.command_queue_direct->GetCommandList();
	D3D12_VIEWPORT viewport = g_d3d_raster.viewport;
	viewport.MinDepth = 0.001f;
	viewport.MaxDepth = 10000.0f;

	command_list->RSSetViewports(1, &viewport);
	D3D12_RECT scissor_rect = { 0, 0, LONG_MAX, LONG_MAX };
	command_list->RSSetScissorRects(1, &scissor_rect);

	if (g_d3d.io.debug_line_depth_enabled)
	{
		CopyResource(command_list, g_d3d_raster.depth_target, g_d3d.rt.depth);
		ResourceTransition(command_list, g_d3d_raster.depth_target, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	}

	ResourceTransition(command_list, g_d3d_raster.render_target, D3D12_RESOURCE_STATE_RENDER_TARGET);

	if (g_d3d.io.debug_line_depth_enabled)
	{
		command_list->OMSetRenderTargets(1, &g_d3d.color_final_rtv.cpu, FALSE, &g_d3d_raster.depth_target_dsv);
		command_list->SetPipelineState(g_d3d_raster.debug_line_state_depth);
	}
	else
	{
		command_list->OMSetRenderTargets(1, &g_d3d.color_final_rtv.cpu, FALSE, nullptr);
		command_list->SetPipelineState(g_d3d_raster.debug_line_state);
	}

	command_list->SetGraphicsRootSignature(g_d3d_raster.debug_line_root_sig);
	command_list->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_LINELIST);

	size_t vertex_buffer_offset = g_d3d_raster.debug_line_at * 2 * sizeof(RT_RasterLineVertex);
	D3D12_VERTEX_BUFFER_VIEW vbv = {};
	vbv.BufferLocation = g_d3d_raster.debug_line_vertex_buffer->GetGPUVirtualAddress() + vertex_buffer_offset;
	vbv.SizeInBytes = (UINT)(g_d3d_raster.debug_line_count * 2 * sizeof(RT_RasterLineVertex));
	vbv.StrideInBytes = sizeof(RT_RasterLineVertex);
	command_list->IASetVertexBuffers(0, 1, &vbv);

	XMVECTOR up = XMVectorSet(g_d3d.scene.camera.up.x, g_d3d.scene.camera.up.y, g_d3d.scene.camera.up.z, 0);
	XMVECTOR pos = XMVectorSet(g_d3d.scene.camera.position.x, g_d3d.scene.camera.position.y, g_d3d.scene.camera.position.z, 0);
	XMVECTOR dir = XMVectorSet(g_d3d.scene.camera.forward.x, g_d3d.scene.camera.forward.y, g_d3d.scene.camera.forward.z, 0);

	float aspect_ratio = viewport.Width / viewport.Height;
	XMMATRIX xm_matrix = XMMatrixTranspose(XMMatrixMultiply(XMMatrixLookToLH(pos, dir, up),
		XMMatrixPerspectiveFovLH(XMConvertToRadians(g_d3d.scene.camera.vfov),aspect_ratio, g_d3d.scene.camera.near_plane, g_d3d.scene.camera.far_plane)));
	command_list->SetGraphicsRoot32BitConstants(0, 16, &xm_matrix, 0);
	command_list->SetGraphicsRoot32BitConstants(1, 1, &g_d3d.scene.camera.far_plane, 0);
	command_list->DrawInstanced((UINT)(g_d3d_raster.debug_line_count * 2), 1, 0, 0);

	g_d3d_raster.debug_line_at += g_d3d_raster.debug_line_count;
	g_d3d_raster.debug_line_count = 0;

	g_d3d.command_queue_direct->ExecuteCommandList(command_list);
}

void RenderBackend::RasterBlitScene(const RT_Vec2* top_left, const RT_Vec2* bottom_right, bool blit_blend)
{
	// NOTES(Justin):
	// - Raster blit will cause aliasing (currently using linear sampler to counter this a bit, but still)
	// blit_blend currently unused (blending disabled temporarily)
	(void)blit_blend;

	FlushRingBuffer(&g_d3d.resource_upload_ring_buffer);

	CommandList& command_list = g_d3d.command_queue_direct->GetCommandList();
	ResourceTransition(command_list, g_d3d.rt_blit.resolve, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	ResourceTransition(command_list, g_d3d_raster.render_target, D3D12_RESOURCE_STATE_RENDER_TARGET);

	FrameData* frame = CurrentFrameData();
	auto CreateRenderTargetSRV = [&](RenderTarget rt, D3D12GlobalDescriptors descriptor)
	{
		RT_ASSERT((descriptor >= D3D12GlobalDescriptors_SRV_START && descriptor < D3D12GlobalDescriptors_CBV_START) ||
			(descriptor >= D3D12GlobalDescriptors_SRV_RT_START && descriptor < D3D12GlobalDescriptors_COUNT));
		CreateTextureSRV(g_d3d.render_targets_blit[rt], frame->descriptors.GetCPUDescriptor(descriptor), g_d3d.render_target_formats[rt]);
	};
	CreateRenderTargetSRV(RenderTarget_resolve, D3D12GlobalDescriptors_SRV_resolve);

	ID3D12DescriptorHeap* heaps[] = { g_d3d.cbv_srv_uav.GetHeap() };
	command_list->SetDescriptorHeaps(1, heaps);

	// TopLeftX, TopLeftY, width, height, minDepth, maxDepth
	float dst_w_f = bottom_right->x - top_left->x;
	float dst_h_f = bottom_right->y - top_left->y;

	// Determine source render size used when generating the blit texture.
	// Compute the source size in screen pixels directly (not render-target pixels),
	// so we can compute the correct scale to fit into the destination rect.
	float src_w_screen = 0.0f;
	float src_h_screen = 0.0f;
	if (g_d3d.render_targets_blit[RenderTarget_resolve]) {
		auto desc = g_d3d.render_targets_blit[RenderTarget_resolve]->GetDesc();
		// desc.Width is the GPU resource width (render-target pixels). If this is a
		// cockpit blit pass, the render-target is a quarter-size of the output so
		// multiply to get back to screen pixels.
		src_w_screen = (float)desc.Width * (g_d3d.scene.render_blit ? 4.0f : 1.0f);
		src_h_screen = (float)desc.Height * (g_d3d.scene.render_blit ? 4.0f : 1.0f);
	}

	if (src_w_screen == 0.0f || src_h_screen == 0.0f) {
		if (g_d3d.render_width_override != 0) {
			// render_width_override is provided in screen pixels already
			src_w_screen = (float)g_d3d.render_width_override;
		} else {
			src_w_screen = (float)(g_d3d.scene.render_blit ? g_d3d.output_width : g_d3d.render_width);
		}

		if (g_d3d.render_height_override != 0) {
			src_h_screen = (float)g_d3d.render_height_override;
		} else {
			src_h_screen = (float)(g_d3d.scene.render_blit ? g_d3d.output_height : g_d3d.render_height);
		}
	}

	// Fit source into destination preserving aspect ratio (letterbox/pillarbox)

	float scale_x = dst_w_f / src_w_screen;
	float scale_y = dst_h_f / src_h_screen;
	// Use scale = max to fill the destination and crop (avoid letterbox/pillarbox)
	// This will make the source extend beyond the dest in one axis but preserve aspect
	// and avoid stretching.
	float scale = RT_MAX(scale_x, scale_y);
	float draw_w = src_w_screen * scale;
	float draw_h = src_h_screen * scale;
	float offset_x = (dst_w_f - draw_w) * 0.5f;
	float offset_y = (dst_h_f - draw_h) * 0.5f;

	// Keep viewport covering the full destination rect; the shader uses
	// blit constants (blit_x/blit_y/blit_width/blit_height) to position
	// and scale the source texture inside this destination. Setting the
	// viewport to the full dst avoids double-scaling issues.
	D3D12_VIEWPORT viewport = { top_left->x, top_left->y, dst_w_f, dst_h_f, 0.0f, 1.0f };
	command_list->RSSetViewports(1, &viewport);

	// store debug values so ImGui can display them
	g_d3d_raster.blit_debug_dst_x = (uint32_t)top_left->x;
	g_d3d_raster.blit_debug_dst_y = (uint32_t)top_left->y;
	g_d3d_raster.blit_debug_dst_w = (uint32_t)dst_w_f;
	g_d3d_raster.blit_debug_dst_h = (uint32_t)dst_h_f;
	g_d3d_raster.blit_debug_draw_w = draw_w;
	g_d3d_raster.blit_debug_draw_h = draw_h;
	g_d3d_raster.blit_debug_offset_x = offset_x;
	g_d3d_raster.blit_debug_offset_y = offset_y;
	D3D12_RECT scissor_rect = { 0, 0, LONG_MAX, LONG_MAX };
	command_list->RSSetScissorRects(1, &scissor_rect);
	command_list->OMSetRenderTargets(1, &g_d3d_raster.rtv_handle, FALSE, nullptr);

	command_list->SetPipelineState(g_d3d_raster.blit_state);
	command_list->SetGraphicsRootSignature(g_d3d_raster.blit_root_sig);
	command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	// Pass blit constants as floats so fractional offsets and sizes are preserved.
	// blit_x/blit_y are offsets inside the viewport (local coordinates).
	float blit_consts[7];
	blit_consts[0] = offset_x; // local offset within destination viewport
	blit_consts[1] = offset_y;
	blit_consts[2] = draw_w;
	blit_consts[3] = draw_h;
	blit_consts[4] = 0.0f; // blending disabled for now
	blit_consts[5] = dst_w_f; // viewport width
	blit_consts[6] = dst_h_f; // viewport height
	command_list->SetGraphicsRoot32BitConstants(0, 7, blit_consts, 0);
	command_list->SetGraphicsRootDescriptorTable(1, frame->descriptors.GetGPUDescriptor(D3D12GlobalDescriptors_SRV_resolve));
	command_list->DrawInstanced(6, 1, 0, 0);

	g_d3d.command_queue_direct->ExecuteCommandList(command_list);
}

void RenderBackend::RasterBlit(RT_ResourceHandle src, const RT_Vec2* top_left, const RT_Vec2* bottom_right, bool blit_blend)
{
	// blit_blend currently unused (blending disabled temporarily)
	(void)blit_blend;

	FlushRingBuffer(&g_d3d.resource_upload_ring_buffer);

	TextureResource* src_texture = g_texture_slotmap.Find(src);
	if (ALWAYS(src_texture))
	{
		CommandList& command_list = g_d3d.command_queue_direct->GetCommandList();
		ResourceTransition(command_list, g_d3d.rt.resolve, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		ResourceTransition(command_list, g_d3d.rt.color_final, D3D12_RESOURCE_STATE_RENDER_TARGET);

		ID3D12DescriptorHeap* heaps[] = { g_d3d.cbv_srv_uav.GetHeap() };
		command_list->SetDescriptorHeaps(1, heaps);

		// TopLeftX, TopLeftY, width, height, minDepth, maxDepth
		D3D12_VIEWPORT viewport = { top_left->x, top_left->y, bottom_right->x - top_left->x, bottom_right->y - top_left->y, 0.0f, 1.0f };
		command_list->RSSetViewports(1, &viewport);
		D3D12_RECT scissor_rect = { 0, 0, LONG_MAX, LONG_MAX };
		command_list->RSSetScissorRects(1, &scissor_rect);
		command_list->OMSetRenderTargets(1, &g_d3d.color_final_rtv.cpu, FALSE, nullptr);

		command_list->SetPipelineState(g_d3d_raster.blit_state);
		command_list->SetGraphicsRootSignature(g_d3d_raster.blit_root_sig);
		command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		// For the simple RasterBlit path compute local offset=0 and pass viewport dims
		float blit_consts2[7];
		blit_consts2[0] = 0.0f; // offset_x
		blit_consts2[1] = 0.0f; // offset_y
		blit_consts2[2] = viewport.Width; // draw_w
		blit_consts2[3] = viewport.Height; // draw_h
		blit_consts2[4] = 0.0f; // blending disabled
		blit_consts2[5] = viewport.Width; // screen/viewport width
		blit_consts2[6] = viewport.Height; // screen/viewport height
		command_list->SetGraphicsRoot32BitConstants(0, 7, blit_consts2, 0);
		command_list->SetGraphicsRootDescriptorTable(1, src_texture->descriptors.gpu);
		command_list->DrawInstanced(6, 1, 0, 0);

		g_d3d.command_queue_direct->ExecuteCommandList(command_list);
	}
}

void RenderBackend::RenderImGuiTexture(RT_ResourceHandle texture_handle, float width, float height)
{
	TextureResource *res = g_texture_slotmap.Find(texture_handle);

	if (!res)
		res = g_texture_slotmap.Find(g_d3d.pink_checkerboard_texture);

	D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle = res->descriptors.gpu;
	ImGui::Image((ImTextureID)gpu_handle.ptr, ImVec2(width, height));
}

void RenderBackend::RenderImGui()
{
	// ------------------------------------------------------------------
	// Dear ImGui implementation

	CommandList& command_list = g_d3d.command_queue_direct->GetCommandList();
	ResourceTransition(command_list, g_d3d.rt.color_final, D3D12_RESOURCE_STATE_RENDER_TARGET);
	{
		ImDrawData* draw_data = ImGui::GetDrawData();
		if (draw_data)
		{
			command_list->OMSetRenderTargets(1, &g_d3d.color_final_rtv.cpu, FALSE, nullptr);

			ID3D12DescriptorHeap* imgui_heaps[] = { g_d3d.cbv_srv_uav.GetHeap() };
			command_list->SetDescriptorHeaps(RT_ARRAY_COUNT(imgui_heaps), imgui_heaps);

			ImGui_ImplDX12_RenderDrawData(draw_data, command_list);
		}
	}

	g_d3d.command_queue_direct->ExecuteCommandList(command_list);
}

void RenderBackend::QueueScreenshot(const char *file_name)
{
	g_d3d.queued_screenshot = true;

	size_t name_len = strlen(file_name);
	memcpy(g_d3d.queued_screenshot_name, file_name, RT_MIN(name_len, sizeof(g_d3d.queued_screenshot_name)-1));
    g_d3d.queued_screenshot_name[RT_MIN(name_len,sizeof(g_d3d.queued_screenshot_name)-1)]=0;
}

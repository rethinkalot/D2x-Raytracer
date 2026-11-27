#include "OpenVRPrototype.h"
#include "GlobalDX.h"
#include "RenderBackend.h"
#include "Resource.h"
#include "CommandQueue.h"
#include "CommandList.h"
#include "VRMath.h"
#include "VRLaunchOptions.h"
#include "VRGraphicsSettings.h"
#include "VRMirrorLayout.h"
#include "FSR2.h"
#include <shellapi.h>
#include "GPUProfiler.h"
#include "Core/MiniMath.h"
#include "ImageReadWrite.h"
#include <openvr.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <algorithm>
#include <vector>
#include <DirectXPackedVector.h>

using namespace RT;
namespace {
FrameData eyeFrameData[BACK_BUFFER_COUNT_SWAP][2]={};
FrameData* lastRightFrame;
bool serialStereo;
bool fastSpatial=true;
bool hybridVisibility=false;
ID3D12QueryHeap* eyeQueryHeap;
ID3D12Resource* eyeQueryReadback;
uint64_t gpuFrequency;
double lastEyeGPU[2]={};
float lastRuntimeGPU,lastFrameInterval;
constexpr GPUProfiler::GPUTimer stageTimers[]={GPUProfiler::GPUTimer_PrimaryRay,GPUProfiler::GPUTimer_DirectLighting,
    GPUProfiler::GPUTimer_DenoiseResample,GPUProfiler::GPUTimer_DenoiseSVGF,GPUProfiler::GPUTimer_Bloom,GPUProfiler::GPUTimer_PostProcess,GPUProfiler::GPUTimer_TAA,
    GPUProfiler::GPUTimer_IndirectLighting,GPUProfiler::GPUTimer_BuildTLAS};
constexpr UINT stageCount=static_cast<UINT>(sizeof(stageTimers)/sizeof(stageTimers[0]));
constexpr UINT timingStride=2+stageCount*2;
double lastStageGPU[2][stageCount]={};
double ClockMS() {
    LARGE_INTEGER value,frequency;
    QueryPerformanceCounter(&value); QueryPerformanceFrequency(&frequency);
    return static_cast<double>(value.QuadPart)*1000.0/static_cast<double>(frequency.QuadPart);
}
vr::IVRSystem* systemVR;
bool active, inEyePass, haveOrigin, stereoThisFrame, recenterHeld, resetHistory;
bool offlineTest;
bool levelTest;
unsigned levelPresentedFrames;
bool benchmarking;
bool testingDetail;
int currentEye;
VRMath::Pose origin=VRMath::Identity(), head=VRMath::Identity();
float unitsPerMetre=1.0f;
// 1.0 keeps robots/players/powerups the size of their hitboxes. Set
// D2_VR_OBJECT_SCALE (0.5-1.5) for a different visual size.
float objectScale=1.0f;
// Extra texture mip bias with FSR, on top of log2(render/output). AMD's
// guidance is -1; D2_VR_TEXTURE_BIAS (-2..1) overrides it.
float textureBias=-1.0f;
// FSR RCAS sharpness for the eyes; negative selects the per-preset default.
float fsrSharpness=-1.0f;
// HUD/menu panel distance in metres (D2_VR_HUD_DISTANCE, 0.5-3.0).
float hudDistance=1.6f;
float reticleScale=1.5f;
bool indirectLighting=true;
bool fastVisualReference=false;
uint32_t lightCandidateBudget=0;
VRMath::Pose renderedHead=VRMath::Identity();
ID3D12Resource* eyeTargets[2][RenderTarget_COUNT]={};
GlobalConstantBuffer previousEye[2]={};
RT_Camera reticleBase={},reticleEyes[2]={};
float reticleTangents[2][4]={};
DescriptorAllocation eyeViews[2]={};
DescriptorAllocation mirrorSource={};
uint64_t eyeFrames[2]={};
uint32_t targetWidth, targetHeight;
uint32_t eyeWidth=1280, eyeHeight=1280;
// Two HUD textures, alternated each frame, so SteamVR can still read the
// last submitted panel while the next one is cleared and drawn.
ID3D12Resource* uiTextures[2];
DescriptorAllocation uiViews[2]={};
unsigned uiIndex;
ID3D12Resource* uiTexture; // current entry of uiTextures
ID3D12Resource* aimTexture;
DescriptorAllocation aimView={};
bool aimCapturing, haveAim;
float aimX,aimY,aimZ=1000;
ID3D12Resource* menuBackground;
DescriptorAllocation uiView={}, menuView={};
uint32_t uiWidth,uiHeight;
vr::VROverlayHandle_t uiOverlay=vr::k_ulOverlayHandleInvalid;
bool poseSampled, poseValid;
// True while WaitGetPoses is succeeding and therefore pacing the game loop.
bool compositorPacing;
// Gameplay frames in the current log window, and how many SteamVR had to
// predict two or more extra frames for (the app missed its frame budget).
unsigned budgetFrames, budgetLateFrames;
float lastAppCpu; // SteamVR: poses ready -> second eye submitted, last logged frame
float lastPreSubmitGpu; // SteamVR: all GPU work submitted before the eyes, last logged frame

VRGraphics::Settings graphicsRunning;
bool graphicsPending;
void ApplyPendingGraphics();
void QueueGraphicsSettingsForTest(int mode);
void EnsureUI();

void Log(const char* message) {
    if(benchmarking) return;
    OutputDebugStringA(message);
    if(FILE* f=std::fopen("openvr-prototype.log","a")) {
        std::fprintf(f,"%s\n",message); std::fclose(f);
    }
}
bool HasArgument(const wchar_t* argument) {
    int argc=0;
    auto** argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(!argv) return false;
    const bool result=VRLaunch::HasOption(argc,argv,argument);
    LocalFree(argv);
    return result;
}

FrameData* PrepareEyeFrame(const FrameData& source,int eye) {
    const UINT slot=g_d3d.current_back_buffer_index;
    auto* frame=&eyeFrameData[slot][eye];
    if(!frame->upload_buffer) {
        AllocateFrameDataLike(frame,source,L"VR eye frame upload",RT_MB(4));
    }
    // Only wait when reusing this swap-slot's eye data, never between eyes.
    // The fence covers this eye's query resolve and all its resource reads.
    if(frame->fence_value) g_d3d.command_queue_direct->WaitForFenceValue(frame->fence_value);
    if(eyeQueryReadback && frame->fence_value && gpuFrequency) {
        uint64_t* times=nullptr;
        DX_CALL(eyeQueryReadback->Map(0,nullptr,reinterpret_cast<void**>(&times)));
        const UINT index=(slot*2+static_cast<UINT>(eye))*timingStride;
        if(times[index+1]>=times[index]) lastEyeGPU[eye]=static_cast<double>(times[index+1]-times[index])*1000.0/static_cast<double>(gpuFrequency);
        for(UINT stage=0;stage<stageCount;++stage) {
            const UINT at=index+2+stage*2;
            if(times[at+1]>=times[at]) lastStageGPU[eye][stage]=static_cast<double>(times[at+1]-times[at])*1000.0/static_cast<double>(gpuFrequency);
        }
        eyeQueryReadback->Unmap(0,nullptr);
    }
    RT_ArenaResetToMarker(&frame->upload_buffer_arena,frame->upload_buffer_arena_reset);
    std::memcpy(frame->instance_descs.cpu,source.instance_descs.cpu,sizeof(D3D12_RAYTRACING_INSTANCE_DESC)*g_d3d.tlas_instance_count);
    std::memcpy(frame->instance_data.cpu,source.instance_data.cpu,sizeof(InstanceData)*g_d3d.tlas_instance_count);
    std::memcpy(frame->raster_vertex_counts,source.raster_vertex_counts,sizeof(uint32_t)*g_d3d.tlas_instance_count);
    std::memcpy(frame->lights.cpu,source.lights.cpu,sizeof(RT_Light)*g_d3d.lights_count);
    std::memcpy(frame->scene_cb.cpu,source.scene_cb.cpu,source.scene_cb.size);
    std::memcpy(frame->tweak_vars.cpu,source.tweak_vars.cpu,source.tweak_vars.size);
    if(!eyeQueryHeap) {
        D3D12_QUERY_HEAP_DESC desc={}; desc.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP; desc.Count=BACK_BUFFER_COUNT_SWAP*4;
        DX_CALL(g_d3d.device->CreateQueryHeap(&desc,IID_PPV_ARGS(&eyeQueryHeap)));
        eyeQueryReadback=RT_CreateReadbackBuffer(L"VR GPU timings",sizeof(uint64_t)*BACK_BUFFER_COUNT_SWAP*2*timingStride);
        DX_CALL(g_d3d.command_queue_direct->GetD3D12CommandQueue()->GetTimestampFrequency(&gpuFrequency));
    }
    return frame;
}
struct TestEyeFrameScope {
    FrameData* saved=g_vr_frame_override;
    TestEyeFrameScope() {
        g_d3d.command_queue_direct->Flush();
        if(lastRightFrame) g_vr_frame_override=lastRightFrame;
    }
    ~TestEyeFrameScope() { g_vr_frame_override=saved; }
};
VRMath::Pose Pose(const vr::HmdMatrix34_t& value) {
    VRMath::Pose r; std::memcpy(r.m,value.m,sizeof(r.m)); return r;
}
RT_Vec3 Transform(const RT_Camera& base,float x,float y,float z) {
    return {base.right.x*x+base.up.x*y+base.forward.x*z,
            base.right.y*x+base.up.y*y+base.forward.y*z,
            base.right.z*x+base.up.z*y+base.forward.z*z};
}
RT_Camera EyeCamera(const RT_Camera& base,int eye) {
    auto eyePose=VRMath::Identity();
    if(offlineTest) eyePose.m[0][3]=eye==0 ? -0.032f : 0.032f;
    else eyePose=Pose(systemVR->GetEyeToHeadTransform(static_cast<vr::EVREye>(eye)));
    const auto local=VRMath::RelativeEye(origin,head,eyePose);
    RT_Camera camera=base;
    const RT_Vec3 displacement=Transform(base,local.m[0][3]*unitsPerMetre,local.m[1][3]*unitsPerMetre,local.m[2][3]*unitsPerMetre);
    camera.position={base.position.x+displacement.x,base.position.y+displacement.y,base.position.z+displacement.z};
    camera.right=Transform(base,local.m[0][0],local.m[1][0],local.m[2][0]);
    camera.up=Transform(base,local.m[0][1],local.m[1][1],local.m[2][1]);
    camera.forward=Transform(base,local.m[0][2],local.m[1][2],local.m[2][2]);
    return camera;
}
void ReleaseTargets() {
    g_d3d.command_queue_direct->Flush();
    FSR2::ExitStereo();
    for(auto& targets:eyeTargets) for(auto& target:targets) {
        if(target) RT_RELEASE_RESOURCE(target);
        target=nullptr;
    }
    targetWidth=targetHeight=0;
    eyeFrames[0]=eyeFrames[1]=0;
}
void EnsureTargets() {
    const auto render=VRGraphics::RenderSize(graphicsRunning,eyeWidth,eyeHeight);
    if(targetWidth==eyeWidth && targetHeight==eyeHeight) return;
    ReleaseTargets();
    for(auto& targets:eyeTargets) {
#define CREATE_EYE_TARGET(name, reg, scale_x, scale_y, output_res, type, format) \
        targets[RenderTarget_##name]=RT_CreateTexture(L"OpenVR " RT_PASTE(L, #name),format, \
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, \
            ((output_res ? eyeWidth : render.width)+scale_x-1)/scale_x,((output_res ? eyeHeight : render.height)+scale_y-1)/scale_y,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        RT_RENDER_TARGETS(CREATE_EYE_TARGET)
#undef CREATE_EYE_TARGET
    }
    for(int eye=0;eye<2;++eye) {
        if(eyeViews[eye].IsNull()) eyeViews[eye]=g_d3d.rtv.Allocate(1);
        CreateTextureRTV(eyeTargets[eye][RenderTarget_color_final],eyeViews[eye].cpu,DXGI_FORMAT_R8G8B8A8_UNORM);
        if(eye==0) {
            if(mirrorSource.IsNull()) mirrorSource=g_d3d.cbv_srv_uav.Allocate(1);
            CreateTextureSRV(eyeTargets[0][RenderTarget_color_final],mirrorSource.cpu,DXGI_FORMAT_R8G8B8A8_UNORM);
        }
    }
    if(VRGraphics::Upscaling(graphicsRunning)) FSR2::InitStereo(eyeWidth,eyeHeight);
    targetWidth=eyeWidth; targetHeight=eyeHeight;
    Log("Allocated separate left/right render targets and temporal histories.");
}
bool Tracking() {
    if(offlineTest) return true;
    if(poseSampled) return poseValid;
    poseSampled=true;
    poseValid=false;
    vr::VREvent_t event;
    while(systemVR->PollNextEvent(&event,sizeof(event))) {
        if(event.eventType==vr::VREvent_Quit) {
            Log("SteamVR requested shutdown; releasing VR resources and returning to desktop rendering.");
            // Point 2D drawing back at the desktop canvas before the VR HUD
            // and reticle textures are released, then disconnect from SteamVR
            // completely so it is not left waiting on a half-active client.
            g_d3d_raster.render_target=g_d3d.rt.color_final;
            g_d3d_raster.rtv_handle=g_d3d.color_final_rtv.cpu;
            g_vr_frame_override=nullptr; inEyePass=false; stereoThisFrame=false;
            compositorPacing=false;
            OpenVRPrototype::Shutdown();
            tweak_vars.upscaling_aa_mode=UPSCALING_AA_MODE_OFF;
            g_d3d.io.scene_transition=true;
            return false;
        }
        if(event.eventType==vr::VREvent_SeatedZeroPoseReset) haveOrigin=false;
    }
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]={};
    if(vr::VRCompositor()->WaitGetPoses(poses,vr::k_unMaxTrackedDeviceCount,nullptr,0)!=vr::VRCompositorError_None) {
        // WaitGetPoses returned without blocking (e.g. another app has focus).
        // Let the game's normal frame limiter run instead of spinning uncapped.
        compositorPacing=false;
        resetHistory=true;
        return false;
    }
    compositorPacing=true;
    const auto& hmd=poses[vr::k_unTrackedDeviceIndex_Hmd];
    if(!hmd.bDeviceIsConnected || !hmd.bPoseIsValid) { resetHistory=true; return false; }
    const auto sample=Pose(hmd.mDeviceToAbsoluteTracking);
    if(!VRMath::Finite(sample)) { resetHistory=true; return false; }
    head=sample;
    const bool pressed=GetForegroundWindow()==g_d3d.hWnd &&
        (GetAsyncKeyState(VK_CONTROL)&0x8000) && (GetAsyncKeyState(VK_MENU)&0x8000) &&
        (GetAsyncKeyState(VK_HOME)&0x8000);
    if(!haveOrigin || (pressed && !recenterHeld)) {
        origin=VRMath::YawOnly(head); haveOrigin=true; resetHistory=true;
        Log("Seated view recentered (heading and position only; horizon kept level); ship orientation and weapon aim were not changed.");
    }
    recenterHeld=pressed;
    poseValid=true;
    return poseValid;
}
void Submit(ID3D12Resource* left,ID3D12Resource* right) {
    if(offlineTest) return;
    ID3D12Resource* images[2]={left,right};
    auto& commands=g_d3d.command_queue_direct->GetCommandList();
    for(auto* image:images) ResourceTransition(commands,image,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    g_d3d.command_queue_direct->ExecuteCommandList(commands);
    // SteamVR queues texture transfers on this queue. Keep all submissions on the renderer thread.
    for(int i=0;i<2;++i) {
        vr::D3D12TextureData_t data={images[i],g_d3d.command_queue_direct->GetD3D12CommandQueue(),0};
        vr::VRTextureWithPose_t texture={};
        texture.handle=&data; texture.eType=vr::TextureType_DirectX12; texture.eColorSpace=vr::ColorSpace_Gamma;
        std::memcpy(texture.mDeviceToAbsoluteTracking.m,renderedHead.m,sizeof(renderedHead.m));
        const auto flags=stereoThisFrame ? vr::Submit_TextureWithPose : vr::Submit_Default;
        const auto error=vr::VRCompositor()->Submit(static_cast<vr::EVREye>(i),&texture,nullptr,flags);
        if(error!=vr::VRCompositorError_None) {
            static bool logged;
            if(!logged) { char text[100]; std::snprintf(text,sizeof(text),"SteamVR eye submission failed: %d",static_cast<int>(error)); Log(text); logged=true; }
        }
    }
}

// One transparent, finite-distance panel. SteamVR projects this independently
// into each eye, instead of treating a flat desktop image as two eye images.
void EnsureUI() {
    if(uiTextures[0] && uiWidth==g_d3d.output_width && uiHeight==g_d3d.output_height) return;
    g_d3d.command_queue_direct->Flush();
    for(auto*& texture:uiTextures) { if(texture) RT_RELEASE_RESOURCE(texture); texture=nullptr; }
    if(aimTexture) RT_RELEASE_RESOURCE(aimTexture);
    uiWidth=g_d3d.output_width; uiHeight=g_d3d.output_height;
    for(int i=0;i<2;++i) {
        uiTextures[i]=RT_CreateTexture(i ? L"VR menus and HUD B" : L"VR menus and HUD A",DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,uiWidth,uiHeight,D3D12_RESOURCE_STATE_RENDER_TARGET);
        if(uiViews[i].IsNull()) uiViews[i]=g_d3d.rtv.Allocate(1);
        CreateTextureRTV(uiTextures[i],uiViews[i].cpu,DXGI_FORMAT_R8G8B8A8_UNORM);
    }
    uiTexture=uiTextures[uiIndex]; uiView=uiViews[uiIndex];
    aimTexture=RT_CreateTexture(L"VR weapon reticle",DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,uiWidth,uiHeight,D3D12_RESOURCE_STATE_RENDER_TARGET);
    if(aimView.IsNull()) aimView=g_d3d.rtv.Allocate(1);
    CreateTextureRTV(aimTexture,aimView.cpu,DXGI_FORMAT_R8G8B8A8_UNORM);
    if(!menuBackground) {
        menuBackground=RT_CreateTexture(L"VR menu background",DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,16,16,D3D12_RESOURCE_STATE_RENDER_TARGET);
        if(menuView.IsNull()) menuView=g_d3d.rtv.Allocate(1);
        CreateTextureRTV(menuBackground,menuView.cpu,DXGI_FORMAT_R8G8B8A8_UNORM);
        auto& commands=g_d3d.command_queue_direct->GetCommandList();
        const float black[4]={0,0,0,1};
        commands->ClearRenderTargetView(menuView.cpu,black,0,nullptr);
        g_d3d.command_queue_direct->ExecuteCommandList(commands);
    }
}

void UpdatePanel() {
    if(offlineTest || uiOverlay==vr::k_ulOverlayHandleInvalid || !haveOrigin) return;
    auto local=VRMath::Identity(); local.m[2][3]=-hudDistance;
    const auto panel=VRMath::Multiply(origin,local);
    vr::HmdMatrix34_t placement;
    std::memcpy(placement.m,panel.m,sizeof(placement.m));
    vr::VROverlay()->SetOverlayTransformAbsolute(uiOverlay,vr::TrackingUniverseSeated,&placement);
    auto& commands=g_d3d.command_queue_direct->GetCommandList();
    ResourceTransition(commands,uiTexture,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    g_d3d.command_queue_direct->ExecuteCommandList(commands);
    vr::D3D12TextureData_t data={uiTexture,g_d3d.command_queue_direct->GetD3D12CommandQueue(),0};
    vr::Texture_t texture={&data,vr::TextureType_DirectX12,vr::ColorSpace_Gamma};
    const auto error=vr::VROverlay()->SetOverlayTexture(uiOverlay,&texture);
    if(error==vr::VROverlayError_None) vr::VROverlay()->ShowOverlay(uiOverlay);
    else {
        static bool logged;
        if(!logged) { char text[100]; std::snprintf(text,sizeof(text),"SteamVR UI panel submission failed: %d",static_cast<int>(error)); Log(text); logged=true; }
    }
}

void MirrorLeftEye() {
    // The source resource stays fixed until EnsureTargets flushes on resize.
    // Rewriting this descriptor is safe only there, so initialize it there.
    auto& commands=g_d3d.command_queue_direct->GetCommandList();
    ResourceTransition(commands,eyeTargets[0][RenderTarget_color_final],D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ResourceTransition(commands,g_d3d.rt.color_final,D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12DescriptorHeap* heaps[]={g_d3d.cbv_srv_uav.GetHeap()};
    commands->SetDescriptorHeaps(1,heaps);
    const float width=static_cast<float>(g_d3d.output_width),height=static_cast<float>(g_d3d.output_height);
    const float scale=graphicsRunning.mirror==1 ? std::min(width/eyeWidth,height/eyeHeight) : std::max(width/eyeWidth,height/eyeHeight);
    const float drawWidth=eyeWidth*scale,drawHeight=eyeHeight*scale;
    D3D12_VIEWPORT viewport={0,0,width,height,0,1};
    D3D12_RECT scissor={0,0,static_cast<LONG>(g_d3d.output_width),static_cast<LONG>(g_d3d.output_height)};
    commands->RSSetViewports(1,&viewport); commands->RSSetScissorRects(1,&scissor);
    commands->OMSetRenderTargets(1,&g_d3d.color_final_rtv.cpu,FALSE,nullptr);
    if(graphicsRunning.mirror!=0) { const float black[4]={0,0,0,1};commands->ClearRenderTargetView(g_d3d.color_final_rtv.cpu,black,0,nullptr); }
    if(graphicsRunning.mirror==2) { g_d3d.command_queue_direct->ExecuteCommandList(commands);return; }
    commands->SetPipelineState(g_d3d_raster.blit_state);
    commands->SetGraphicsRootSignature(g_d3d_raster.blit_root_sig);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const float constants[7]={(width-drawWidth)*0.5f,(height-drawHeight)*0.5f,drawWidth,drawHeight,0,width,height};
    commands->SetGraphicsRoot32BitConstants(0,7,constants,0);
    commands->SetGraphicsRootDescriptorTable(1,mirrorSource.gpu);
    commands->DrawInstanced(6,1,0,0);
    g_d3d.command_queue_direct->ExecuteCommandList(commands);
}

uint64_t SaveEye(ID3D12Resource* image,const char* path) {
    const auto desc=image->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint={};
    UINT64 total=0;
    g_d3d.device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&total);
    ID3D12Resource* readback=RT_CreateReadbackBuffer(L"VR self-test readback",static_cast<size_t>(total));
    auto& commands=g_d3d.command_queue_direct->GetCommandList();
    ResourceTransition(commands,image,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src={}; src.pResource=image; src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst={}; dst.pResource=readback; dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=footprint;
    commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    g_d3d.command_queue_direct->ExecuteCommandList(commands);
    g_d3d.command_queue_direct->Flush();
    unsigned char* pixels=nullptr;
    DX_CALL(readback->Map(0,nullptr,reinterpret_cast<void**>(&pixels)));
    RT_WritePNGToDisk(path,static_cast<int>(desc.Width),static_cast<int>(desc.Height),4,pixels,footprint.Footprint.RowPitch);
    uint64_t hash=1469598103934665603ull;
    for(UINT y=0;y<desc.Height;++y) for(size_t x=0;x<static_cast<size_t>(desc.Width)*4;++x) {
        hash^=pixels[y*footprint.Footprint.RowPitch+x]; hash*=1099511628211ull;
    }
    readback->Unmap(0,nullptr); RT_RELEASE_RESOURCE(readback);
    return hash;
}

void TestSpatialDenoising() {
    TestEyeFrameScope frameScope;
    // Exercise the real five-pass shader with noisy lighting, a firefly and a
    // material boundary. The last eye's descriptor table is still installed.
    using namespace DirectX;
    using namespace DirectX::PackedVector;
    const size_t count=static_cast<size_t>(eyeWidth)*eyeHeight;
    std::vector<XMFLOAT3PK> lighting(count);
    std::vector<XMHALF4> moments(count);
    std::vector<uint16_t> material(count);
    uint32_t random=12345;
    for(uint32_t y=0;y<eyeHeight;++y) for(uint32_t x=0;x<eyeWidth;++x) {
        random=random*1664525u+1013904223u;
        const float mean=x<eyeWidth/2 ? 0.5f : 1.5f;
        float value=(random & 0x80000000u) ? mean*2 : 0;
        if(x==eyeWidth/4 && y==eyeHeight/2) value=8;
        const size_t i=static_cast<size_t>(y)*eyeWidth+x;
        XMStoreFloat3PK(&lighting[i],XMVectorReplicate(value));
        moments[i]=XMHALF4(value,value*value,value,value*value);
        material[i]=x<eyeWidth/2 ? 1 : 2;
    }
    auto Upload=[&](RenderTarget target,void* data,size_t bits) {
        void* mips[]={data};
        UploadTextureData(eyeTargets[1][target],eyeWidth,eyeHeight,bits,mips,1);
        FlushRingBuffer(&g_d3d.resource_upload_ring_buffer);
    };
    Upload(RenderTarget_diff_denoise_ping,lighting.data(),32);
    Upload(RenderTarget_spec_denoise_ping,lighting.data(),32);
    Upload(RenderTarget_moments,moments.data(),64);
    Upload(RenderTarget_moments_hist,moments.data(),64);
    Upload(RenderTarget_material,material.data(),16);
    Upload(RenderTarget_material_prev,material.data(),16);
    std::vector<uint32_t> normal(count,0);
    std::vector<float> depth(count,1);
    std::vector<uint8_t> metallic(count,0);
    std::vector<uint16_t> history(count,0);
    Upload(RenderTarget_normal,normal.data(),32);
    Upload(RenderTarget_normal_prev,normal.data(),32);
    Upload(RenderTarget_depth,depth.data(),32);
    Upload(RenderTarget_depth_prev,depth.data(),32);
    Upload(RenderTarget_metallic,metallic.data(),8);
    Upload(RenderTarget_metallic_prev,metallic.data(),8);
    Upload(RenderTarget_history_length,history.data(),16);
    auto& commands=g_d3d.command_queue_direct->GetCommandList();
    ID3D12DescriptorHeap* heaps[]={g_d3d.cbv_srv_uav.GetHeap()};
    commands->SetDescriptorHeaps(1,heaps);
    commands->SetComputeRootSignature(g_d3d.global_root_sig);
    commands->SetComputeRootDescriptorTable(RaytraceRootParameters_MainDescriptorTable,CurrentFrameData()->descriptors.gpu);
    commands->SetComputeRootDescriptorTable(RaytraceRootParameters_BindlessSRVTable,g_d3d.cbv_srv_uav.GetGPUBase());
    commands->SetComputeRootDescriptorTable(RaytraceRootParameters_BindlessTriangleBufferTable,g_d3d.cbv_srv_uav.GetGPUBase());
    commands->SetPipelineState(fastSpatial ? g_d3d.cs.svgf_blur_vr.pso : g_d3d.cs.svgf_blur.pso);
    for(UINT step=0;step<5;++step) {
        commands->SetComputeRoot32BitConstant(RaytraceRootParameters_DenoiseIteration,step,0);
        commands->Dispatch((eyeWidth+GROUP_X-1)/GROUP_X,(eyeHeight+GROUP_Y-1)/GROUP_Y,1);
        D3D12_RESOURCE_BARRIER barrier={}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
        commands->ResourceBarrier(1,&barrier);
    }
    auto* image=eyeTargets[1][RenderTarget_diff_denoise_ping];
    const auto desc=image->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint={}; UINT64 total=0;
    g_d3d.device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&total);
    auto* readback=RT_CreateReadbackBuffer(L"VR denoising test",static_cast<size_t>(total));
    ResourceTransition(commands,image,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src={}; src.pResource=image; src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst={}; dst.pResource=readback; dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=footprint;
    commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    g_d3d.command_queue_direct->ExecuteCommandList(commands); g_d3d.command_queue_direct->Flush();
    unsigned char* pixels=nullptr; DX_CALL(readback->Map(0,nullptr,reinterpret_cast<void**>(&pixels)));
    double error=0; size_t samples=0; float firefly=0,edgeLeft=0,edgeRight=0;
    for(uint32_t y=32;y<eyeHeight-32;++y) for(uint32_t x=32;x<eyeWidth/2-32;++x) {
        const auto* pixel=reinterpret_cast<const XMFLOAT3PK*>(pixels+y*footprint.Footprint.RowPitch)+x;
        const float value=XMVectorGetX(XMLoadFloat3PK(pixel));
        error+=(value-0.5)*(value-0.5); ++samples;
    }
    auto Pixel=[&](uint32_t x) {
        return XMVectorGetX(XMLoadFloat3PK(reinterpret_cast<const XMFLOAT3PK*>(pixels+(eyeHeight/2)*footprint.Footprint.RowPitch)+x));
    };
    firefly=Pixel(eyeWidth/4); edgeLeft=Pixel(eyeWidth/2-1); edgeRight=Pixel(eyeWidth/2);
    const double rms=std::sqrt(error/static_cast<double>(samples));
    const bool pass=std::isfinite(rms) && rms<0.1 && firefly<0.8f && edgeLeft<0.8f && edgeRight>1.0f;
    char result[220]; std::snprintf(result,sizeof(result),"%s: GPU spatial denoising RMS %.4f (input 0.5); firefly %.3f (input 8); material edge %.3f / %.3f.",pass ? "PASS" : "FAIL",rms,firefly,edgeLeft,edgeRight); Log(result);
    readback->Unmap(0,nullptr); RT_RELEASE_RESOURCE(readback);
}

void TestSpatialAA() {
    TestEyeFrameScope frameScope;
    using namespace DirectX::PackedVector;
    std::vector<XMHALF4> pattern(static_cast<size_t>(eyeWidth)*eyeHeight);
    for(uint32_t y=0;y<eyeHeight;++y) for(uint32_t x=0;x<eyeWidth;++x) {
        const float value=static_cast<float>(y)<static_cast<float>(x)*0.43f+200.0f ? 1.0f : 0.0f;
        pattern[static_cast<size_t>(y)*eyeWidth+x]=XMHALF4(value,value,value,1);
    }
    auto* frame=CurrentFrameData();
    auto* cb=frame->scene_cb.As<GlobalConstantBuffer>();
    auto* vars=frame->tweak_vars.As<TweakVars>();
    const auto savedCB=*cb; const auto savedVars=*vars;
    vars->bloom_amount=0; vars->vignette_strength=0; vars->motion_blur_quality=0;
    cb->screen_color_overlay={};
    // Same target the last right-eye RaytraceRender wrote (see EyeHistoryIndex).
    const auto target=((eyeFrames[1]+1)&1) ? RenderTarget_taa_history : RenderTarget_taa_result;
    auto* input=eyeTargets[1][target];
    auto* output=eyeTargets[1][RenderTarget_postfx];
    void* mips[]={pattern.data()};
    UploadTextureData(input,eyeWidth,eyeHeight,64,mips,1);
    FlushRingBuffer(&g_d3d.resource_upload_ring_buffer);
    for(int enabled=0;enabled<2;++enabled) {
        cb->debug_flags=(savedCB.debug_flags & ~0x80000000u) | (enabled ? 0x80000000u : 0u);
        auto& commands=g_d3d.command_queue_direct->GetCommandList();
        ResourceTransition(commands,input,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ResourceTransition(commands,output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap* heaps[]={g_d3d.cbv_srv_uav.GetHeap()};
        commands->SetDescriptorHeaps(1,heaps);
        commands->SetComputeRootSignature(g_d3d.global_root_sig);
        commands->SetComputeRootDescriptorTable(RaytraceRootParameters_MainDescriptorTable,frame->descriptors.gpu);
        commands->SetComputeRootDescriptorTable(RaytraceRootParameters_BindlessSRVTable,g_d3d.cbv_srv_uav.GetGPUBase());
        commands->SetComputeRootDescriptorTable(RaytraceRootParameters_BindlessTriangleBufferTable,g_d3d.cbv_srv_uav.GetGPUBase());
        commands->SetPipelineState(g_d3d.cs.post_process.pso);
        commands->Dispatch((eyeWidth+GROUP_X-1)/GROUP_X,(eyeHeight+GROUP_Y-1)/GROUP_Y,1);
        g_d3d.command_queue_direct->ExecuteCommandList(commands); g_d3d.command_queue_direct->Flush();
        SaveEye(output,enabled ? "vr-selftest-aa-on.png" : "vr-selftest-aa-off.png");
    }
    *cb=savedCB; *vars=savedVars;
}

void TestUploadWrap() {
    const bool savedEyePass=inEyePass;
    inEyePass=true;
    auto ring=CreateRingBuffer(1024);
    constexpr size_t blocks=36, blockSize=256;
    auto* destination=RT_CreateReadWriteBuffer(L"VR upload wrap destination",blocks*blockSize);
    auto* readback=RT_CreateReadbackBuffer(L"VR upload wrap readback",blocks*blockSize);
    for(size_t i=0;i<blocks;++i) {
        unsigned char bytes[blockSize]; std::memset(bytes,static_cast<int>(i+1),blockSize);
        const auto allocation=WriteToRingBuffer(&ring,blockSize,256,bytes);
        ResourceTransition(*allocation.command_list,destination,D3D12_RESOURCE_STATE_COPY_DEST);
        allocation.command_list->GetD3D12CommandList()->CopyBufferRegion(destination,i*blockSize,allocation.resource,allocation.byte_offset,blockSize);
        if(i%3==0) FlushRingBuffer(&ring);
    }
    FlushRingBuffer(&ring);
    auto& commands=g_d3d.command_queue_direct->GetCommandList();
    ResourceTransition(commands,destination,D3D12_RESOURCE_STATE_COPY_SOURCE);
    commands->CopyBufferRegion(readback,0,destination,0,blocks*blockSize);
    g_d3d.command_queue_direct->ExecuteCommandList(commands); g_d3d.command_queue_direct->Flush();
    unsigned char* bytes=nullptr; DX_CALL(readback->Map(0,nullptr,reinterpret_cast<void**>(&bytes)));
    bool pass=true;
    for(size_t i=0;i<blocks;++i) for(size_t j=0;j<blockSize;++j)
        pass &= bytes[i*blockSize+j]==i+1;
    readback->Unmap(0,nullptr);
    Log(pass ? "PASS: asynchronous upload data survived repeated exact-boundary wraps and partial submissions." : "FAIL: upload ring contents changed before GPU copies completed.");
    g_d3d.command_queue_direct->ExecuteCommandList(*ring.command_list); g_d3d.command_queue_direct->Flush();
    RT_RELEASE_RESOURCE(destination); RT_RELEASE_RESOURCE(readback); RT_RELEASE_RESOURCE(ring.resource);
    inEyePass=savedEyePass;
}

void TestHybridVisibilityCases(RT_Camera camera) {
    const bool savedHybrid=hybridVisibility;
    const auto savedHead=head; head=VRMath::Identity();
    std::vector<uint32_t> pixels(64*64);
    for(UINT y=0;y<64;++y) for(UINT x=0;x<64;++x)
        pixels[y*64+x]=((x/8+y/8)&1) ? 0xFF1450FFu : 0x001450FFu;
    RT_UploadTextureParams upload={}; upload.image.width=64; upload.image.height=64;
    upload.image.format=RT_TextureFormat_RGBA8; upload.image.pixels=pixels.data();
    upload.name="DMAX 11 VR alpha visibility fixture";
    const auto texture=RenderBackend::UploadTexture(upload);
    RT_Material material={}; material.albedo_texture=texture;
    material.flags=RT_MaterialFlag_BlackbodyRadiator;
    material.emissive_color={1,1,1}; material.emissive_strength=4;
    RenderBackend::UpdateMaterial(RT_MATERIAL_FLAT_WHITE,&material);
    g_rt_material_indices[RT_MATERIAL_FLAT_WHITE]=RT_MATERIAL_FLAT_WHITE;
    RT_Triangle tiles[2]={};
    const RT_Vec3 corners[4]={{-1,-1,0},{1,-1,0},{1,1,0},{-1,1,0}};
    const RT_Vec2 uvs[4]={{1,1},{0,1},{0,0},{1,0}};
    const int indices[2][3]={{0,1,2},{0,2,3}};
    for(int tri=0;tri<2;++tri) {
        for(int v=0;v<3;++v) {
            tiles[tri].positions[v]=corners[indices[tri][v]];
            tiles[tri].normals[v]={0,0,1}; tiles[tri].tangents[v]={1,0,0,1};
            tiles[tri].uvs[v]=uvs[indices[tri][v]];
        }
        tiles[tri].color=0xFFFFFFFFu;
        tiles[tri].material_edge_index=RT_TRIANGLE_MATERIAL_INSTANCE_OVERRIDE;
        tiles[tri].segment=7; tiles[tri].segment_adjacent=0;
    }
    RT_UploadMeshParams meshUpload={}; meshUpload.triangle_count=2; meshUpload.triangles=tiles;
    meshUpload.name="DMAX 11 VR segment fixture";
    const auto mesh=RenderBackend::UploadMesh(meshUpload);
    const bool savedRetrace=tweak_vars.retrace_rays; tweak_vars.retrace_rays=true;
    for(int test=0;test<5;++test) for(int hybrid=0;hybrid<2;++hybrid) {
        hybridVisibility=hybrid!=0;
        RT_SceneSettings settings={}; settings.camera=&camera; settings.render_segment=test>=3 ? 0 : -1;
        RenderBackend::BeginScene(&settings); g_d3d.scene.prev_camera=camera;
        const RT_Vec3 background={0,0,test>=3 ? 1.5f : -0.5f};
        RenderBackend::RaytraceBillboardColored(RT_MATERIAL_EMISSIVE_WHITE,{0.03f,0.15f,1},{2,2},background,background);
        if(test>=3) {
            RT_Mat4 transform=RT_Mat4FromBasisVectors({1.2f,0,0},{0,0.8f,0},{0,0,-1});
            RT_RenderMeshParams draw={}; draw.mesh_handle=mesh; draw.transform=&transform; draw.prev_transform=&transform;
            draw.color=0xFFFFFFFFu; draw.material_override=RT_MATERIAL_FLAT_WHITE; draw.instance_mask=RT_RENDER_MASK_LEVELGEOMETRY;
            RenderBackend::RaytraceMesh(draw);
            if(test==4) {
                transform=RT_Mat4Mul(RT_Mat4FromTranslation({0,0,-1}),RT_Mat4FromBasisVectors({2,0,0},{0,2,0},{0,0,-1}));
                draw.flags=RT_RenderMeshFlags_ReverseCulling; draw.instance_mask=RT_RENDER_MASK_PORTALS;
                RenderBackend::RaytraceMesh(draw);
            }
        } else {
            RenderBackend::RaytraceBillboardColored(RT_MATERIAL_FLAT_WHITE,{1,1,1},{1.2f,0.8f},{0,0,0},{0,0,0});
            const auto at=g_d3d.tlas_instance_count-1;
            CurrentFrameData()->instance_descs.As<D3D12_RAYTRACING_INSTANCE_DESC>()[at].InstanceMask=RT_RENDER_MASK_COCKPIT;
            CurrentFrameData()->instance_data.As<InstanceData>()[at].instance_mask=RT_RENDER_MASK_COCKPIT;
            if(test==1) CurrentFrameData()->instance_descs.As<D3D12_RAYTRACING_INSTANCE_DESC>()[at].Flags|=D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE;
            if(test==2) {
                auto* input=CurrentFrameData();
                input->instance_descs.As<D3D12_RAYTRACING_INSTANCE_DESC>()[at].Transform[0][0]*=-1;
                input->instance_data.As<InstanceData>()[at].object_to_world.e[0][0]*=-1;
                input->instance_data.As<InstanceData>()[at].world_to_object=RT_Mat4Inverse(input->instance_data.As<InstanceData>()[at].object_to_world);
            }
        }
        OpenVRPrototype::RenderScene();
        char path[100]; std::snprintf(path,sizeof(path),"vr-selftest-visibility-%d-%s.png",test,hybrid ? "hybrid" : "rays");
        SaveEye(eyeTargets[0][RenderTarget_color_final],path);
        RenderBackend::EndFrame();
    }
    tweak_vars.retrace_rays=savedRetrace;
    RenderBackend::ReleaseMesh(mesh); RenderBackend::ReleaseTexture(texture);
    hybridVisibility=savedHybrid; head=savedHead;
    Log("Captured hybrid/ray visibility cases: alpha cockpit, reverse culling, mirrored mesh, rejected and connected segments.");
}

void TestStockLightingMaterial(RT_Camera camera) {
    graphicsRunning.mode=0;
    RenderBackend::EndFrame(); // Commit shader definitions before comparisons.
    uint32_t white=0xFFFFFFFFu;
    RT_UploadTextureParams upload={};upload.image.width=upload.image.height=1;
    upload.image.format=RT_TextureFormat_RGBA8;upload.image.pixels=&white;
    const auto texture=RenderBackend::UploadTexture(upload);
    RT_Material material={};material.albedo_texture=texture;material.emissive_texture=texture;
    material.emissive_color={1,1,1};material.emissive_strength=0.08f;material.roughness=1;
    g_rt_material_indices[RT_MATERIAL_FLAT_WHITE]=RT_MATERIAL_FLAT_WHITE;
    for(int stock=0;stock<2;++stock) for(int lit=0;lit<2;++lit) {
        material.flags=stock ? RT_MaterialFlag_GameBitmap : 0;
        RenderBackend::UpdateMaterial(RT_MATERIAL_FLAT_WHITE,&material);
        RT_Triangle triangles[2]={};
        RT_Vec3 positions[4]={{-1,-1,0},{1,-1,0},{1,1,0},{-1,1,0}};
        const int indices[2][3]={{0,1,2},{0,2,3}};
        for(int t=0;t<2;++t) {
            for(int v=0;v<3;++v) {
                triangles[t].positions[v]=positions[indices[t][v]];
                triangles[t].normals[v]={0,0,1};triangles[t].tangents[v]={1,0,0,1};
                triangles[t].uvs[v]={0.5f,0.5f};
            }
            triangles[t].color=0xFFFFFFFFu;triangles[t].material_edge_index=RT_TRIANGLE_MATERIAL_INSTANCE_OVERRIDE;
            triangles[t].segment=triangles[t].segment_adjacent=-1;
            triangles[t].baked_lighting=lit ? 0x3FFFFFFFu : 0;
        }
        RT_UploadMeshParams mesh={};mesh.triangles=triangles;mesh.triangle_count=2;
        const auto handle=RenderBackend::UploadMesh(mesh);
        RT_SceneSettings scene={};scene.camera=&camera;scene.render_segment=-1;
        RenderBackend::BeginScene(&scene);g_d3d.scene.prev_camera=camera;g_d3d.io.scene_transition=true;
        RT_Mat4 transform=RT_Mat4FromTranslation({0,0,0});
        RT_RenderMeshParams draw={};draw.mesh_handle=handle;draw.transform=&transform;draw.prev_transform=&transform;
        draw.flags=RT_RenderMeshFlags_ReverseCulling;
        draw.color=0xFFFFFFFFu;draw.material_override=RT_MATERIAL_FLAT_WHITE;draw.instance_mask=RT_RENDER_MASK_LEVELGEOMETRY;
        RenderBackend::RaytraceMesh(draw);OpenVRPrototype::RenderScene();
        char name[80];std::snprintf(name,sizeof(name),"vr-stock-material-%s-%d.png",stock ? "stock" : "replacement",lit);
        SaveEye(eyeTargets[0][RenderTarget_color_final],name);
        RenderBackend::ReleaseMesh(handle);RenderBackend::EndFrame();
    }
    RenderBackend::ReleaseTexture(texture);
}

void RunOfflineTest() {
    Log("Starting synthetic stereo GPU self-test; no headset/runtime is used.");
    offlineTest=true; active=true; haveOrigin=true;
    if(HasArgument(L"-openvr-fsr-quality-test")) graphicsRunning.mode=3;
    if(HasArgument(L"-openvr-fsr-balanced-test")) graphicsRunning.mode=4;
    if(HasArgument(L"-openvr-fsr-performance-test")) graphicsRunning.mode=5;
    if(HasArgument(L"-openvr-native-output-test")) { eyeWidth=2232;eyeHeight=2292; }
    TestUploadWrap();
    g_d3d.upscaling_aa_mode=UPSCALING_AA_MODE_OFF;
    tweak_vars.upscaling_aa_mode=UPSCALING_AA_MODE_OFF;
    RenderBackend::OnWindowResize(g_d3d.output_width,g_d3d.output_height);
    RT_Camera camera={}; camera.position={0,0,-3}; camera.right={1,0,0}; camera.up={0,1,0}; camera.forward={0,0,1}; camera.vfov=90;
    if(HasArgument(L"-openvr-stock-material-test")) { TestStockLightingMaterial(camera);return; }
    RT_SceneSettings scene={}; scene.camera=&camera; scene.render_segment=-1;
    RenderBackend::BeginScene(&scene);
    g_d3d.scene.prev_camera=camera;
    g_rt_material_indices[RT_MATERIAL_EMISSIVE_WHITE]=RT_MATERIAL_EMISSIVE_WHITE;
    RenderBackend::RaytraceBillboardColored(RT_MATERIAL_EMISSIVE_WHITE,{1.0f,0.4f,0.1f},{1.0f,1.0f},{0.3f,0.2f,0.0f},{0.3f,0.2f,0.0f});
    if(OpenVRPrototype::RenderScene()) {
        const uint64_t left=SaveEye(eyeTargets[0][RenderTarget_color_final],"vr-selftest-left.png");
        const uint64_t right=SaveEye(eyeTargets[1][RenderTarget_color_final],"vr-selftest-right.png");
        Log(left!=right ? "PASS: separate stereo GPU outputs rendered, read back, and differ." : "FAIL: stereo GPU outputs were identical.");
        const auto savedFrame=g_d3d.frame_index;
        ++g_d3d.frame_index;
        RenderBackend::BeginScene(&scene);
        RenderBackend::RaytraceBillboardColored(RT_MATERIAL_EMISSIVE_WHITE,{1.0f,0.4f,0.1f},{1.0f,1.0f},{-0.9f,0.2f,0.0f},{0.3f,0.2f,0.0f});
        if(OpenVRPrototype::RenderScene())
            SaveEye(eyeTargets[0][RenderTarget_color_final],"vr-selftest-moved.png");
        g_d3d.frame_index=savedFrame;
    } else Log("FAIL: stereo render did not execute.");
    if(HasArgument(L"-openvr-native-output-test")) {
        RenderBackend::EndFrame();
        for(int mode : {0,3,4,5,0}) {
            QueueGraphicsSettingsForTest(mode);
            for(int frame=0;frame<3;++frame) {
                ++g_d3d.frame_index;RenderBackend::BeginScene(&scene);
                RenderBackend::RaytraceBillboardColored(RT_MATERIAL_EMISSIVE_WHITE,{1.0f,0.4f,0.1f},{1.0f,1.0f},{0.3f,0.2f,0.0f},{0.3f,0.2f,0.0f});
                OpenVRPrototype::RenderScene();RenderBackend::EndFrame();
            }
            RT_ASSERT(eyeWidth==2232 && eyeHeight==2292);
            char name[96];std::snprintf(name,sizeof(name),"vr-native-output-mode-%d.png",mode);
            SaveEye(eyeTargets[0][RenderTarget_color_final],name);
        }
        Log("PASS: full native output retained across Off/Quality/Balanced/Performance/Off transitions.");
        return;
    }
    if(HasArgument(L"-openvr-mirror-isolation-test")) {
        EnsureUI();
        auto* canvas=g_d3d.rt.color_final;auto* scratch=g_d3d.rt.depth;
        auto* hud=uiTexture;auto* reticle=aimTexture;auto* eye=eyeTargets[0][RenderTarget_color_final];
        const uint64_t before=SaveEye(eye,"vr-mirror-eye-before.png");
        RECT originalWindow={};::GetWindowRect(g_d3d.hWnd,&originalWindow);
        const unsigned originalWidth=g_d3d.window_width,originalHeight=g_d3d.window_height;
        const unsigned sizes[][2]={{1280,720},{1920,1080},{3840,2160},{1280,960},{3440,1440}};
        for(const auto& size:sizes) {
            RECT bounds={0,0,(LONG)size[0],(LONG)size[1]};
            ::AdjustWindowRectEx(&bounds,(DWORD)::GetWindowLongPtr(g_d3d.hWnd,GWL_STYLE),FALSE,(DWORD)::GetWindowLongPtr(g_d3d.hWnd,GWL_EXSTYLE));
            ::SetWindowPos(g_d3d.hWnd,nullptr,0,0,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            RenderBackend::OnWindowResize(size[0],size[1]);EnsureUI();
            if(g_d3d.output_width!=RT_VR_MIRROR_WIDTH || g_d3d.output_height!=RT_VR_MIRROR_HEIGHT) { Log("FAIL: monitor resize changed VR canvas dimensions.");ExitProcess(3); }
            if(canvas!=g_d3d.rt.color_final || scratch!=g_d3d.rt.depth || hud!=uiTexture || reticle!=aimTexture || eye!=eyeTargets[0][RenderTarget_color_final]) { Log("FAIL: monitor resize recreated a VR render resource.");ExitProcess(3); }
            if(before!=SaveEye(eye,"vr-mirror-eye-after.png")) { Log("FAIL: monitor resize changed headset image pixels.");ExitProcess(3); }
            char screenshot[100];std::snprintf(screenshot,sizeof(screenshot),"vr-desktop-screenshot-%ux%u.png",size[0],size[1]);
            RenderBackend::QueueScreenshot(screenshot);
            RenderBackend::BlitVRMirror();
            auto* target=g_d3d.back_buffers[g_d3d.current_back_buffer_index];
            if(target->GetDesc().Width!=RT_VR_MIRROR_WIDTH || target->GetDesc().Height!=RT_VR_MIRROR_HEIGHT) { Log("FAIL: VR presentation buffers changed dimensions.");ExitProcess(3); }
            char name[100];std::snprintf(name,sizeof(name),"vr-mirror-window-%ux%u.png",size[0],size[1]);SaveEye(target,name);
            auto& restore=g_d3d.command_queue_direct->GetCommandList();ResourceTransition(restore,target,D3D12_RESOURCE_STATE_PRESENT);g_d3d.command_queue_direct->ExecuteCommandList(restore);
        }
        ::SetWindowPos(g_d3d.hWnd,nullptr,0,0,originalWindow.right-originalWindow.left,originalWindow.bottom-originalWindow.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        RenderBackend::OnWindowResize(originalWidth,originalHeight);
        Log("PASS: monitor sizes 720p/1080p/4K/4:3/ultrawide retain eye pixels and all VR canvas/HUD/reticle/scratch resources.");
        ExitProcess(0);
    }
    if(VRGraphics::Upscaling(graphicsRunning)) {
        RenderBackend::EndFrame(); // Freeze shader definitions between synthetic frames.
        for(unsigned frame=0;frame<32;++frame) {
            ++g_d3d.frame_index;
            head.m[0][3]=static_cast<float>(frame)*0.003f;
            RenderBackend::BeginScene(&scene);
            RenderBackend::RaytraceBillboardColored(RT_MATERIAL_EMISSIVE_WHITE,{1.0f,0.4f,0.1f},{1.0f,1.0f},{-0.9f,0.2f,0.0f},{0.3f,0.2f,0.0f});
            OpenVRPrototype::RenderScene();
            RenderBackend::EndFrame();
        }
        SaveEye(eyeTargets[0][RenderTarget_color_final],"vr-selftest-fsr-left.png");
        SaveEye(eyeTargets[1][RenderTarget_color_final],"vr-selftest-fsr-right.png");
        Log("PASS: FSR 3.1 stereo history rendered 32 moving-head frames.");
        if(HasArgument(L"-openvr-live-settings-test")) {
            for(int mode : {4,5,0,3,3}) {
                QueueGraphicsSettingsForTest(mode);
                for(int frame=0;frame<3;++frame) {
                    ++g_d3d.frame_index;
                    RenderBackend::BeginScene(&scene);
                    RenderBackend::RaytraceBillboardColored(RT_MATERIAL_EMISSIVE_WHITE,{1.0f,0.4f,0.1f},{1.0f,1.0f},{-0.9f,0.2f,0.0f},{0.3f,0.2f,0.0f});
                    OpenVRPrototype::RenderScene();RenderBackend::EndFrame();
                }
                const auto expected=VRGraphics::RenderSize(graphicsRunning,eyeWidth,eyeHeight);
                const auto actual=eyeTargets[0][RenderTarget_color]->GetDesc();
                RT_ASSERT(actual.Width==expected.width && actual.Height==expected.height);
                char path[96];std::snprintf(path,sizeof(path),"vr-selftest-live-mode-%d.png",mode);
                SaveEye(eyeTargets[0][RenderTarget_color_final],path);
            }
            Log("PASS: live Quality/Balanced/Performance/Off transitions and unchanged selection rendered without restart.");
        }

        RT_RasterTriVertex vertices[6]={};
        const RT_Vec3 positions[6]={{-0.035f,-0.035f,0},{-0.035f,0.035f,0},{0.035f,0.035f,0},
                                  {-0.035f,-0.035f,0},{0.035f,0.035f,0},{0.035f,-0.035f,0}};
        for(int i=0;i<6;++i) { vertices[i].pos=positions[i];vertices[i].color={0,1,0,1}; }
        RT_RasterTrianglesParams quad={RT_RESOURCE_HANDLE_NULL,vertices,6};
        EnsureUI();RenderBackend::RasterSetViewport(0,0,static_cast<float>(uiWidth),static_cast<float>(uiHeight));
        for(int step=0;step<3;++step) {
            head=step ? VRMath::Pose{{{0.9659258f,0,0.2588190f,0},{0,1,0,0},{-0.2588190f,0,0.9659258f,0}}} : VRMath::Identity();
            ++g_d3d.frame_index;RenderBackend::BeginScene(&scene);
            OpenVRPrototype::RenderScene();RenderBackend::EndFrame();
            if(step<2) {
                OpenVRPrototype::BeginReticle(0,-5,50);
                RenderBackend::RasterTriangles(&quad,1);OpenVRPrototype::EndReticle();
            }
            SaveEye(eyeTargets[0][RenderTarget_color_final],step==0 ? "vr-selftest-fsr-reticle-a.png" : (step==1 ? "vr-selftest-fsr-reticle-b.png" : "vr-selftest-fsr-reticle-cleared.png"));
        }
        return; // Keep the spatial-only fixtures separate from FSR captures.
    }
    TestSpatialDenoising();
    EnsureUI();
    auto& commands=g_d3d.command_queue_direct->GetCommandList();
    const float transparent[4]={0,0,0,0};
    ResourceTransition(commands,uiTexture,D3D12_RESOURCE_STATE_RENDER_TARGET);
    commands->ClearRenderTargetView(uiView.cpu,transparent,0,nullptr);
    g_d3d.command_queue_direct->ExecuteCommandList(commands);
    RenderBackend::RasterSetRenderTarget(RT_RESOURCE_HANDLE_NULL);
    RenderBackend::RasterSetViewport(0,0,static_cast<float>(uiWidth),static_cast<float>(uiHeight));
    RT_RasterTriVertex vertices[6]={};
    const RT_Vec3 positions[6]={{-0.5f,-0.5f,0},{-0.5f,0.5f,0},{0.5f,0.5f,0},
                              {-0.5f,-0.5f,0},{0.5f,0.5f,0},{0.5f,-0.5f,0}};
    for(int i=0;i<6;++i) { vertices[i].pos=positions[i]; vertices[i].color={1.0f,0.5f,0.25f,0.5f}; }
    RT_RasterTrianglesParams quad={RT_RESOURCE_HANDLE_NULL,vertices,6};
    RenderBackend::RasterTriangles(&quad,1);
    RenderBackend::RasterRender();
    const auto uiHash=SaveEye(uiTexture,"vr-selftest-ui.png");
    SaveEye(g_d3d.rt.color_final,"vr-selftest-mirror.png");
    SaveEye(eyeTargets[1][RenderTarget_color_final],"vr-selftest-right-before-aim.png");
    MirrorLeftEye();
    OpenVRPrototype::BeginReticle(0,-5,50);
    RenderBackend::RasterTriangles(&quad,1);
    OpenVRPrototype::EndReticle();
    SaveEye(aimTexture,"vr-selftest-aim.png");
    SaveEye(eyeTargets[0][RenderTarget_color_final],"vr-selftest-left-with-aim.png");
    SaveEye(eyeTargets[1][RenderTarget_color_final],"vr-selftest-right-with-aim.png");
    SaveEye(g_d3d.rt.color_final,"vr-selftest-mirror-with-aim.png");
    Log(uiHash==SaveEye(uiTexture,"vr-selftest-ui-after-aim.png") ?
        "PASS: reticle capture is separate from the menu panel." : "FAIL: reticle capture changed the menu panel.");
    Log("GPU UI test captured: transparent panel, half-alpha quad, and desktop mirror.");
    // Consecutive frames with head motion: a green sight must disappear from
    // its old pixels rather than accumulate in the final eye target.
    for(auto& vertex:vertices) {
        vertex.pos.x*=0.14f; vertex.pos.y*=0.14f;
        vertex.color={0,1,0,1};
    }
    auto TrailFrame=[&](bool turned) {
        head=turned ? VRMath::Pose{{{0.9659258f,0,0.2588190f,0},{0,1,0,0},{-0.2588190f,0,0.9659258f,0}}} : VRMath::Identity();
        RenderBackend::BeginScene(&scene);
        RenderBackend::RaytraceBillboardColored(RT_MATERIAL_EMISSIVE_WHITE,{1.0f,0.4f,0.1f},{1.0f,1.0f},{-0.9f,0.2f,0.0f},{-0.9f,0.2f,0.0f});
        OpenVRPrototype::RenderScene();
        if(turned) SaveEye(eyeTargets[0][RenderTarget_color_final],"vr-selftest-trail-b-clean.png");
        OpenVRPrototype::BeginReticle(0,-5,50);
        RenderBackend::RasterTriangles(&quad,1);
        OpenVRPrototype::EndReticle();
        SaveEye(eyeTargets[0][RenderTarget_color_final],turned ? "vr-selftest-trail-b.png" : "vr-selftest-trail-a.png");
    };
    TrailFrame(false); TrailFrame(true);
    TestSpatialAA();
    TestHybridVisibilityCases(camera);
    // Exercise both swap slots, grow/shrink scene inputs, then measure a
    // warmed-up fixed workload. No resolution/quality reductions are used.
    if(HasArgument(L"-openvr-native-benchmark")) {
        eyeWidth=1869; eyeHeight=1920; EnsureTargets();
    }
    const auto savedSlot=g_d3d.current_back_buffer_index;
    const auto savedPrevSlot=g_d3d.prev_back_buffer_index;
    const auto savedFrameIndex=g_d3d.frame_index;
    const bool lightBenchmark=HasArgument(L"-openvr-light-benchmark");
    std::vector<RT_Light> benchmarkLights;
    if(lightBenchmark) {
        RT_Material surface={}; surface.albedo_texture=g_d3d.white_texture_handle; surface.roughness=0.7f;
        RenderBackend::UpdateMaterial(RT_MATERIAL_FLAT_WHITE,&surface);
        g_rt_material_indices[RT_MATERIAL_FLAT_WHITE]=RT_MATERIAL_FLAT_WHITE;
        for(UINT i=0;i<64;++i) {
            const RT_Vec3 position={-4.0f+static_cast<float>(i%8)*1.15f,-4.0f+static_cast<float>(i/8)*1.15f,-1.5f};
            const RT_Vec3 emission={0.0015f+0.0003f*static_cast<float>(i%3),0.0018f,0.0021f};
            benchmarkLights.push_back(RT_MakeSphericalLight(emission,position,0.05f));
        }
    }
    std::vector<double> cpuSamples;
    double batchStart=0;
    benchmarking=true;
    // Match the real frame lifecycle: otherwise the hot-reload timestamp is
    // never committed and this offline loop measures shader recompilation.
    RenderBackend::EndFrame();
    for(UINT iteration=0;iteration<32;++iteration) {
        g_d3d.current_back_buffer_index=iteration%BACK_BUFFER_COUNT_SWAP;
        g_d3d.prev_back_buffer_index=(iteration+1)%BACK_BUFFER_COUNT_SWAP;
        g_d3d.frame_index=iteration;
        if(iteration==8) { g_d3d.command_queue_direct->Flush(); batchStart=ClockMS(); }
        RenderBackend::BeginScene(&scene);
        g_d3d.scene.prev_camera=camera;
        if(lightBenchmark) {
            RenderBackend::RaytraceSubmitLights(benchmarkLights.size(),benchmarkLights.data());
            RenderBackend::RaytraceBillboardColored(RT_MATERIAL_FLAT_WHITE,{0.7f,0.7f,0.7f},{6,6},{0,0,2},{0,0,2});
        }
        const UINT instances=iteration<6 ? 1u<<iteration : 32u;
        for(UINT i=0;i<instances;++i) {
            const RT_Vec3 p={-1.5f+static_cast<float>(i%8)*0.4f,-0.5f+static_cast<float>(i/8)*0.4f,1};
            RenderBackend::RaytraceBillboardColored(lightBenchmark ? RT_MATERIAL_FLAT_WHITE : RT_MATERIAL_EMISSIVE_WHITE,{1,0.4f,0.1f},{0.25f,0.25f},p,p);
        }
        for(int panel=0;panel<2;++panel) {
            const RT_Vec3 pos={panel ? 1.3f : -1.3f,-0.6f,-0.5f};
            RenderBackend::RaytraceBillboardColored(lightBenchmark ? RT_MATERIAL_FLAT_WHITE : RT_MATERIAL_EMISSIVE_WHITE,{0.3f,0.4f,0.5f},{0.6f,1.2f},pos,pos);
            const auto at=g_d3d.tlas_instance_count-1;
            CurrentFrameData()->instance_descs.As<D3D12_RAYTRACING_INSTANCE_DESC>()[at].InstanceMask=RT_RENDER_MASK_COCKPIT;
            CurrentFrameData()->instance_data.As<InstanceData>()[at].instance_mask=RT_RENDER_MASK_COCKPIT;
        }
        const double cpuStart=ClockMS();
        OpenVRPrototype::RenderScene();
        if(iteration>=8) cpuSamples.push_back(ClockMS()-cpuStart);
        RenderBackend::EndFrame();
    }
    g_d3d.command_queue_direct->Flush();
    const double batchMS=(ClockMS()-batchStart)/24.0;
    benchmarking=false;
    double cpuMean=0; for(const auto value:cpuSamples) cpuMean+=value/static_cast<double>(cpuSamples.size());
    std::sort(cpuSamples.begin(),cpuSamples.end());
    char benchmark[300]; std::snprintf(benchmark,sizeof(benchmark),"Stereo benchmark: serial %d, %ux%u, 32 world + 2 cockpit instances, 24 measured frames; CPU mean %.3f ms p95 %.3f ms; batch %.3f ms/frame; last GPU L %.3f R %.3f ms.",serialStereo ? 1 : 0,eyeWidth,eyeHeight,cpuMean,cpuSamples[cpuSamples.size()*95/100],batchMS,lastEyeGPU[0],lastEyeGPU[1]); Log(benchmark);
    SaveEye(eyeTargets[0][RenderTarget_color_final],"vr-selftest-stress-left.png");
    SaveEye(eyeTargets[1][RenderTarget_color_final],"vr-selftest-stress-right.png");
    Log("PASS: alternating swap slots and growing scene inputs completed.");
    if(lightBenchmark) {
        char lightBenchmarkLog[300]; std::snprintf(lightBenchmarkLog,sizeof(lightBenchmarkLog),"Lit benchmark: %ux%u per eye; 64 lights; candidate budget %u; batch %.3f ms/frame; direct lighting GPU L %.3f R %.3f ms.",eyeWidth,eyeHeight,lightCandidateBudget,batchMS,lastStageGPU[0][1],lastStageGPU[1][1]); Log(lightBenchmarkLog);
        SaveEye(eyeTargets[0][RenderTarget_color_final],"vr-selftest-light-benchmark.png");
        OpenVRPrototype::RenderScene();
        SaveEye(eyeTargets[0][RenderTarget_color_final],"vr-selftest-light-next.png");
    }

    // A mipmapped fine checker must retain identical detail when marked as
    // foreground cockpit geometry: VR uses the same projection for both.
    std::vector<uint32_t> checker(512*512);
    for(UINT y=0;y<512;++y) for(UINT x=0;x<512;++x)
        checker[y*512+x]=((x/4+y/4)&1) ? 0xFFFFFFFFu : 0xFF101010u;
    RT_UploadTextureParams upload={};
    upload.image.width=512; upload.image.height=512;
    upload.image.format=RT_TextureFormat_RGBA8; upload.image.pixels=checker.data();
    upload.name="VR cockpit detail regression";
    const auto detailTexture=RenderBackend::UploadTexture(upload);
    RT_Material detail={}; detail.albedo_texture=detailTexture;
    detail.roughness=1;
    detail.emissive_color={1,1,1}; detail.emissive_strength=1;
    RenderBackend::UpdateMaterial(RT_MATERIAL_FLAT_WHITE,&detail);
    g_rt_material_indices[RT_MATERIAL_FLAT_WHITE]=RT_MATERIAL_FLAT_WHITE;
    const float savedDebugBlend=tweak_vars.debug_render_blend_factor;
    testingDetail=true;
    tweak_vars.debug_render_blend_factor=1;
    for(int cockpit=0;cockpit<2;++cockpit) {
        RenderBackend::BeginScene(&scene); g_d3d.scene.prev_camera=camera;
        RenderBackend::RaytraceBillboardColored(RT_MATERIAL_FLAT_WHITE,{1,1,1},{2,2},{0,0,0},{0,0,0});
        if(cockpit) {
            auto* input=CurrentFrameData();
            input->instance_descs.As<D3D12_RAYTRACING_INSTANCE_DESC>()[0].InstanceMask=RT_RENDER_MASK_COCKPIT;
            input->instance_data.As<InstanceData>()[0].instance_mask=RT_RENDER_MASK_COCKPIT;
        }
        OpenVRPrototype::RenderScene();
        SaveEye(eyeTargets[0][RenderTarget_color_final],cockpit ? "vr-selftest-detail-cockpit.png" : "vr-selftest-detail-world.png");
        RenderBackend::EndFrame();
    }
    Log("Captured cockpit/world fine texture comparison; check-vr-images.py allows final-frame quantization/dither.");
    testingDetail=false;
    tweak_vars.debug_render_blend_factor=savedDebugBlend;
    RenderBackend::ReleaseTexture(detailTexture);

    g_d3d.current_back_buffer_index=savedSlot; g_d3d.prev_back_buffer_index=savedPrevSlot; g_d3d.frame_index=savedFrameIndex;
    head=VRMath::Identity();
    const auto savedMirror=graphicsRunning.mirror;
    graphicsRunning.mirror=1;MirrorLeftEye();SaveEye(g_d3d.rt.color_final,"vr-selftest-mirror-fit.png");
    graphicsRunning.mirror=2;MirrorLeftEye();SaveEye(g_d3d.rt.color_final,"vr-selftest-mirror-off.png");
    graphicsRunning.mirror=savedMirror;
    ReleaseTargets(); offlineTest=false; active=false; stereoThisFrame=false;
    g_d3d_raster.render_target=g_d3d.rt.color_final;
    g_d3d_raster.rtv_handle=g_d3d.color_final_rtv.cpu;
}
}

#include "VRGraphicsSettings.inl"
void OpenVRPrototype::Init() {
    const bool selfTest=HasArgument(L"-openvr-selftest");
    levelTest=HasArgument(L"-openvr-leveltest");
    const bool vrRequested=HasArgument(L"-openvr");
    SetWindowTextA(g_d3d.hWnd,vrRequested ? "DMAX 11 VR" : "DMAX 11");
    if(!vrRequested && !selfTest) {
        Log("DMAX 11: desktop mode; add -OpenVR to enable optimized VR.");
        return;
    }
    // One public mode switch selects the complete current VR prototype.
    // The reference visibility flag is retained only for diagnostic A/B runs.

    Log("DMAX 11 VR optimization: exact blue-noise atlas; conservative opaque-alpha fast path.");
    fastVisualReference=HasArgument(L"-openvr-fast-visual-reference");
    indirectLighting=!fastVisualReference;
    // The public -OpenVR entry point uses the validated smooth lighting budget.
    lightCandidateBudget=8u;
    Log(fastVisualReference ? "VR diagnostic fast visual profile selected." :
        "VR restored visual profile: primary rays and indirect lighting/reflections enabled; original light-clamping limits.");
    char lightSetting[32]={};
    if(GetEnvironmentVariableA("D2_VR_LIGHT_CANDIDATES",lightSetting,sizeof(lightSetting))>0) {
        const int value=std::atoi(lightSetting);
        if(value>=0 && value<=16) lightCandidateBudget=static_cast<uint32_t>(value);
    }
    char lightLog[160]; std::snprintf(lightLog,sizeof(lightLog),"VR lighting candidate budget %u (0=full); small light sets retain full selection.",lightCandidateBudget); Log(lightLog);
    hybridVisibility=(fastVisualReference || HasArgument(L"-openvr-hybrid-visibility")) && !HasArgument(L"-openvr-ray-visibility");
    if(hybridVisibility) {
        Log("DMAX 11 VR: experimental raster visibility with ray-traced lighting.");
    }
    char spatialSetting[16]={};
    fastSpatial=!(GetEnvironmentVariableA("D2_VR_REFERENCE_GPU",spatialSetting,sizeof(spatialSetting))>0 && std::atoi(spatialSetting)!=0);
    char serialSetting[16]={};
    serialStereo=GetEnvironmentVariableA("D2_VR_SERIAL_STEREO",serialSetting,sizeof(serialSetting))>0 && std::atoi(serialSetting)!=0;
    if(selfTest) {
        reticleScale=1.0f; // Preserve the established synthetic alignment fixture scale.
        if(HasArgument(L"-openvr-noise-atlas-test")) {
            FlushRingBuffer(&g_d3d.resource_upload_ring_buffer);
            SaveEye(g_d3d.blue_noise_atlas,"vr-selftest-noise-atlas.png");
            for(unsigned n=0;n<BLUE_NOISE_TEX_COUNT;++n) {
                char name[80];std::snprintf(name,sizeof(name),"vr-selftest-noise-source-%u.png",n);
                SaveEye(g_d3d.blue_noise_textures[n],name);
            }
        }
        RunOfflineTest(); return;
    }
    if(levelTest) {
        offlineTest=active=haveOrigin=true;
        graphicsRunning.mode=4; // Fixed Balanced policy for the offline level fixture.
        const bool quest2High=HasArgument(L"-openvr-quest2-high-profile");
        eyeWidth=quest2High ? 2496 : (HasArgument(L"-openvr-quest2-profile") ? 2112 : 2232);
        eyeHeight=quest2High ? 2688 : (HasArgument(L"-openvr-quest2-profile") ? 2304 : 2292);
        g_d3d.upscaling_aa_mode=UPSCALING_AA_MODE_OFF;
        tweak_vars.upscaling_aa_mode=UPSCALING_AA_MODE_OFF;
        EnsureTargets();EnsureUI();
        Log("D2 real-level regression: fixed tracking, headset-profile stereo output, Balanced; no headset submission.");
        return;
    }
    Log("OpenVR prototype requested (Valve OpenVR SDK 2.12.14).");
    Log(fastSpatial ? "VR specialized spatial denoiser enabled; five full-resolution passes." : "VR reference spatial denoiser selected.");
    if(!vr::VR_IsRuntimeInstalled() || !vr::VR_IsHmdPresent()) {
        Log("SteamVR runtime/headset not available. Continuing in desktop mode; connect the headset and restart with -openvr.");
        return;
    }
    vr::EVRInitError error=vr::VRInitError_None;
    systemVR=vr::VR_Init(&error,vr::VRApplication_Scene);
    if(error!=vr::VRInitError_None || !systemVR || !vr::VRCompositor()) {
        Log(vr::VR_GetVRInitErrorAsEnglishDescription(error));
        if(systemVR) vr::VR_Shutdown();
        systemVR=nullptr;
        return;
    }
    // DX12 textures must originate on the GPU used by SteamVR.
    int32_t adapterIndex=-1;
    systemVR->GetDXGIOutputInfo(&adapterIndex);
    if(adapterIndex>=0) {
        ComPtr<IDXGIFactory1> factory; ComPtr<IDXGIAdapter1> adapter;
        if(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
           SUCCEEDED(factory->EnumAdapters1(static_cast<UINT>(adapterIndex),&adapter))) {
            DXGI_ADAPTER_DESC1 desc={}; adapter->GetDesc1(&desc);
            const LUID gpu=g_d3d.device->GetAdapterLuid();
            if(gpu.LowPart!=desc.AdapterLuid.LowPart || gpu.HighPart!=desc.AdapterLuid.HighPart) {
                Log("SteamVR and the game are using different GPUs. Select the same GPU before enabling VR.");
                vr::VR_Shutdown(); systemVR=nullptr; return;
            }
        }
    }
    vr::VRCompositor()->SetTrackingSpace(vr::TrackingUniverseSeated);
    {
        char value[32];
        if(GetEnvironmentVariableA("D2_VR_HUD_DISTANCE",value,sizeof(value))>0) {
            const float d=static_cast<float>(std::atof(value));
            if(std::isfinite(d) && d>=0.5f && d<=3.0f) hudDistance=d;
        }
        if(GetEnvironmentVariableA("D2_VR_TEXTURE_BIAS",value,sizeof(value))>0) {
            const float b=static_cast<float>(std::atof(value));
            if(std::isfinite(b) && b>=-2.0f && b<=1.0f) textureBias=b;
        }
        if(GetEnvironmentVariableA("D2_VR_FSR_SHARPNESS",value,sizeof(value))>0) {
            const float s=static_cast<float>(std::atof(value));
            if(std::isfinite(s) && s>=0.0f && s<=1.0f) fsrSharpness=s;
        }
        char textureLog[160];
        if(fsrSharpness>=0) std::snprintf(textureLog,sizeof(textureLog),"VR textures: anisotropic 16x; FSR extra mip bias %.2f; FSR sharpness %.2f.",textureBias,fsrSharpness);
        else std::snprintf(textureLog,sizeof(textureLog),"VR textures: anisotropic 16x; FSR extra mip bias %.2f; FSR sharpness per preset (0.45/0.55/0.65).",textureBias);
        Log(textureLog);
    }
    if(vr::VROverlay()) {
        char key[100]; std::snprintf(key,sizeof(key),"d2.raytracer.ui.%lu",GetCurrentProcessId());
        if(vr::VROverlay()->CreateOverlay(key,"Descent 2 cockpit UI",&uiOverlay)==vr::VROverlayError_None) {
            // Keep the same apparent size at any distance (1.4 m wide at 1.6 m).
            vr::VROverlay()->SetOverlayWidthInMeters(uiOverlay,1.4f*hudDistance/1.6f);
            vr::VROverlay()->SetOverlayFlag(uiOverlay,vr::VROverlayFlags_IsPremultiplied,true);
            char panelLog[120]; std::snprintf(panelLog,sizeof(panelLog),"UI panel created: %.2fm wide, %.2fm ahead of the recentered seat (double-buffered).",1.4f*hudDistance/1.6f,hudDistance); Log(panelLog);

        }
    }
    if(uiOverlay==vr::k_ulOverlayHandleInvalid) {
        Log("Could not create the SteamVR UI panel; returning to desktop rendering.");
        if(uiOverlay!=vr::k_ulOverlayHandleInvalid) vr::VROverlay()->DestroyOverlay(uiOverlay);
        uiOverlay=vr::k_ulOverlayHandleInvalid;
        vr::VR_Shutdown(); systemVR=nullptr; return;
    }
    uint32_t recommendedWidth=0,recommendedHeight=0;
    uint32_t maxEyeEdge=1664;
    char setting[64];
    if(GetEnvironmentVariableA("D2_VR_MAX_EYE_EDGE",setting,sizeof(setting))>0) {
        const int value=std::atoi(setting);
        if(value>=768 && value<=2048) maxEyeEdge=static_cast<uint32_t>(value);
    }
    if(GetEnvironmentVariableA("D2_VR_OBJECT_SCALE",setting,sizeof(setting))>0) {
        const float value=static_cast<float>(std::atof(setting));
        if(std::isfinite(value) && value>=0.5f && value<=1.5f) objectScale=value;
    }
    if(GetEnvironmentVariableA("D2_VR_RETICLE_SCALE",setting,sizeof(setting))>0) {
        const float value=static_cast<float>(std::atof(setting));
        if(std::isfinite(value) && value>=0.5f && value<=3.0f) reticleScale=value;
    }
    char reticleLog[100]; std::snprintf(reticleLog,sizeof(reticleLog),"VR reticle size scale %.2f; ship-local aim unchanged.",reticleScale); Log(reticleLog);
    if(GetEnvironmentVariableA("D2_VR_INDIRECT",setting,sizeof(setting))>0)
        indirectLighting=std::atoi(setting)!=0;
    systemVR->GetRecommendedRenderTargetSize(&recommendedWidth,&recommendedHeight);
    ConfigureGraphics(recommendedWidth,recommendedHeight,maxEyeEdge);
    char resolution[200]; std::snprintf(resolution,sizeof(resolution),"VR eye resolution %ux%u (SteamVR recommendation %ux%u). FSR per-eye history; short motion-reprojected lighting history (diffuse 4, specular 2).",eyeWidth,eyeHeight,recommendedWidth,recommendedHeight); Log(resolution);
    char scale[64];
    if(GetEnvironmentVariableA("D2_VR_UNITS_PER_METRE",scale,sizeof(scale))>0) {
        const float value=static_cast<float>(std::atof(scale));
        if(std::isfinite(value) && value>=0.1f && value<=10.0f) unitsPerMetre=value;
    }
    active=true;
    const float frequency=systemVR->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,vr::Prop_DisplayFrequency_Float);
    char settingsLog[240]; std::snprintf(settingsLog,sizeof(settingsLog),"VR display %.1f Hz; motion smoothing enabled %d; object visual scale %.2f; indirect lighting %d; spatial AA enabled; explicit render poses enabled.",frequency,vr::VRCompositor()->IsMotionSmoothingEnabled() ? 1 : 0,objectScale,indirectLighting ? 1 : 0); Log(settingsLog);
    // Start with the spatial baseline; the eye pass enables FSR only when selected.
    tweak_vars.upscaling_aa_mode=UPSCALING_AA_MODE_OFF;
    g_d3d.io.scene_transition=true;
    Log("OpenVR active. Recenter: Ctrl+Alt+Home. Existing controls still steer and aim the ship.");
}
bool OpenVRPrototype::Active() { return active; }
bool OpenVRPrototype::FixedMirrorCanvas() { static const bool requested=HasArgument(L"-OpenVR");return requested; }
float OpenVRPrototype::ObjectScale() { return active ? objectScale : 1.0f; }
bool OpenVRPrototype::CapturingReticle() { return aimCapturing; }
void OpenVRPrototype::BeginReticle(float x,float y,float z) {
    if(!active) return;
    RenderBackend::RasterRender();
    EnsureUI();
    aimX=x; aimY=y; aimZ=z; haveAim=true; aimCapturing=true;
    auto& commands=g_d3d.command_queue_direct->GetCommandList();
    ResourceTransition(commands,aimTexture,D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float clear[4]={0,0,0,0}; commands->ClearRenderTargetView(aimView.cpu,clear,0,nullptr);
    g_d3d.command_queue_direct->ExecuteCommandList(commands);
    g_d3d_raster.render_target=aimTexture; g_d3d_raster.rtv_handle=aimView.cpu;
}
void OpenVRPrototype::EndReticle() {
    if(!active || !aimCapturing) return;
    const size_t count=g_d3d_raster.tri_count*3;
    const auto* source=g_d3d_raster.tri_vertex_buf_ptr+g_d3d_raster.tri_at*3;
    const std::vector<RT_RasterTriVertex> vertices(source,source+count);
    const auto savedViewport=g_d3d_raster.viewport;
    RenderBackend::RasterRender();
    aimCapturing=false;
    if(stereoThisFrame && count) {
        // Use exactly the world's eye cameras/projection. A SteamVR overlay
        // must not independently interpret ship coordinates or recentering.
        const auto localTarget=Transform(reticleBase,aimX,aimY,aimZ);
        RT_Vec3 target={reticleBase.position.x+localTarget.x,reticleBase.position.y+localTarget.y,reticleBase.position.z+localTarget.z};
        const float distance=std::sqrt(aimX*aimX+aimY*aimY+aimZ*aimZ);
        const float width=2.0f*distance*std::tan(2.0f*3.14159265f/180.0f)*reticleScale/0.07f;
        const float height=width*static_cast<float>(uiHeight)/static_cast<float>(uiWidth);
        auto Dot=[](RT_Vec3 a,RT_Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; };
        for(int eye=0;eye<2;++eye) {
            std::vector<RT_RasterTriVertex> projected=vertices;
            const auto& camera=reticleEyes[eye];
            const auto* tangents=reticleTangents[eye];
            bool visible=true;
            for(auto& vertex:projected) {
                const auto offset=Transform(reticleBase,vertex.pos.x*width*0.5f,vertex.pos.y*height*0.5f,0);
                const RT_Vec3 point={target.x+offset.x-camera.position.x,target.y+offset.y-camera.position.y,target.z+offset.z-camera.position.z};
                visible &= VRMath::ProjectEye(Dot(point,camera.right),Dot(point,camera.up),Dot(point,camera.forward),
                    tangents[0],tangents[1],tangents[2],tangents[3],vertex.pos.x,vertex.pos.y);
                vertex.pos.z=0;
            }
            if(!visible) continue;
            auto Draw=[&](ID3D12Resource* resource,D3D12_CPU_DESCRIPTOR_HANDLE view,uint32_t w,uint32_t h) {
                RT_ASSERT(g_d3d_raster.tri_at*3+count<=BACK_BUFFER_COUNT*MAX_RASTER_TRIANGLES*3);
                std::memcpy(g_d3d_raster.tri_vertex_buf_ptr+g_d3d_raster.tri_at*3,projected.data(),count*sizeof(RT_RasterTriVertex));
                g_d3d_raster.tri_count=count/3;
                g_d3d_raster.render_target=resource; g_d3d_raster.rtv_handle=view;
                RenderBackend::RasterSetViewport(0,0,static_cast<float>(w),static_cast<float>(h));
                RenderBackend::RasterRender();
            };
            Draw(eyeTargets[eye][RenderTarget_color_final],eyeViews[eye].cpu,eyeWidth,eyeHeight);

        }
    }
    // Live presentation makes one final spectator copy after reticle drawing.
    // Offline fixtures retain immediate copies for their capture assertions.
    if(offlineTest && stereoThisFrame && count) MirrorLeftEye();
    g_d3d_raster.viewport=savedViewport;
    g_d3d_raster.render_target=uiTexture; g_d3d_raster.rtv_handle=uiView.cpu;
}
void OpenVRPrototype::BeginFrame() {
    if(!active || offlineTest) return;
    haveAim=false; aimCapturing=false;
    Tracking();
    if(!active) return;
    ApplyPendingGraphics();
    EnsureUI();
    uiIndex^=1u; uiTexture=uiTextures[uiIndex]; uiView=uiViews[uiIndex];
    auto& commands=g_d3d.command_queue_direct->GetCommandList();
    ResourceTransition(commands,uiTexture,D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float clear[4]={0,0,0,0};
    commands->ClearRenderTargetView(uiView.cpu,clear,0,nullptr);
    g_d3d.command_queue_direct->ExecuteCommandList(commands);
    g_d3d_raster.render_target=uiTexture;
    g_d3d_raster.rtv_handle=uiView.cpu;
}
bool OpenVRPrototype::UIRenderTarget(ID3D12Resource** resource,D3D12_CPU_DESCRIPTOR_HANDLE* view) {
    if(!active) return false;
    EnsureUI(); *resource=aimCapturing ? aimTexture : uiTexture; *view=aimCapturing ? aimView.cpu : uiView.cpu; return true;
}
bool OpenVRPrototype::EyePass() { return inEyePass; }
bool OpenVRPrototype::FirstEye() { return inEyePass && currentEye==0; }
bool OpenVRPrototype::QueueEyeUploads() { return inEyePass && !serialStereo; }
bool OpenVRPrototype::HybridVisibility() { return hybridVisibility; }
bool OpenVRPrototype::FastSpatial() { return fastSpatial; }
bool OpenVRPrototype::ReuseScene() { return inEyePass && currentEye==1 && !serialStereo; }
float OpenVRPrototype::FsrSharpness() {
    if(fsrSharpness>=0) return fsrSharpness;
    // Same RCAS strengths as the desktop presets (Quality/Balanced/Performance).
    return graphicsRunning.mode==3 ? 0.45f : (graphicsRunning.mode==5 ? 0.65f : 0.55f);
}
bool OpenVRPrototype::CompositorPaced() { return active && (offlineTest || compositorPacing); }
unsigned long long OpenVRPrototype::EyeHistoryIndex() { return eyeFrames[currentEye]; }
ID3D12Resource* OpenVRPrototype::SharedTLAS() {
    return eyeFrameData[g_d3d.current_back_buffer_index][0].top_level_as;
}
void OpenVRPrototype::BeginEyeGpuTiming(ID3D12GraphicsCommandList* commands) {
    if(inEyePass && eyeQueryHeap && !serialStereo)
        commands->EndQuery(eyeQueryHeap,D3D12_QUERY_TYPE_TIMESTAMP,g_d3d.current_back_buffer_index*4+static_cast<UINT>(currentEye)*2);
}
void OpenVRPrototype::EndEyeGpuTiming(ID3D12GraphicsCommandList* commands) {
    if(inEyePass && eyeQueryHeap && !serialStereo) {
        const UINT index=g_d3d.current_back_buffer_index*4+static_cast<UINT>(currentEye)*2;
        commands->EndQuery(eyeQueryHeap,D3D12_QUERY_TYPE_TIMESTAMP,index+1);
        const UINT offset=(g_d3d.current_back_buffer_index*2+static_cast<UINT>(currentEye))*timingStride;
        commands->ResolveQueryData(eyeQueryHeap,D3D12_QUERY_TYPE_TIMESTAMP,index,2,eyeQueryReadback,offset*sizeof(uint64_t));
        for(UINT stage=0;stage<stageCount;++stage) {
            const UINT query=g_d3d.current_back_buffer_index*GPUProfiler::GPUTimer_NumTimers*2+stageTimers[stage]*2;
            commands->ResolveQueryData(g_d3d.query_heap,D3D12_QUERY_TYPE_TIMESTAMP,query,2,eyeQueryReadback,(offset+2+stage*2)*sizeof(uint64_t));
        }
    }
}
void OpenVRPrototype::ProjectionTangents(float* left,float* right,float* top,float* bottom) {
    if(offlineTest) { *left=-1; *right=1; *top=-1; *bottom=1; return; }
    systemVR->GetProjectionRaw(static_cast<vr::EVREye>(currentEye),left,right,top,bottom);
}
bool OpenVRPrototype::RenderScene() {
    if(!active || g_d3d.scene.render_blit || g_d3d.scene.external) return false;
    if(!Tracking()) return false;
    // Head-only fixtures leave the ship and its headlights pointing forward.
    if(levelTest) {
        static unsigned headTestFrame;
        const bool turn=HasArgument(L"-openvr-head-turntest");
        if(turn || HasArgument(L"-openvr-head-right-test")) {
            const float yaw=turn ? 1.3962634f*std::sin(headTestFrame++*0.05235988f) : 1.5707963f;
            const float c=std::cos(yaw),s=std::sin(yaw);
            head=VRMath::Pose{{{c,0,s,0},{0,1,0,0},{-s,0,c,0}}};
        }
    }
    const double renderStarted=ClockMS();
    const auto savedDeltaTime=g_d3d.io.delta_time;
    // Offline comparisons must not feed wall-clock compilation/loading delays
    // into FSR's exposure history. Live VR retains the real frame delta.
    if(levelTest) g_d3d.io.delta_time=1.0f/72.0f;
    EnsureTargets();
    if(serialStereo) g_d3d.command_queue_direct->Flush();
    // Diagnostic only: exercise the same spherical lights used by weapons
    // and explosions, without requiring interactive combat or a headset.
    static const bool lightStress=HasArgument(L"-openvr-light-stress");
    if(levelTest && lightStress) {
        RT_Light lights[64];
        const auto& camera=g_d3d.scene.camera;
        const RT_Vec3 right=RT_Vec3Cross(camera.up,camera.forward);
        for(unsigned i=0;i<64;++i) {
            RT_Vec3 position=RT_Vec3MulsAdd(camera.position,camera.forward,2.0f+0.15f*(i/8));
            position=RT_Vec3MulsAdd(position,right,(static_cast<float>(i%8)-3.5f)*0.25f);
            position=RT_Vec3MulsAdd(position,camera.up,(static_cast<float>(i/8)-3.5f)*0.15f);
            lights[i]=RT_MakeSphericalLight({0.0015f,0.0018f,0.0021f},position,0.05f);
        }
        RenderBackend::RaytraceSubmitLights(64,lights);
    }
    FrameData* sourceFrame=CurrentFrameData();
    const auto savedScene=g_d3d.scene;
    renderedHead=head;
    reticleBase=savedScene.camera;
    const auto savedAccum=g_d3d.accum_frame_index;
    const auto savedAA=g_d3d.upscaling_aa_mode;
    const auto savedTweaks=tweak_vars;
    const uint32_t savedOutputWidth=g_d3d.output_width,savedOutputHeight=g_d3d.output_height;
    const uint32_t savedRenderWidth=g_d3d.render_width,savedRenderHeight=g_d3d.render_height;
    const uint32_t savedOverrideWidth=g_d3d.render_width_override,savedOverrideHeight=g_d3d.render_height_override;
    g_d3d.render_width_override=g_d3d.render_height_override=0;
    ID3D12Resource* desktopTargets[RenderTarget_COUNT];
    std::memcpy(desktopTargets,g_d3d.render_targets,sizeof(desktopTargets));
    const bool transition=g_d3d.io.scene_transition || resetHistory;
    if(transition) eyeFrames[0]=eyeFrames[1]=0;
    resetHistory=false;
    for(int eye=0;eye<2;++eye) {
        currentEye=eye; inEyePass=true;
        if(!serialStereo) g_vr_frame_override=PrepareEyeFrame(*sourceFrame,eye);
        if(eye==1) lastRightFrame=CurrentFrameData();
        std::memcpy(g_d3d.render_targets,eyeTargets[eye],sizeof(desktopTargets));
        g_d3d.scene=savedScene;
        g_d3d.scene.freezeframe=false;
        g_d3d.scene.camera=EyeCamera(savedScene.camera,eye);
        reticleEyes[eye]=g_d3d.scene.camera;
        ProjectionTangents(&reticleTangents[eye][0],&reticleTangents[eye][1],&reticleTangents[eye][2],&reticleTangents[eye][3]);
        const bool fsr=VRGraphics::Upscaling(graphicsRunning);
        const auto render=VRGraphics::RenderSize(graphicsRunning,eyeWidth,eyeHeight);
        g_d3d.output_width=eyeWidth;g_d3d.output_height=eyeHeight;
        g_d3d.render_width=render.width;g_d3d.render_height=render.height;
        g_d3d.upscaling_aa_mode=fsr ? UPSCALING_AA_MODE_AMD_FSR_2_2 : UPSCALING_AA_MODE_OFF;
        g_d3d.accum_frame_index=eyeFrames[eye];
        g_d3d.io.scene_transition=transition;
        if(offlineTest) Log(eye==0 ? "GPU test: preparing left eye." : "GPU test: preparing right eye.");
        RenderBackend::EndScene();
        g_d3d.io.scene_transition=transition || eyeFrames[eye]==0;
        auto* cb=CurrentFrameData()->scene_cb.As<GlobalConstantBuffer>();
        const auto& previous=eyeFrames[eye]>0 && !transition ? previousEye[eye] : *cb;
        cb->prev_view=previous.view; cb->prev_view_inv=previous.view_inv;
        cb->prev_proj=previous.proj; cb->prev_proj_inv=previous.proj_inv;
        if(testingDetail) cb->debug_render_mode=RT_DebugRenderMode_Albedo;
        cb->viewport_offset_y=0.0f;
        cb->taa_jitter=fsr ? g_d3d.halton_samples[eyeFrames[eye] % HALTON_SAMPLE_COUNT] : RT_Vec2{0.0f,0.0f};
        cb->debug_flags|=0x80000000u;
        static const bool stockFillReference=HasArgument(L"-openvr-stock-fill-reference");
        if(stockFillReference) cb->debug_flags|=GlobalDebugFlags_DisableVRStockFill; // Match cockpit and world projections in stereo.
        if(fastSpatial) cb->debug_flags|=0x40000000u; // Resolve foreground cockpit before hidden world rays.
        previousEye[eye]=*cb;
        auto* vars=CurrentFrameData()->tweak_vars.As<TweakVars>();
        tweak_vars.upscaling_aa_mode=g_d3d.upscaling_aa_mode;
        tweak_vars.taa_per_pixel_jitter=false;
        tweak_vars.motion_blur_quality=0;
        // Restore bounced lighting and reflections; the fast profile is now
        // an explicit diagnostic comparison, not the normal VR path.
        tweak_vars.enable_pathtracing=indirectLighting;
        tweak_vars.ris=(lightCandidateBudget && g_d3d.lights_count>lightCandidateBudget) ? 1 : 2;
        if(lightCandidateBudget) tweak_vars.ris_spp=lightCandidateBudget;
        tweak_vars.albedo_sample_linear=1;
        tweak_vars.normal_sample_linear=1;
        tweak_vars.metallic_roughness_sample_linear=1;
        // FSR reconstructs output-resolution detail, so sample textures for the
        // output size plus AMD's recommended extra bias (textureBias, default -1).
        tweak_vars.mip_bias_u=fsr ? static_cast<float>(std::log2(static_cast<double>(render.width)/eyeWidth))+textureBias : 0;
        tweak_vars.mip_bias_v=fsr ? static_cast<float>(std::log2(static_cast<double>(render.height)/eyeHeight))+textureBias : 0;
        tweak_vars.sharpen_amount=0;
        tweak_vars.svgf_stabilize=false;
        tweak_vars.svgf_enabled=true;
        tweak_vars.svgf_firefly_diffuse=fastVisualReference ? std::min(savedTweaks.svgf_firefly_diffuse,8.0f) : savedTweaks.svgf_firefly_diffuse;
        tweak_vars.svgf_firefly_specular=fastVisualReference ? std::min(savedTweaks.svgf_firefly_specular,4.0f) : savedTweaks.svgf_firefly_specular;
        tweak_vars.svgf_max_hist_len_diff=1;
        tweak_vars.svgf_max_hist_len_spec=1;
        if(!HasArgument(L"-openvr-spatial-reference")) {
            tweak_vars.svgf_max_hist_len_diff=4;
            tweak_vars.svgf_max_hist_len_spec=2;
        }
        tweak_vars.svgf_history_fix_frames=0;
        tweak_vars.svgf_jitter=0;
        *vars=tweak_vars;
        if(offlineTest) Log("GPU test: dispatching raytracer.");
        RenderBackend::RaytraceRender();
        if(serialStereo) g_d3d.command_queue_direct->Flush();
        else CurrentFrameData()->fence_value=g_d3d.command_queue_direct->Signal();
        if(offlineTest) Log("GPU test: eye render completed.");
        ++eyeFrames[eye];
    }
    if(!serialStereo) {
        // The right eye reads the left eye's TLAS. Both slots remain owned by
        // this pair until that final reader and its query resolve complete.
        const uint64_t pairFence=g_d3d.command_queue_direct->Signal();
        for(auto& frame:eyeFrameData[g_d3d.current_back_buffer_index]) frame.fence_value=pairFence;
    }
    inEyePass=false;
    g_d3d.io.scene_transition=false;
    g_vr_frame_override=nullptr;
    std::memcpy(g_d3d.render_targets,desktopTargets,sizeof(desktopTargets));
    g_d3d.scene=savedScene;
    g_d3d.accum_frame_index=savedAccum;
    g_d3d.upscaling_aa_mode=savedAA;
    g_d3d.output_width=savedOutputWidth; g_d3d.output_height=savedOutputHeight;
    g_d3d.render_width=savedRenderWidth; g_d3d.render_height=savedRenderHeight;
    g_d3d.render_width_override=savedOverrideWidth;g_d3d.render_height_override=savedOverrideHeight;
    tweak_vars=savedTweaks;
    if(offlineTest) MirrorLeftEye();
    g_d3d.io.delta_time=savedDeltaTime;
    if(levelTest) {
        static unsigned frames;
        static std::vector<double> samples;
        static double stageSums[stageCount]={};
        if(++frames>8 && lastEyeGPU[0]>0 && lastEyeGPU[1]>0) {
            samples.push_back(lastEyeGPU[0]+lastEyeGPU[1]);
            for(UINT stage=0;stage<stageCount;++stage) stageSums[stage]+=lastStageGPU[0][stage]+lastStageGPU[1][stage];
        }
        if(frames==120 && !samples.empty()) {
            double mean=0;for(double v:samples) mean+=v;mean/=samples.size();
            if(FILE* f=std::fopen("vr-leveltest-stages.json","wb")) {
                const char* labels[]={"primary","direct","resample","spatial","bloom","post","taa_fsr","indirect","tlas"};
                std::fprintf(f,"{");
                for(UINT stage=0;stage<stageCount;++stage)
                    std::fprintf(f,"%s\"%s\":%.4f",stage ? "," : "",labels[stage],stageSums[stage]/samples.size());
                std::fprintf(f,"}\n");std::fclose(f);
            }
            std::sort(samples.begin(),samples.end());
            if(FILE* f=std::fopen("vr-leveltest-profile.json","wb")) {
                std::fprintf(f,"{\"samples\":%zu,\"eye_width\":%u,\"eye_height\":%u,\"gpu_pair_mean_ms\":%.4f,\"gpu_pair_p95_ms\":%.4f}\n",samples.size(),eyeWidth,eyeHeight,mean,samples[samples.size()*95/100]);std::fclose(f);
            }
        }
    }
    static ULONGLONG lastTiming;
    if(!offlineTest && GetTickCount64()-lastTiming>5000) {
        lastTiming=GetTickCount64();
        char timing[220]; std::snprintf(timing,sizeof(timing),"VR stereo: CPU recording/reuse %.3f ms; GPU L %.3f ms R %.3f ms (prior completed slot); %ux%u; serial %d.",ClockMS()-renderStarted,lastEyeGPU[0],lastEyeGPU[1],eyeWidth,eyeHeight,serialStereo ? 1 : 0); Log(timing);
        char lightTiming[160]; std::snprintf(lightTiming,sizeof(lightTiming),"VR light sampling: %u lights, candidate budget %u, mode %s.",g_d3d.lights_count,lightCandidateBudget,
            lightCandidateBudget && g_d3d.lights_count>lightCandidateBudget ? "subset" : "full"); Log(lightTiming);
        char stages[640]; std::snprintf(stages,sizeof(stages),"VR GPU stages L/R ms: primary %.2f/%.2f; lighting %.2f/%.2f; resample %.2f/%.2f; spatial %.2f/%.2f; bloom %.2f/%.2f; post %.2f/%.2f; indirect %.2f/%.2f; taa_fsr %.2f/%.2f; tlas %.2f/%.2f; specialized %d (prior completed slot).",
            lastStageGPU[0][0],lastStageGPU[1][0],lastStageGPU[0][1],lastStageGPU[1][1],lastStageGPU[0][2],lastStageGPU[1][2],
            lastStageGPU[0][3],lastStageGPU[1][3],lastStageGPU[0][4],lastStageGPU[1][4],lastStageGPU[0][5],lastStageGPU[1][5],
            lastStageGPU[0][7],lastStageGPU[1][7],lastStageGPU[0][6],lastStageGPU[1][6],lastStageGPU[0][8],lastStageGPU[1][8],fastSpatial ? 1 : 0); Log(stages);

    }
    stereoThisFrame=true;
    return true;
}
void OpenVRPrototype::Present() {
    static const bool captureMenu=HasArgument(L"-vr-graphics-menu-test");
    static unsigned menuCaptureFrames;
    if(captureMenu && ++menuCaptureFrames==4) {
        RenderBackend::RasterRender();
        SaveEye(g_d3d.rt.color_final,"vr-graphics-menu-test.png");
    }
    if(!active) return;
    if(Tracking() && active) {
        EnsureUI();
        if(levelTest && stereoThisFrame && HasArgument(L"-openvr-side-capture") &&
            (levelPresentedFrames==29 || levelPresentedFrames==89)) {
            SaveEye(eyeTargets[0][RenderTarget_color_final],levelPresentedFrames==29 ?
                "vr-leveltest-side-a.png" : "vr-leveltest-side-b.png");
        }
        if(levelTest && stereoThisFrame && HasArgument(L"-openvr-temporal-capture") && levelPresentedFrames>=89 && levelPresentedFrames<97) {
            char name[100]; std::snprintf(name,sizeof(name),"vr-leveltest-temporal-%u.png",levelPresentedFrames);
            SaveEye(eyeTargets[0][RenderTarget_color_final],name);
        }
        if(levelTest && stereoThisFrame && ++levelPresentedFrames==120) {
            SaveEye(eyeTargets[0][RenderTarget_color_final],"vr-leveltest-final-left.png");
            SaveEye(eyeTargets[1][RenderTarget_color_final],"vr-leveltest-final-right.png");
            Log("PASS: D2 level rendered 120 stereo frames.");
        }
        if(stereoThisFrame) {
            if(!offlineTest) MirrorLeftEye();
            Submit(eyeTargets[0][RenderTarget_color_final],eyeTargets[1][RenderTarget_color_final]);
        }
        else Submit(menuBackground,menuBackground); // Uniform background; UI exists only on the real quad.
        UpdatePanel();
        vr::Compositor_FrameTiming runtimeTiming={};runtimeTiming.m_nSize=sizeof(runtimeTiming);
        if(!offlineTest && vr::VRCompositor()->GetFrameTiming(&runtimeTiming,1)) {
            lastRuntimeGPU=runtimeTiming.m_flTotalRenderGpuMs;
            lastFrameInterval=runtimeTiming.m_flClientFrameIntervalMs;
            if(stereoThisFrame) {
                ++budgetFrames;
                if(VR_COMPOSITOR_ADDITIONAL_PREDICTED_FRAMES(runtimeTiming)>=2) ++budgetLateFrames;
            }
        }
        static ULONGLONG lastCompositorLog;
        if(!offlineTest && GetTickCount64()-lastCompositorLog>5000) {
            lastCompositorLog=GetTickCount64();
            vr::Compositor_FrameTiming timing={}; timing.m_nSize=sizeof(timing);
            // Frame 1 ago is complete; frame 0 is still in progress and reports partial intervals.
            if(vr::VRCompositor()->GetFrameTiming(&timing,1)) {
                char text[240]; std::snprintf(text,sizeof(text),"VR compositor: interval %.2f ms, GPU %.2f ms, presents %u, dropped %u, motion-smoothed %u, extra predicted frames %u, reprojection flags 0x%x.",timing.m_flClientFrameIntervalMs,timing.m_flTotalRenderGpuMs,timing.m_nNumFramePresents,timing.m_nNumDroppedFrames,(timing.m_nReprojectionFlags & vr::VRCompositor_ReprojectionMotion) ? 1u : 0u,static_cast<unsigned>(VR_COMPOSITOR_ADDITIONAL_PREDICTED_FRAMES(timing)),timing.m_nReprojectionFlags); Log(text);
                // Where the app's frame time goes, relative to this frame's vsync:
                // poses ready -> both eyes submitted is CPU time from tracking to
                // submission; pre-submit GPU is the scene work SteamVR waited for.
                char path[340]; std::snprintf(path,sizeof(path),"VR frame path: poses ready %.2f ms, eyes submitted %.2f ms (app CPU %.2f ms), pre-submit GPU %.2f ms (eyes %.2f, cameras/HUD/other %.2f), compositor idle %.2f ms, vsyncs to first view %u.",
                    timing.m_flNewPosesReadyMs,timing.m_flNewFrameReadyMs,timing.m_flNewFrameReadyMs-timing.m_flNewPosesReadyMs,
                    timing.m_flPreSubmitGpuMs,lastEyeGPU[0]+lastEyeGPU[1],timing.m_flPreSubmitGpuMs-(lastEyeGPU[0]+lastEyeGPU[1]),
                    timing.m_flCompositorIdleCpuMs,timing.m_nNumVSyncsToFirstView); Log(path);
                lastAppCpu=timing.m_flNewFrameReadyMs-timing.m_flNewPosesReadyMs;
                lastPreSubmitGpu=timing.m_flPreSubmitGpuMs;
            }
            // Sustained extra prediction means the frame is not finishing inside
            // one refresh, which adds a refresh of head-motion latency.
            if(budgetFrames>=30 && budgetLateFrames*2>budgetFrames) {
                const double budget=graphicsRefresh>0 ? 1000.0/graphicsRefresh : 0.0;
                const double gpu=lastEyeGPU[0]+lastEyeGPU[1];
                // SteamVR adds prediction when tracking -> CPU -> GPU does not fit
                // in one refresh. Name the part that dominates: CPU only when it is a
                // large share, otherwise the GPU total (eyes plus cameras/HUD).
                const double cpu=lastAppCpu>0 ? lastAppCpu : 0.0;
                const double gpuTotal=lastPreSubmitGpu>gpu ? lastPreSubmitGpu : gpu;
                const char* advice;
                if(budget>0 && cpu>budget*0.4) advice="CPU-bound: time between tracking and submission is the largest part; graphics settings will not help much.";
                else if(budget>0 && cpu+gpuTotal>budget*0.8) advice="GPU-bound: app CPU plus GPU is close to one refresh. Try VR Graphics > FSR 3.1 > Performance, or a lower D2_VR_LIGHT_CANDIDATES (e.g. 4).";
                else advice="Frame fits the refresh; the extra prediction comes from outside the eye render (other GPU load or SteamVR scheduling).";
                char warning[400]; std::snprintf(warning,sizeof(warning),"VR frame budget exceeded: %u of %u gameplay frames needed 2+ extra predicted frames (eye GPU %.1f ms, all pre-submit GPU %.1f ms, app CPU %.1f ms, budget %.1f ms). %s",
                    budgetLateFrames,budgetFrames,gpu,gpuTotal,cpu,budget,advice); Log(warning);
            }
            budgetFrames=budgetLateFrames=0;
        }
        if(!offlineTest) vr::VRCompositor()->PostPresentHandoff();
    }
    stereoThisFrame=false;
    poseSampled=poseValid=false;
    tweak_vars.upscaling_aa_mode=UPSCALING_AA_MODE_OFF;
}
void OpenVRPrototype::Shutdown() {
    if(systemVR) {
        g_d3d.command_queue_direct->Flush();
        if(uiOverlay!=vr::k_ulOverlayHandleInvalid) {
            vr::VROverlay()->HideOverlay(uiOverlay);
            vr::VROverlay()->DestroyOverlay(uiOverlay);
            uiOverlay=vr::k_ulOverlayHandleInvalid;
        }
        vr::VR_Shutdown();
        systemVR=nullptr;
    }
    ReleaseTargets();
    lastRuntimeGPU=lastFrameInterval=0;
    if(!mirrorSource.IsNull()) { g_d3d.cbv_srv_uav.Free(mirrorSource); mirrorSource={}; }
    auto releaseFrame=[](FrameData& frame) {
        if(frame.upload_buffer) RT_RELEASE_RESOURCE(frame.upload_buffer);
        if(frame.top_level_as) RT_RELEASE_RESOURCE(frame.top_level_as);
        if(frame.top_level_as_scratch) RT_RELEASE_RESOURCE(frame.top_level_as_scratch);
        if(frame.pixel_debug_readback) RT_RELEASE_RESOURCE(frame.pixel_debug_readback);
        if(!frame.descriptors.IsNull()) g_d3d.cbv_srv_uav.Free(frame.descriptors);
        const auto staging=frame.non_shader_descriptors;
        frame={};
        frame.non_shader_descriptors=staging;
    };
    for(auto& slot:eyeFrameData) for(auto& frame:slot) releaseFrame(frame);
    lastRightFrame=nullptr; g_vr_frame_override=nullptr;
    if(eyeQueryReadback) RT_RELEASE_RESOURCE(eyeQueryReadback);
    eyeQueryReadback=nullptr;
    if(eyeQueryHeap) { eyeQueryHeap->Release(); eyeQueryHeap=nullptr; }
    for(auto*& texture:uiTextures) { if(texture) RT_RELEASE_RESOURCE(texture); texture=nullptr; }
    if(aimTexture) RT_RELEASE_RESOURCE(aimTexture);
    if(menuBackground) RT_RELEASE_RESOURCE(menuBackground);
    uiTexture=aimTexture=menuBackground=nullptr;
    // The RTV heap is a linear arena that cannot free. Keep these views so a
    // later VR restart (SteamVR quit mid-game, then -OpenVR again) reuses them.
    uiView={};
    active=false;
}

#pragma once
struct ID3D12Resource;
struct D3D12_CPU_DESCRIPTOR_HANDLE;
struct ID3D12GraphicsCommandList;
struct RT_VRGraphicsState;
struct RT_VRGraphicsSettings;
namespace OpenVRPrototype {
RT_VRGraphicsState GraphicsState();
bool SaveGraphicsSettings(const RT_VRGraphicsSettings* settings);
void Init();
void Shutdown();
bool Active();
bool FixedMirrorCanvas();
float ObjectScale();
bool CapturingReticle();
void BeginReticle(float x, float y, float z);
void EndReticle();
void BeginFrame();
bool UIRenderTarget(ID3D12Resource** resource, D3D12_CPU_DESCRIPTOR_HANDLE* view);
bool RenderScene();
void Present();
bool EyePass();
bool ReuseScene();
// True while SteamVR's WaitGetPoses is pacing frames (game frame cap not needed).
bool CompositorPaced();
// FSR sharpening strength for eye passes (0..1).
float FsrSharpness();
// Per-eye frame counter used to ping-pong that eye's temporal history targets.
unsigned long long EyeHistoryIndex();
bool FirstEye();
bool FastSpatial();
bool HybridVisibility();
bool QueueEyeUploads();
ID3D12Resource* SharedTLAS();
void BeginEyeGpuTiming(ID3D12GraphicsCommandList* commands);
void EndEyeGpuTiming(ID3D12GraphicsCommandList* commands);
void ProjectionTangents(float* left, float* right, float* top, float* bottom);
}

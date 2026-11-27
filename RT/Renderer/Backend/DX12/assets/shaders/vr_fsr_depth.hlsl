#include "include/common.hlsl"
// FSR expects projected depth; the ray tracer stores distance along a world ray.
// Output reversed, infinite-far depth (near / view-space z). The renderer uses a
// 0.001 near plane and 10000 far plane, so standard [0,1] depth crowds almost the
// whole level into the last few float steps below 1.0. Reversed depth keeps full
// precision at every distance. FSR is created with DEPTH_INVERTED | DEPTH_INFINITE.
[numthreads(GROUP_X, GROUP_Y, 1)]
void VRFsrDepthCS(COMPUTE_ARGS) {
    EARLY_OUT
    float distance=img_depth[pixel_pos];
    float3 world=img_view_dir[pixel_pos].xyz*distance;
    float3 view=mul(g_global_cb.view,float4(world,0)).xyz;
    // Near plane from the projection itself: clip.z = A*z + B and near = -B/A.
    float A=mul(g_global_cb.proj,float4(0,0,2,1)).z-mul(g_global_cb.proj,float4(0,0,1,1)).z;
    float B=mul(g_global_cb.proj,float4(0,0,1,1)).z-A;
    float nearZ=-B/A;
    img_fsr_depth[pixel_pos]=(distance>=RT_RAY_T_MAX || view.z<=0) ? 0.0 : saturate(nearZ/view.z);
}

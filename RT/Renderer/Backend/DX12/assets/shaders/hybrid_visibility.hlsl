#include "include/common.hlsl"
#include "portal_retrace_ray.hlsl"
struct DrawConstants { uint instance_index; };
ConstantBuffer<DrawConstants> draw : register(b0,space2);
struct VertexOutput {
    float4 position : SV_Position;
    float2 bary : TEXCOORD0;
    float3 world : TEXCOORD1;
    nointerpolation uint primitive : TEXCOORD2;
};
VertexOutput HybridVS(uint vertex : SV_VertexID) {
    VertexOutput result;
    uint primitive=vertex/3,corner=vertex%3;
    InstanceData instance=g_instance_data_buffer[draw.instance_index];
    RT_Triangle hitTri=GetHitTriangle(instance.triangle_buffer_idx,primitive);
    float3 vertexPosition=corner==0 ? hitTri.pos0 : (corner==1 ? hitTri.pos1 : hitTri.pos2);
    result.world=mul(instance.object_to_world,float4(vertexPosition,1)).xyz;
    result.position=mul(g_global_cb.proj,mul(g_global_cb.view,float4(result.world,1)));
    result.position.xy+=float2(-2,-2)*g_global_cb.taa_jitter/g_global_cb.render_dim*result.position.w;
    result.bary=float2(corner==1,corner==2);
    result.primitive=primitive;
    return result;
}
uint2 HybridPS(VertexOutput input) : SV_Target0 {
    int2 pixel=int2(input.position.xy);
    Material material=(Material)0;
    if(IsHitTransparent(draw.instance_index,input.primitive,input.bary,pixel,material)) discard;
    InstanceData instance=g_instance_data_buffer[draw.instance_index];
    RT_Triangle hitTri=GetHitTriangle(instance.triangle_buffer_idx,input.primitive);
    bool cockpit=(instance.instance_mask & RT_RENDER_MASK_COCKPIT)!=0;
    if(!cockpit && tweak.retrace_rays && g_global_cb.ray_segment!=-1 &&
       hitTri.segment!=-1 && hitTri.segment!=g_global_cb.ray_segment) {
        // Preserve DMAX's segment validation for overlapping/unconnected mines.
        RayDesc camera=GetRayDesc(pixel,g_global_cb.render_dim);
        float distance=length(input.world-camera.Origin);
        RayDesc ray; ray.Origin=input.world; ray.Direction=-camera.Direction;
        ray.TMin=0; ray.TMax=distance+1;
        PortalRetraceRayPayload retrace;
        retrace.search_segment=hitTri.segment; retrace.found=false;
        retrace.hit_distance=RT_RAY_T_MAX; retrace.next_segment=-1;
        TracePortalRetraceRay(ray,retrace,pixel,false);
        if(retrace.found && retrace.next_segment!=g_global_cb.ray_segment) {
            retrace.search_segment=retrace.next_segment; retrace.found=false;
            retrace.hit_distance=RT_RAY_T_MAX; retrace.next_segment=-1;
            TracePortalRetraceRay(ray,retrace,pixel,false);
        }
        if(!retrace.found) discard;
    }
    return uint2(draw.instance_index,input.primitive);
}

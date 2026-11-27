#ifndef PRIMARY_RAY_INLINE_HLSL
#define PRIMARY_RAY_INLINE_HLSL

#include "include/common.hlsl"
#include "primary_ray.hlsli"
#include "portal_retrace_ray.hlsl"

// Smooth Transparency: every energy effect between the camera and the surface adds its color as
// light (alpha-weighted, like additive sprites), so explosions keep soft edges and dark pixels add
// nothing. Additive, so the order the hits come back in doesn't matter.
float3 AccumulateEnergyGlow(RayDesc ray, float max_t)
{
	float3 glow = 0;
	ray.TMax = max_t;
	RayQuery<RAY_FLAG_NONE> q;
	q.TraceRayInline(g_scene, RAY_FLAG_NONE, RT_RENDER_MASK_OBJECTS | RT_RENDER_MASK_PROJECTILES, ray);
	while (q.Proceed())
	{
		if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
			continue;
		InstanceData instance = g_instance_data_buffer[q.CandidateInstanceIndex()];
		RT_Triangle tri = GetHitTriangle(instance.triangle_buffer_idx, q.CandidatePrimitiveIndex());
		uint material_index = 0;
		float2 uv = 0;
		float3 normal = 0, tangent = 0;
		GetHitMaterialAndUVs(instance, tri, q.CandidateTriangleBarycentrics(), material_index, uv, normal, tangent);
		Material material = g_materials[material_index];
		if (!IsEnergyEffect(material))
			continue;
		float4 texel = GetTextureFromIndex(material.albedo_index).SampleLevel(g_sampler_linear_wrap, uv, 0);
		float4 color_mod = UnpackRGBA(instance.material_color) * UnpackRGBA(tri.color);
		glow += texel.rgb * texel.a * color_mod.rgb * color_mod.a * UnpackRGBE(material.emissive_factor);
	}
	return glow;
}

[numthreads(GROUP_X, GROUP_Y, 1)]
void PrimaryRayInline(COMPUTE_ARGS)
{
	// ---------------------------------------------------------------------------------------------------------------
	// Set up the primary ray and its payload, then trace the ray

	RayDesc ray = GetRayDesc(pixel_pos, g_global_cb.render_dim);
	PrimaryRayPayload payload = (PrimaryRayPayload)0;
	payload.primitive_idx = ~0;
	payload.instance_idx = ~0;
	payload.start_segment = g_global_cb.ray_segment;
	payload.hit_segment = -1;

#ifdef VR_HYBRID_VISIBILITY
    if(any(pixel_pos>=g_global_cb.render_dim)) return;
    uint2 visible=img_visibility_prim[pixel_pos];
    payload.instance_idx=visible.x; payload.primitive_idx=visible.y;
    payload.hit_distance=RT_RAY_T_MAX;
    if(visible.x!=~0u) {
        // Rasterization chooses the hitTri; solve only its intersection to
        // retain full-precision barycentrics for high-resolution cockpit maps.
        InstanceData instance=g_instance_data_buffer[visible.x];
        RT_Triangle hitTri=GetHitTriangle(instance.triangle_buffer_idx,visible.y);
        float3 origin=mul(instance.world_to_object,float4(ray.Origin,1)).xyz;
        float3 direction=mul(instance.world_to_object,float4(ray.Direction,0)).xyz;
        float3 edge1=hitTri.pos1-hitTri.pos0,edge2=hitTri.pos2-hitTri.pos0;
        float3 p=cross(direction,edge2),t=origin-hitTri.pos0;
        float inv=rcp(dot(edge1,p)); float3 q=cross(t,edge1);
        payload.barycentrics=float2(dot(t,p),dot(direction,q))*inv;
        payload.hit_distance=dot(edge2,q)*inv;
    }
    RayDesc geometry_ray=ray;
#else
	PrimaryRayPayload cockpit_payload = (PrimaryRayPayload)0;
	cockpit_payload.primitive_idx = ~0;
	cockpit_payload.instance_idx = ~0;
	cockpit_payload.start_segment = -1;
	cockpit_payload.hit_segment = -1;
	// VR must use the headset's asymmetric projection for cockpit and world alike.
	RayDesc cockpit_ray = ray;
	if ((g_global_cb.debug_flags & 0x80000000u) == 0)
		cockpit_ray = GetFixedFovRayDesc(pixel_pos, g_global_cb.render_dim, 85.0f);
	bool cockpitFirst=(g_global_cb.debug_flags & 0x40000000u)!=0;
	if(cockpitFirst) TracePrimaryRay(cockpit_ray, cockpit_payload, pixel_pos, RT_RENDER_MASK_COCKPIT);
	// A visible foreground cockpit replaces the world hit completely.
	// Skip the world traversal only after resolving cockpit transparency.
	if(!cockpitFirst || cockpit_payload.instance_idx==~0) {
	if (!g_global_cb.external)
	{
		TracePrimaryRay(ray, payload, pixel_pos, ~(RT_RENDER_MASK_PORTALS | RT_RENDER_MASK_COCKPIT));
	}
	else
	{
		RayDesc pretrace_ray = GetRayDesc(pixel_pos, g_global_cb.render_dim);
		PortalRetraceRayPayload pretrace_payload;
		pretrace_payload.search_segment = g_global_cb.ray_segment;
		pretrace_payload.found = false;
		pretrace_payload.hit_distance = RT_RAY_T_MAX;
		pretrace_payload.next_segment = -1;

		TracePortalRetraceRay(pretrace_ray, pretrace_payload, pixel_pos, true);

		if (pretrace_payload.found)
		{
			TracePrimaryRay(ray, payload, pixel_pos, ~(RT_RENDER_MASK_PORTALS | RT_RENDER_MASK_TERRAIN | RT_RENDER_MASK_COCKPIT));
		}
		else
		{
			TracePrimaryRay(ray, payload, pixel_pos, ~(RT_RENDER_MASK_PORTALS | RT_RENDER_MASK_LEVELGEOMETRY | RT_RENDER_MASK_COCKPIT));
		}
	}

	}
	if(!cockpitFirst) TracePrimaryRay(cockpit_ray, cockpit_payload, pixel_pos, RT_RENDER_MASK_COCKPIT);
	RayDesc geometry_ray = ray;
	if (cockpit_payload.instance_idx != ~0)
	{
		payload = cockpit_payload;
		geometry_ray = cockpit_ray;
	}
	
#endif

	// ---------------------------------------------------------------------------------------------------------------
	// Get the geometry data from the primary ray hit

	HitGeometry geo = (HitGeometry)0;
	geo.depth = RT_RAY_T_MAX;

	GetHitGeometryFromRay(geometry_ray,
		payload.instance_idx, payload.primitive_idx, payload.barycentrics, payload.hit_distance,
		0, pixel_pos, g_global_cb.render_dim, geo
	);

#ifndef VR_HYBRID_VISIBILITY
	// Energy effects in front of the world surface (not over the cockpit).
	if (tweak.smooth_transparency && cockpit_payload.instance_idx == ~0)
		geo.emissive += AccumulateEnergyGlow(ray, payload.instance_idx != ~0 ? payload.hit_distance : RT_RAY_T_MAX);
#endif

	// Keep the cockpit's authored material response. Bright-light protection
	// is applied during composition without flattening normal-map detail.

	// ---------------------------------------------------------------------------------------------------------------
	// Evaluate G-buffer motion vectors

    float3 geo_world_p = ReconstructWorldPosition(g_global_cb.view_inv, geometry_ray.Direction, payload.hit_distance);
	float3x4 world_to_object = float3x4(geo.instance_data.world_to_object[0],
										geo.instance_data.world_to_object[1],
										geo.instance_data.world_to_object[2]);

	float3 object_p     = mul(world_to_object, float4(geo_world_p, 1)).xyz;
	float3 prev_world_p = mul(geo.instance_data.object_to_world_prev, float4(object_p, 1)).xyz;

	if (!tweak.object_motion_vectors || tweak.freezeframe)
    {
		prev_world_p = geo_world_p;
    }

	float3 view_p      = mul(g_global_cb.view, float4(geo_world_p, 1)).xyz;
	float3 prev_view_p = mul(g_global_cb.prev_view, float4(prev_world_p, 1)).xyz;

	float2 screen_p      = Project(g_global_cb.proj, view_p);
	float2 prev_screen_p = Project(g_global_cb.prev_proj, prev_view_p);

	float2 screen_motion = prev_screen_p - screen_p;
	screen_motion.y = -screen_motion.y;

	geo.motion = screen_motion;

	// -------------------------------------------------------------------------------------
    // Write to G-buffers

    // G-buffer values are stored as follows:
    // img_albedo - albedo color as sampled from the texture (no color mod)
    // img_emissive - emissive color as sampled from the texture (no color mod)
    // img_normal - normal is stored as an octahedral encoding, world-space with normal mapping already applied
    // img_depth - linear, unnormalized depth (RAY_T)
    // img_motion - screen-space motion vectors
    // img_view_dir - view direction of pixel (ray direction), and a multiplier used to reconstruct world position with, 0 = no geometry hit, 1 = geometry hit
    // img_metallic - metallic, sampled from texture and modified with material metalness factor, metalness overrides
    // img_roughness - roughness, sampled from texture and modified with material roughness factor, roughness overrides, and path space regulation
    // img_material - material index of the global material buffer
    // img_visibility_prim - InstanceIndex() and PrimitiveIndex() of surface at hit location
    //      is used to get the InstanceData and RT_Triangle of the surface
    // img_visibility_bary - barycentric coordinates of surface at hit location

	img_albedo[pixel_pos] = float4(geo.albedo, 1.0);
	img_emissive[pixel_pos] = float4(geo.emissive, 0.0);
	img_normal[pixel_pos] = geo.normal;
	img_depth[pixel_pos] = geo.depth;
	img_motion[pixel_pos] = float4(geo.motion, 0, 0);
	img_view_dir[pixel_pos] = float4(geo.view_dir, select(geo.depth == RT_RAY_T_MAX, 0.0, 1.0));
	img_metallic[pixel_pos] = geo.metallic;
	img_roughness[pixel_pos] = geo.roughness;
	img_material[pixel_pos] = geo.material_index;
	img_visibility_prim[pixel_pos] = geo.vis_prim;
	img_visibility_bary[pixel_pos] = geo.vis_bary;

	if (tweak.upscaling_aa_mode == UPSCALING_AA_MODE_AMD_FSR_2_2)
	{
		// Determine if the pixel should write to the reactive mask for FSR 3.1
	    img_fsr2_reactive_mask[pixel_pos] = float((g_materials[geo.material_index].flags & RT_MaterialFlag_Fsr2ReactiveMask) != 0);
	}

#if RT_PIXEL_DEBUG
    // Write pixel debug data
    PixelDebugData debug_data = (PixelDebugData)0;

    debug_data.primitive_id = geo.vis_prim.y;
    debug_data.metallic_roughness = float2(geo.metallic, geo.roughness);
    debug_data.uv_barycentrics = float4(float2(0, 0), geo.vis_bary);
    debug_data.material_edge_index = geo.hit_triangle.material_edge_index;
    debug_data.material_index1 = geo.material_index;

    WritePixelDebug(debug_data);
#endif
}

#endif /* PRIMARY_RAY_INLINE_HLSL */

#ifndef DIRECT_LIGHTING_INLINE_HLSL
#define DIRECT_LIGHTING_INLINE_HLSL

#include "include/common.hlsl"
#include "primary_ray.hlsli"
#include "direct_lighting.hlsli"

void DirectLightingAtPixel(int2 pixel_pos)
{
	if (any(pixel_pos >= g_global_cb.render_dim)) return;
	// ----------------------------------------------------------------------------
	// Fetch geometry data from G-buffers

	HitGeometry geo = (HitGeometry)0;
	geo.material_index = img_material[pixel_pos].x;
	geo.albedo         = img_albedo[pixel_pos].xyz;
	geo.emissive       = img_emissive[pixel_pos].xyz;
	geo.normal		  = img_normal[pixel_pos].xy;
	geo.depth		  = img_depth[pixel_pos].r;
	geo.view_dir       = img_view_dir[pixel_pos].xyz;
	geo.metallic       = img_metallic[pixel_pos].x;
	geo.roughness      = img_roughness[pixel_pos].x;
	geo.vis_prim       = img_visibility_prim[pixel_pos].xy;
	geo.vis_bary       = img_visibility_bary[pixel_pos].xy;
	geo.world_p        = ReconstructWorldPosFromGBuffer(pixel_pos);
	
	// ----------------------------------------------------------------------------
	// Get the instance and hit triangle from the instance and primitive indices

	geo.instance_data  = g_instance_data_buffer[geo.vis_prim.x];
	geo.hit_triangle   = GetHitTriangle(geo.instance_data.triangle_buffer_idx, geo.vis_prim.y);
	
	// ----------------------------------------------------------------------------
	// Calculate the direct lighting at hit surface

	DirectLightingOutput direct_lighting_output = (DirectLightingOutput)0;
	CalculateDirectLightingAtSurface(geo, direct_lighting_output, pixel_pos, false);
    // Retain the mine's authored base illumination in VR. Some missions have
    // few physical light fixtures: one bounced ray cannot reconstruct their
    // baked lighting. This floor preserves those gradients without more rays,
    // and applies equally to original and upscaled wall textures.
    if ((g_global_cb.debug_flags & 0x80000000u) &&
        !(g_global_cb.debug_flags & GlobalDebugFlags_DisableVRStockFill) &&
        !g_global_cb.external && HasHitGeometry(geo.vis_prim) &&
        (geo.instance_data.instance_mask & RT_RENDER_MASK_LEVELGEOMETRY)) {
        uint packed=geo.hit_triangle.baked_lighting;
        float3 vertex_light=float3(packed & 1023u,(packed >> 10) & 1023u,(packed >> 20) & 1023u)/1023.0;
        float3 weights=float3(1.0-geo.vis_bary.x-geo.vis_bary.y,geo.vis_bary.xy);
        float fill=0.35*saturate(dot(vertex_light,weights));
        direct_lighting_output.direct_lighting=max(direct_lighting_output.direct_lighting,fill.xxx);
    }



	if (tweak.svgf_enabled)
	{
		direct_lighting_output.direct_specular *= rcp(lerp(0.04, 0.01 + direct_lighting_output.albedo, geo.metallic));
	}
	
	// ----------------------------------------------------------------------------
	// Write the diffuse, specular, and emissive lighting to their respective buffers

	img_spec[pixel_pos] = float4(direct_lighting_output.direct_specular, 0);
	img_diff[pixel_pos] = float4(direct_lighting_output.direct_lighting, 0);
	img_emissive[pixel_pos] = float4(direct_lighting_output.emissive_lighting, 0);
}

[numthreads(DIRECT_GROUP_X, DIRECT_GROUP_Y, 1)]
void DirectLightingInline(COMPUTE_ARGS)
{
    DirectLightingAtPixel(pixel_pos);
}

[numthreads(GROUP_X, GROUP_Y, 1)]
void DirectLightingInlineReference(COMPUTE_ARGS)
{
    DirectLightingAtPixel(pixel_pos);
}

#endif /* DIRECT_LIGHTING_INLINE_HLSL */

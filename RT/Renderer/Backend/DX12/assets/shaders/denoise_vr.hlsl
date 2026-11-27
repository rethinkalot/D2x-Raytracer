#include "include/common.hlsl"
// VR specialization: no history-dependent spatial variance and no jitter.
// Keep the same five color kernels, geometry weights, and precision.
// Temporal moments do not feed these weights or the final lighting.
struct DenoiseCB { uint iteration; };
ConstantBuffer<DenoiseCB> g_denoise_cb : register(b0, space2);
void DoSvgfFilterStep(int2 center_pos,
    RWTexture2D<float4> source_diff, RWTexture2D<float4> source_spec,
    out float3 result_direct, out float3 result_spec)
{
    float3 center_normal=DecodeNormalOctahedron(img_normal[center_pos].xy);
    float center_depth=img_depth[center_pos].x;
    float center_metallic=img_metallic[center_pos].x;
    uint center_material=img_material[center_pos].x;
    float3 sum_diff=source_diff[center_pos].rgb;
    float3 sum_spec=source_spec[center_pos].rgb;
    float sum_w=1;
    int step_size=1L << g_denoise_cb.iteration;
    float kernel[2][2]={{1.0,0.5},{0.5,0.25}};
    [unroll] for(int x=-1;x<=1;++x)
    [unroll] for(int y=-1;y<=1;++y) {
        if(x==0 && y==0) continue;
        int2 pos=clamp(center_pos+step_size*int2(x,y),int2(0,0),g_global_cb.render_dim-1);
        float3 sample_normal=DecodeNormalOctahedron(img_normal[pos].xy);
        float depth_dist=abs(center_depth-img_depth[pos].x);
        float normal_dist=max(0,dot(center_normal,sample_normal));
        float metallic_dist=abs(center_metallic-img_metallic[pos].x);
        float w=kernel[abs(x)][abs(y)];
        w*=min(exp(-(depth_dist/square(tweak.svgf_depth_sigma))),1.0);
        w*=pow(normal_dist,tweak.svgf_normal_sigma);
        w*=min(exp(-(metallic_dist/square(tweak.svgf_metallic_sigma))),1.0);
        w*=(img_material[pos].x==center_material);
        sum_diff+=w*source_diff[pos].rgb;
        sum_spec+=w*source_spec[pos].rgb;
        sum_w+=w;
    }
    float inv=rcp(sum_w);
    result_direct=sum_diff*inv;
    result_spec=sum_spec*inv;
}

[numthreads(GROUP_X, GROUP_Y, 1)]
void DenoiseDirectCS(int2 pixel_pos : SV_DispatchThreadID)
{
	EARLY_OUT

	float3 filtered_direct, filtered_spec;
	switch (g_denoise_cb.iteration)
	{
		case 0: DoSvgfFilterStep(pixel_pos, img_diff_denoise_ping, img_spec_denoise_ping, filtered_direct, filtered_spec); break;
		case 1: DoSvgfFilterStep(pixel_pos, img_diff_denoise_pong, img_spec_denoise_pong, filtered_direct, filtered_spec); break;
		case 2: DoSvgfFilterStep(pixel_pos, img_diff_hist,         img_spec_hist, filtered_direct, filtered_spec); break;
		case 3: DoSvgfFilterStep(pixel_pos, img_diff_denoise_ping, img_spec_denoise_ping, filtered_direct, filtered_spec); break;
		case 4: DoSvgfFilterStep(pixel_pos, img_diff_denoise_pong, img_spec_denoise_pong, filtered_direct, filtered_spec); break;
	}

	switch (g_denoise_cb.iteration)
	{
		case 0: 
		{
			img_diff_denoise_pong[pixel_pos].rgb = filtered_direct; 
			img_spec_denoise_pong[pixel_pos].rgb = filtered_spec;

		} break;

		case 1: 
		{
			img_diff_hist[pixel_pos].rgb = filtered_direct; 
			img_spec_hist[pixel_pos].rgb = filtered_spec;

		} break;

		case 2: 
		{
			img_diff_denoise_ping[pixel_pos].rgb = filtered_direct; 
			img_spec_denoise_ping[pixel_pos].rgb = filtered_spec;

		} break;

		case 3: 
		{
			img_diff_denoise_pong[pixel_pos].rgb = filtered_direct; 
			img_spec_denoise_pong[pixel_pos].rgb = filtered_spec;

		} break;

		case 4: 
		{
			img_diff_denoise_ping[pixel_pos].rgb = filtered_direct; 
			img_spec_denoise_ping[pixel_pos].rgb = filtered_spec;

		} break;
	}
}


#include "include/common.hlsl"
// Short per-eye lighting history retains the inexpensive spatial kernels.
// Never read/write reprojected history lengths in the same dispatch.
float3 ClampFireflyLuminance(float3 color, float max_luminance)
{
    color = max(color, 0.0);
    float luminance = Luminance(color);
    return color * min(1.0, max_luminance / max(luminance, 0.0001));
}
[numthreads(GROUP_X, GROUP_Y, 1)]
void Denoise_Resample(int2 pixel_pos : SV_DispatchThreadID)
{
    if (any(pixel_pos >= g_global_cb.render_dim)) return;
    float3 diff=ClampFireflyLuminance(img_diff[pixel_pos].rgb, tweak.svgf_firefly_diffuse);
    float3 spec=ClampFireflyLuminance(img_spec[pixel_pos].rgb, tweak.svgf_firefly_specular);
    if(g_global_cb.frame_index>0 && tweak.svgf_max_hist_len_diff>1) {
        float3 normal=DecodeNormalOctahedron(img_normal[pixel_pos].xy);
        float3 world=ReconstructWorldPosFromGBuffer(pixel_pos);
        BilinearSamples samples;
        GetBilinearSamples(float2(pixel_pos)+g_global_cb.render_dim*img_motion[pixel_pos].xy,samples);
        float3 hist_diff=0,hist_spec=0;
        float weight=0;
        [unroll] for(int i=0;i<4;++i) {
            int2 pos=samples.positions[i];
            if(any(pos<0) || any(pos>=g_global_cb.render_dim)) continue;
            float3 prev_normal=DecodeNormalOctahedron(img_normal_prev[pos].xy);
            float3 prev_world=ReconstructPrevWorldPosFromGBuffer(pos);
            // Reject disocclusions and differently facing surfaces before blending.
            float w=samples.weights[i];
            w*=dot(normal,prev_normal)>0.95;
            w*=abs(dot(normal,world-prev_world))<0.1;
            hist_diff+=w*img_diff_hist[pos].rgb;
            hist_spec+=w*img_spec_hist[pos].rgb;
            weight+=w;
        }
        if(weight>0.01) {
            float age=min(float(g_global_cb.frame_index+1),float(tweak.svgf_max_hist_len_diff));
            diff=lerp(hist_diff/weight,diff,rcp(age));
            float spec_age=min(float(g_global_cb.frame_index+1),float(tweak.svgf_max_hist_len_spec));
            spec=lerp(hist_spec/weight,spec,rcp(spec_age));
        }
    }
    img_diff_denoise_ping[pixel_pos].rgb=diff;
    img_spec_denoise_ping[pixel_pos].rgb=spec;
    img_history_length[pixel_pos] = 0;
}

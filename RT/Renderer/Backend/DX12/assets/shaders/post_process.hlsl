#include "include/common.hlsl"

//------------------------------------------------------------------------

float LinearTosRGB(float x)
{
	return x <= 0.0031308 ? x*12.92 : pow(1.055*x, 1.0 / 2.4) - 0.055;
}

float3 LinearTosRGB(float3 x)
{
	return float3(LinearTosRGB(x.x), LinearTosRGB(x.y), LinearTosRGB(x.z));
}

float NaturalShoulder(float x)
{
    return 1.0 - exp(-x);
}

float NaturalShoulder(float x, float t)
{
    float v1 = x;
    float v2 = t + (1.0 - t) * NaturalShoulder((x - t) / (1.0 - t));
    return x <= t ? v1 : v2;
}

float3 NaturalShoulder(float3 x, float t)
{
    return float3(
        NaturalShoulder(x.x, t),
        NaturalShoulder(x.y, t),
        NaturalShoulder(x.z, t)
    );
}

float3 ApplyTonemappingCurve(float3 color)
{
    return NaturalShoulder(color.xyz, tweak.tonemap_linear_section) * rcp(NaturalShoulder(tweak.tonemap_whitepoint, tweak.tonemap_linear_section));
}

float3 ColorPreservingTonemap(float3 color)
{
    float3 per_channel = ApplyTonemappingCurve(color.xyz);

    float peak = max(color.x, max(color.y, color.z));
    color.xyz *= rcp(peak + 1e-6);
    color.xyz *= ApplyTonemappingCurve(peak);

    color.xyz = lerp(color.xyz, per_channel, tweak.tonemap_hue_shift);

    return color;
}

// Spatial edge antialiasing for VR. Read only this frame's image: the cockpit
// and world get filtered before the classic reticle is rasterized separately.
float3 VREdgeAA(float2 uv, float3 center)
{
	float2 texel = rcp(float2(g_global_cb.output_dim));
	float3 nw = tex_taa_result.SampleLevel(g_sampler_linear_clamp, uv+float2(-1,-1)*texel, 0).rgb;
	float3 ne = tex_taa_result.SampleLevel(g_sampler_linear_clamp, uv+float2( 1,-1)*texel, 0).rgb;
	float3 sw = tex_taa_result.SampleLevel(g_sampler_linear_clamp, uv+float2(-1, 1)*texel, 0).rgb;
	float3 se = tex_taa_result.SampleLevel(g_sampler_linear_clamp, uv+float2( 1, 1)*texel, 0).rgb;
	float m = Luminance(center), a = Luminance(nw), b = Luminance(ne), c = Luminance(sw), d = Luminance(se);
	float lo = min(m,min(min(a,b),min(c,d))), hi = max(m,max(max(a,b),max(c,d)));
	if (hi-lo < max(0.03125,hi*0.125)) return center;
	float2 direction = float2(-(a+b-c-d),a+c-b-d);
	float reduction = max((a+b+c+d)*0.03125,0.0078125);
	direction = clamp(direction/(min(abs(direction.x),abs(direction.y))+reduction),-4.0,4.0)*texel;
	float3 inner = 0.5*(tex_taa_result.SampleLevel(g_sampler_linear_clamp,uv-direction/6,0).rgb+
		tex_taa_result.SampleLevel(g_sampler_linear_clamp,uv+direction/6,0).rgb);
	float3 outer = 0.5*inner+0.25*(tex_taa_result.SampleLevel(g_sampler_linear_clamp,uv-direction*0.5,0).rgb+
		tex_taa_result.SampleLevel(g_sampler_linear_clamp,uv+direction*0.5,0).rgb);
	float luminance = Luminance(outer);
	return luminance<lo || luminance>hi ? inner : outer;
}

//------------------------------------------------------------------------
// Player shield bubbles and the first-person shield sweep (ported from the
// Inferno DMAX shield effects). The game sets these once per frame through
// RT_SetShieldEffects; b2 is the third CBV in the global descriptor table.

#define RT_MAX_SHIELD_BUBBLES 8

struct ShieldBubble
{
	float4 center_radius;          // xyz world centre, w radius
	float4 shield_color_visibility;// rgb shield colour, w visibility
	float4 impact_color_strength;  // rgb impact colour, w impact strength
	float4 impact_dir_fade;        // xyz impact direction, w impact fade
	float4 params0;                // damage, invulnerable, shutdown fade, expiry warning
	float4 params1;                // activation fade, destabilizing, self bubble, charging
};

struct ShieldEffects
{
	uint   bubble_count;
	float  time;
	float  sweep_progress;         // < 0: no sweep
	float  sweep_fade;
	float  sweep_invulnerable;
	float  sweep_activating;
	float  sweep_elapsed;
	float  sweep_pad;
	ShieldBubble bubbles[RT_MAX_SHIELD_BUBBLES];
};

ConstantBuffer<ShieldEffects> g_shield_effects : register(b2);

float ShieldHash(float value)
{
	return fmod(sin(value * 12.9898) * 43758.5453, 1.0) * 0.5 + 0.5;
}

float ShieldEnergyFlow(float3 local, float time)
{
	float wave_a = sin(dot(local, float3(4.1, 2.7, 3.6)) * 2.15 + time * 1.35);
	float wave_b = sin(dot(local, float3(-2.4, 4.8, 3.1)) * 2.55 - time * 1.05);
	float wave_c = sin(dot(local, float3(3.3, -2.1, 5.2)) * 1.85 + time * 0.72);
	float current = 0.5 + 0.5 * (wave_a * 0.48 + wave_b * 0.32 + wave_c * 0.20);
	return smoothstep(0.34, 0.88, current);
}

float ShieldArc(float distance_to_path, float gate, float flicker)
{
	float core = exp(-abs(distance_to_path) * 72.0);
	float glow = exp(-abs(distance_to_path) * 19.0) * 0.24;
	return (core + glow) * gate * flicker;
}

float3 ShieldRotateX(float3 p, float angle) { float s = sin(angle), c = cos(angle); return float3(p.x, p.y * c - p.z * s, p.y * s + p.z * c); }
float3 ShieldRotateY(float3 p, float angle) { float s = sin(angle), c = cos(angle); return float3(p.x * c + p.z * s, p.y, -p.x * s + p.z * c); }
float3 ShieldRotateZ(float3 p, float angle) { float s = sin(angle), c = cos(angle); return float3(p.x * c - p.y * s, p.x * s + p.y * c, p.z); }

// One point on the bubble surface; returns additive colour (already times alpha).
float3 ShadeShieldBubble(ShieldBubble s, float3 local, float3 view, float time)
{
	const float3 normal = local;
	const float3 shield_color = s.shield_color_visibility.rgb;
	const float visibility = s.shield_color_visibility.w;
	// Your own hits take the colour of your shield (blue, or gold while
	// invulnerable) instead of the weapon's; other players keep weapon colours.
	const float3 impact_color = lerp(s.impact_color_strength.rgb, s.shield_color_visibility.rgb * 1.05, 0.8 * s.params1.z);
	const float impact_strength = s.impact_color_strength.w;
	const float impact_fade = s.impact_dir_fade.w;
	const float damage = s.params0.x;
	const float invulnerable = s.params0.y;
	const float shutdown_fade = s.params0.z;
	const float expiry_warning = s.params0.w;
	const float activation_fade = s.params1.x;
	const float destabilizing = s.params1.y;
	// Your own bubble, seen from inside in VR: the view runs straight out
	// through the surface, so the rim terms vanish and the always-on base
	// glow would tint the whole world. Keep only hits, arcs and sweeps, faint.
	const float self_bubble = s.params1.z;
	const float base_glow = 1.0 - self_bubble;
	// Hits and the power-up / shutdown fronts are the self view's main cues.
	const float self_cue = lerp(1.0, 1.7, self_bubble);
	const float charging = s.params1.w;

	// saturate: rounding can push |dot| a hair past 1, and pow() of a negative
	// number is NaN (black specks). From inside your own bubble the view is
	// almost exactly along the normal everywhere, so this happened a lot there.
	const float fresnel = pow(saturate(1.0 - abs(dot(normal, view))), 2.65);
	const float3 hit_dir = s.impact_dir_fade.xyz;
	const float hit_facing = dot(normal, hit_dir * rsqrt(max(dot(hit_dir, hit_dir), 1e-8)));
	// Seen from inside, the hit cone covers a big part of the view (the 0.57
	// cone is ~110 degrees across) and its white core washed out the screen.
	// The self view gets a tighter ripple and a softer core.
	const float halo_start = lerp(0.57, 0.74, s.params1.z);
	const float core_start = lerp(0.91, 0.965, s.params1.z);
	const float halo = pow(saturate((hit_facing - halo_start) / (1.0 - halo_start)), 2.15);
	const float core = pow(saturate((hit_facing - core_start) / (1.0 - core_start)), 2.4) * lerp(1.0, 0.45, s.params1.z);
	const float impact = halo * impact_strength * impact_fade;

	const float flow = ShieldEnergyFlow(local, time);
	const float shimmer = 0.84 + 0.16 * sin(time * 7.0 + dot(local, float3(9.1, 13.7, 17.3)));

	float arcs = 0.0;
	float warning_stutter = 0.0;
	float warning_flash = 0.0;
	[branch] if (invulnerable > 0.0 || destabilizing > 0.0)
	{
		const float flicker_a = saturate(0.52 + 0.31 * sin(time * 13.7 + local.z * 19.0) + 0.24 * sin(time * 31.1 - local.x * 11.0));
		const float flicker_b = saturate(0.48 + 0.34 * sin(time * 17.3 - local.x * 23.0) + 0.27 * sin(time * 27.7 + local.y * 15.0));
		const float burst_a = smoothstep(0.24, 0.68, 0.5 + 0.31 * sin(time * 2.17) + 0.19 * sin(time * 5.83 + 1.7));
		const float burst_b = smoothstep(0.30, 0.72, 0.5 + 0.28 * sin(time * 1.73 + 2.4) + 0.22 * sin(time * 7.11));
		const float burst_c = smoothstep(0.34, 0.76, 0.5 + 0.33 * sin(time * 2.63 + 4.1) + 0.17 * sin(time * 8.37 + 0.8));
		const float burst_d = smoothstep(0.28, 0.70, 0.5 + 0.26 * sin(time * 1.31 + 5.0) + 0.25 * sin(time * 6.49 + 2.1));
		const float jitter_a = sin(time * 19.0 + sin(time * 3.7) * 4.0) * 0.018;
		const float jitter_b = sin(time * 23.0 + sin(time * 4.3 + 1.0) * 3.0) * 0.015;
		const float3 p_a = ShieldRotateZ(ShieldRotateY(local, time * 0.91 + sin(time * 0.37) * 1.8), sin(time * 0.53 + 1.2) * 1.5);
		const float3 p_b = ShieldRotateX(ShieldRotateZ(local, -time * 0.73 + sin(time * 0.41) * 2.1), sin(time * 0.67 + 3.0) * 1.7);
		const float3 p_c = ShieldRotateY(ShieldRotateX(local, time * 1.07 + sin(time * 0.29) * 2.4), sin(time * 0.47 + 4.4) * 1.9);
		const float path_a = p_a.y - (sin(p_a.x * 4.8 + time * 1.35) * 0.22 + sin(p_a.z * 10.0 - time * 2.1) * 0.055 + jitter_a);
		const float path_b = p_b.z - (sin(p_b.y * 5.3 - time * 1.05) * 0.19 + sin(p_b.x * 12.0 + time * 2.7) * 0.045 + jitter_b);
		const float path_branch = p_a.y - (sin(p_a.x * 6.5 + time * 1.35) * 0.12 + 0.34);
		const float path_c = p_c.x - (sin(p_c.z * 5.8 + time * 1.72) * 0.20 - sin(p_c.y * 13.0 - time * 2.4) * 0.040);
		const float path_d = p_b.z - (sin(p_b.x * 7.2 - time * 1.48) * 0.13 - 0.31);
		const float gate_a = smoothstep(-0.72, -0.30, p_a.x) * (1.0 - smoothstep(0.48, 0.82, p_a.x));
		const float gate_b = smoothstep(-0.68, -0.20, p_b.y) * (1.0 - smoothstep(0.36, 0.76, p_b.y));
		const float branch_gate = smoothstep(0.05, 0.28, p_a.x) * (1.0 - smoothstep(0.55, 0.82, p_a.x));
		const float gate_c = smoothstep(-0.78, -0.42, p_c.z) * (1.0 - smoothstep(0.18, 0.58, p_c.z));
		const float gate_d = smoothstep(-0.58, -0.18, p_b.y) * (1.0 - smoothstep(0.55, 0.86, p_b.y));
		arcs = ShieldArc(path_a, gate_a, flicker_a * burst_a) +
			ShieldArc(path_b, gate_b, flicker_b * burst_b) +
			ShieldArc(path_branch, branch_gate, flicker_a * flicker_b * burst_c) * 0.72 +
			ShieldArc(path_c, gate_c, flicker_b * burst_c) * 0.82 +
			ShieldArc(path_d, gate_d, flicker_a * burst_d) * 0.74;
		warning_stutter = smoothstep(0.05, 0.55, 0.5 + 0.5 * sin(time * 18.0 + sin(time * 6.7) * 2.8));
		warning_flash = pow(saturate(0.5 + 0.5 * sin(time * 10.5)), 5.0);
		arcs *= 1.0 + expiry_warning * (1.25 + warning_stutter * 1.65);
	}

	const float invul_pulse = 0.82 + 0.18 * sin(time * 3.4);
	float3 rgb = shield_color * (0.07 + fresnel * 0.52) * base_glow;
	rgb += shield_color * flow * ((0.018 + fresnel * 0.055) * base_glow + impact * 0.13) * shimmer;
	rgb += impact_color * impact * (0.55 + flow * 0.20) * self_cue;
	// White spark at the centre of a hit; blue-white for your own bubble.
	rgb += lerp(float3(2.8, 2.8, 3.0), float3(1.2, 1.6, 3.0), self_bubble) * core * impact_strength * impact_fade * 0.38 * self_cue;
	rgb += shield_color * flow * damage * 0.035 * base_glow;
	rgb += shield_color * invulnerable * ((0.018 + fresnel * 0.48) * base_glow + arcs * lerp(0.72, 0.55, self_bubble)) * invul_pulse;
	rgb += impact_color * invulnerable * arcs * (0.22 + impact * 0.62);
	rgb += impact_color * invulnerable * impact * (0.34 + arcs * 0.75 + core * 0.45);
	rgb += shield_color * invulnerable * expiry_warning * (fresnel * (0.30 + warning_flash * 0.75) + arcs * 0.24);
	rgb += shield_color * destabilizing * (0.025 + fresnel * (0.20 + warning_flash * 0.30) + arcs * 0.58);
	rgb += float3(0.72, 0.92, 2.8) * destabilizing * arcs * 0.24;
	float alpha = saturate(0.30 * base_glow + 0.25 * self_bubble + fresnel * 0.30 + impact * 0.28 + core * 0.18 + invulnerable * (0.018 + arcs * 0.24 + fresnel * 0.16));
	alpha += destabilizing * (0.035 + fresnel * 0.12 + arcs * 0.12);

	// Energy-to-shield transfer: pulses of energy-gold light climb the bubble
	// and turn shield-blue as they rise, while the surface currents surge.
	[branch] if (charging > 0.0)
	{
		const float rise = saturate(local.y * 0.5 + 0.5);
		const float swirl = sin(local.x * 7.0 + local.z * 5.0 + time * 3.1) * 0.06;
		const float phase = frac(local.y * 1.35 - time * 2.3 + swirl);
		const float band = pow(saturate(1.0 - abs(phase - 0.5) * 4.5), 2.5);
		const float throb = 0.75 + 0.25 * sin(time * 11.0);
		const float3 energy_color = lerp(float3(2.6, 1.7, 0.35), shield_color * 1.4, rise);
		rgb += energy_color * band * 0.55 * charging;
		rgb += shield_color * flow * (0.10 + fresnel * 0.20) * throb * charging;
		alpha += charging * (0.10 + band * 0.30);
	}

	// Power-up: the field charges from top to bottom.
	if (activation_fade < 1.0)
	{
		const float progress = smoothstep(0.0, 1.0, activation_fade);
		const float ripple = sin(local.x * 15.0 + local.z * 11.0 + time * 10.0) * 0.055;
		const float boundary = lerp(1.16, -1.16, progress) + ripple;
		const float reveal = smoothstep(boundary - 0.075, boundary + 0.075, local.y);
		const float charge_edge = exp(-abs(local.y - boundary) * 22.0);
		const float3 charge_color = lerp(shield_color * 1.55, float3(3.2, 1.5, 0.28), invulnerable);
		rgb += charge_color * charge_edge * 0.62 * self_cue;
		rgb *= reveal;
		alpha *= reveal;
	}

	// Shutdown: the field discharges from top to bottom.
	if (shutdown_fade > 0.0)
	{
		const float progress = smoothstep(0.0, 1.0, shutdown_fade);
		const float ripple = sin(local.x * 15.0 + local.z * 11.0 + time * 9.0) * 0.055;
		const float boundary = lerp(1.16, -1.16, progress) + ripple;
		const float remaining = 1.0 - smoothstep(boundary - 0.075, boundary + 0.075, local.y);
		const float discharge_edge = exp(-abs(local.y - boundary) * 24.0) * saturate(shutdown_fade * 8.0);
		const float flicker = lerp(0.42, 1.0, smoothstep(0.15, 0.70, 0.5 + 0.5 * sin(time * 24.0)));
		const float3 discharge_color = lerp(shield_color * 1.45, float3(3.0, 1.35, 0.24), invulnerable);
		rgb += discharge_color * discharge_edge * 0.46 * self_cue;
		rgb *= remaining * flicker;
		alpha *= remaining * flicker;
	}

	const float warning_power = lerp(1.0, lerp(0.48, 1.28, warning_stutter), expiry_warning);
	const float critical_power = lerp(1.0, lerp(0.58, 1.30, warning_stutter), destabilizing);
	const float scale = visibility * warning_power * critical_power * lerp(1.0, 0.85, self_bubble);
	// Additive with source alpha, like the original effect.
	// Other bubbles use the original additive blend (alpha scaled as well).
	// The self view applies the scale once, or it ends up far too faint.
	return rgb * scale * saturate(alpha * lerp(scale, 1.0, self_bubble));
}

float3 ShieldBubblesAlongRay(float3 origin, float3 dir, float scene_depth)
{
	float3 sum = 0.0;
	const float time = g_shield_effects.time;
	const uint count = min(g_shield_effects.bubble_count, (uint)RT_MAX_SHIELD_BUBBLES);
	for (uint i = 0; i < count; i++)
	{
		const ShieldBubble s = g_shield_effects.bubbles[i];
		// Your own bubble surrounds the camera, so nothing you see can really
		// be in front of it. Against a wall the wall is nearer than the bubble
		// surface, and depth-testing cut wall-shaped holes and outlines into it.
		const bool self_view = s.params1.z > 0.5;
		const float3 oc = origin - s.center_radius.xyz;
		const float b = dot(oc, dir);
		const float c = dot(oc, oc) - s.center_radius.w * s.center_radius.w;
		const float h = b * b - c;
		if (h <= 0.0)
			continue;
		const float root = sqrt(h);
		// Both the near and far side of the bubble, as with the original
		// (drawn without back-face culling), each hidden behind the scene.
		[unroll] for (int side = 0; side < 2; side++)
		{
			const float t = side == 0 ? -b - root : -b + root;
			if (t <= 0.0 || (!self_view && t >= scene_depth))
				continue;
			const float3 local = normalize(origin + dir * t - s.center_radius.xyz);
			sum += ShadeShieldBubble(s, local, -dir, time);
		}
	}
	return sum;
}

// First-person shield collapse / power-up: a curved energy front sweeping
// down the view (screen space, so it works the same in each VR eye).
float3 ShieldSweep(float2 uv, float2 size)
{
	const float progress = g_shield_effects.sweep_progress;
	if (progress < 0.0)
		return 0.0;
	const float fade = g_shield_effects.sweep_fade;
	const bool invulnerable = g_shield_effects.sweep_invulnerable > 0.5;
	const bool activating = g_shield_effects.sweep_activating > 0.5;
	const float elapsed = g_shield_effects.sweep_elapsed;
	const float time = g_shield_effects.time;

	const float4 field_color = invulnerable ? float4(1.45, 0.58, 0.10, 0.006) : float4(0.15, 0.31, 1.35, 0.005);
	const float4 glow_color  = invulnerable ? float4(2.5, 1.08, 0.20, 0.17)   : float4(0.28, 0.58, 2.35, 0.15);
	const float4 core_color  = invulnerable ? float4(3.0, 1.85, 0.68, 0.38)   : float4(0.85, 1.16, 2.8, 0.34);

	const float y = uv.y * size.y;
	const float nx = uv.x * 2.0 - 1.0;
	const float base_y = lerp(-size.y * 0.16, size.y * 1.10, progress);
	const float curve_y = base_y + sin(nx * 11.0 + time * 12.0) * size.y * 0.007 + nx * nx * size.y * 0.14;

	float3 rgb = field_color.rgb * field_color.a;
	// Soft-edged bands: rgb * alpha where |distance| < half width.
	#define SHIELD_BAND(center, half_width, color) \
		(color.rgb * color.a * (1.0 - smoothstep(0.55, 1.0, abs(y - (center)) / max(half_width, 0.5))))

	const float trail_offset = size.y * (activating ? -0.050 : 0.050);
	const float4 trail_color = float4(glow_color.rgb, glow_color.a * 0.32);
	rgb += SHIELD_BAND(curve_y + trail_offset, size.y * 0.009, trail_color);
	rgb += SHIELD_BAND(curve_y, size.y * 0.015, glow_color);
	rgb += SHIELD_BAND(curve_y, max(1.25, size.y * 0.0028), core_color);

	// During shutdown, short pieces peel away behind the front.
	if (!activating && progress > 0.12)
	{
		const float segment = floor(uv.x * 64.0);
		const float breakup = ShieldHash(segment * 17.0 + floor(elapsed * 32.0) * 29.0);
		if (breakup < 0.12 + progress * 0.10)
		{
			const float drift = size.y * (0.018 + 0.045 * ShieldHash(segment * 31.0 + floor(elapsed * 19.0)));
			const float lean = frac(uv.x * 64.0);
			const float fragment_y = curve_y - drift * lerp(1.0, 0.82, lean);
			const float4 fragment_glow = float4(glow_color.rgb, glow_color.a * 0.46 * (1.0 - progress * 0.35));
			const float4 fragment_core = float4(core_color.rgb, core_color.a * 0.52 * (1.0 - progress * 0.35));
			rgb += SHIELD_BAND(fragment_y, size.y * 0.0065, fragment_glow);
			rgb += SHIELD_BAND(fragment_y, max(0.8, size.y * 0.0015), fragment_core);
		}
	}
	#undef SHIELD_BAND
	return rgb * fade;
}

[numthreads(GROUP_X, GROUP_Y, 1)]
void PostProcessCS(COMPUTE_ARGS)
{
	int2 co = pixel_pos;
	float2 uv = (float2(co) + 0.5) / float2(g_global_cb.output_dim);

	int2 co_render = int2(float2(pixel_pos) * (float2(g_global_cb.render_dim) / float2(g_global_cb.output_dim)));

	float3 color = tex_taa_result[co].rgb;
	if ((g_global_cb.debug_flags & 0x80000000u) != 0)
		color = VREdgeAA(uv,color);

	// Adding the color overlay for picking stuff up/damaged to the final color.

	if (tweak.motion_blur_quality > 0)
	{
		// TODO(daniel): Think about per-object motion blur extending out from the object silhouette

		float2 motion = img_motion[co_render].xy;
		float  jitter = RandomSample(co, Random_MotionBlur);

		uint sample_count = (1U << (tweak.motion_blur_quality + 1));
		for (uint i = 0; i < sample_count; i++)
		{
			float  t      = ((float(i) + 1.0 - tweak.motion_blur_jitter*jitter) / float(sample_count));
			float2 offset = t*motion;

			offset = lerp(offset, offset*abs(offset), tweak.motion_blur_curve);

			color += tex_taa_result.SampleLevel(g_sampler_linear_clamp, uv + offset, 0).rgb;
		}
		color *= rcp(1 + sample_count);
	}

	float3 bloom0 = tex_bloom0.SampleLevel(g_sampler_linear_clamp, uv, 0).rgb;
	float3 bloom1 = tex_bloom1.SampleLevel(g_sampler_linear_clamp, uv, 0).rgb;
	float3 bloom2 = tex_bloom2.SampleLevel(g_sampler_linear_clamp, uv, 0).rgb;
	float3 bloom3 = tex_bloom3.SampleLevel(g_sampler_linear_clamp, uv, 0).rgb;
	float3 bloom4 = tex_bloom4.SampleLevel(g_sampler_linear_clamp, uv, 0).rgb;
	float3 bloom5 = tex_bloom5.SampleLevel(g_sampler_linear_clamp, uv, 0).rgb;
	float3 bloom6 = tex_bloom6.SampleLevel(g_sampler_linear_clamp, uv, 0).rgb;
	float3 bloom7 = tex_bloom7.SampleLevel(g_sampler_linear_clamp, uv, 0).rgb;
	float3 bloom = (bloom0 + bloom1 + bloom2 + bloom3 + bloom4 + bloom5 + bloom6 + bloom7)*(1.0 / 8.0);

	float aspect = float(g_global_cb.render_dim.y) / float(g_global_cb.render_dim.x);

	float2 uv_sq = 2.0*uv - 1.0;
	uv_sq.y *= aspect;
	uv_sq *= tweak.vignette_scale;

	float vignette = dot(uv_sq, uv_sq);
	//vignette *= vignette;

	vignette = lerp(1, saturate(1.0 - vignette), tweak.vignette_strength);
	color *= vignette;

	switch (tweak.bloom_blend_mode)
	{
		case 0: // Lerp
		{
			color = lerp(color, bloom, tweak.bloom_amount);
		} break;

		case 1: // Additive
		{
			color += tweak.bloom_amount*bloom;
		} break;

		case 2: // Subtractive
		{
			color -= max(0, tweak.bloom_amount*(bloom - color));
		} break;
	}

	//------------------------------------------------------------------------
	// Shield bubbles on other players and the first-person shield sweep. The
	// foreground cockpit always hides the world behind it, bubbles included.

	uint shield_instance_idx = img_visibility_prim[co_render].x;
	bool shield_cockpit_pixel = shield_instance_idx != ~0 &&
		(g_instance_data_buffer[shield_instance_idx].instance_mask & RT_RENDER_MASK_COCKPIT) != 0;
	[branch] if (g_shield_effects.bubble_count > 0 && !shield_cockpit_pixel)
	{
		float2 ray_uv = uv;
		ray_uv.y = 1.0 - ray_uv.y;
		ray_uv.y -= g_global_cb.viewport_offset_y;
		float3 view_d = Unproject(g_global_cb.proj_inv, ray_uv, 1);
		float3 ray_dir = normalize(mul(g_global_cb.view_inv, float4(view_d, 0)).xyz);
		float3 ray_origin = mul(g_global_cb.view_inv, float4(0, 0, 0, 1)).xyz;
		color += ShieldBubblesAlongRay(ray_origin, ray_dir, img_depth[co_render].r);
	}
	[branch] if (g_shield_effects.sweep_progress >= 0.0 && !shield_cockpit_pixel)
	{
		color += ShieldSweep(uv, float2(g_global_cb.output_dim));
	}

	//------------------------------------------------------------------------
	// Tonemap and sRGB

	color *= exp2(tweak.exposure);

	color = ColorPreservingTonemap(color);
	color = LinearTosRGB(color);

	// NOTE(daniel): Moved up here from where it was before, because it was being applied
	// over top the debug display which isn't helpful. Adding it after tonemap/srgb conversion
	// is maybe questionable but it probably matches the game's original rendering better
	// Match the classic 2D cockpit behavior: palette flashes tint the world seen
	// through the windows, but the foreground cockpit overlay itself stays at its
	// normally lit color.
	uint overlay_instance_idx = img_visibility_prim[co_render].x;
	bool foreground_cockpit_pixel = overlay_instance_idx != ~0 &&
		(g_instance_data_buffer[overlay_instance_idx].instance_mask & RT_RENDER_MASK_COCKPIT) != 0;
	if (!foreground_cockpit_pixel)
	{
		color += g_global_cb.screen_color_overlay.xyz;
	}

	//------------------------------------------------------------------------
	// Debug texture view

	float3 debug_color = color;
	float debug_blend_factor = tweak.debug_render_blend_factor;

	switch (g_global_cb.debug_render_mode)
	{
		case RT_DebugRenderMode_Debug:
		{
			debug_color = img_debug[co_render].xyz;
			debug_blend_factor = img_debug[co_render].w;
		} break;

		case RT_DebugRenderMode_Normals:
		{
			float3 normal = DecodeNormalOctahedron(img_normal[co_render].xy);
			debug_color = 0.5*normal + 0.5;
		} break;

		case RT_DebugRenderMode_Depth:
		{
			float depth = img_depth[co_render].r;
			debug_color = depth / 1000.0f; // TODO: Specify depth scale
		} break;

		case RT_DebugRenderMode_Albedo:
		{
			debug_color = img_albedo[co_render].rgb;
		} break;

		case RT_DebugRenderMode_Emissive:
		{
			debug_color = img_emissive[co_render].rgb;
		} break;

		case RT_DebugRenderMode_Diffuse:
		{
			float3 direct = tweak.svgf_stabilize ? img_diff_stable[co_render].rgb : img_diff_denoise_ping[co_render].rgb;
			debug_color = direct.rgb;
		} break;

		case RT_DebugRenderMode_Specular:
		{
			float3 spec = tweak.svgf_stabilize ? img_spec_stable[co_render].rgb : img_spec_denoise_ping[co_render].rgb;
			debug_color = spec;
		} break;

		case RT_DebugRenderMode_Motion:
		{
			float4 motion = img_motion[co_render];
			debug_color = float3(abs(motion.xy) * 100.0, 0);
		} break;

		case RT_DebugRenderMode_MetallicRoughness:
		{
			float metallic = img_metallic[co_render].x;
			float roughness = img_roughness[co_render].x;
			debug_color = float3(metallic, roughness, 0);
		} break;

		case RT_DebugRenderMode_HistoryLength:
		{
			float2 history_length = img_history_length[co_render];

			float2 hist_max = 
			{
				tweak.svgf_max_hist_len_diff,
				tweak.svgf_max_hist_len_spec,
			};

			debug_color = float3(history_length / hist_max, 0);
		} break;

		case RT_DebugRenderMode_Materials:
		{
			static const float3 colors[] = {
				{ 1, 0, 0 },
				{ 0, 1, 0 },
				{ 0, 0, 1 },
				{ 1, 1, 0 },
				{ 1, 0, 1 },
				{ 0, 1, 1 },
			};

			uint  material_index = img_material[co_render].x;
			debug_color = colors[material_index % 6];

			if ((material_index / 6) % 2 == 1)
			{
				debug_color *= 0.5;
			}
		} break;

		case RT_DebugRenderMode_FirstMoment:
		{
			float4 moments = img_moments[co_render];
			debug_color = moments.x;
		} break;

		case RT_DebugRenderMode_SecondMoment:
		{
			float4 moments = img_moments[co_render];
			debug_color = moments.y;
		} break;

		case RT_DebugRenderMode_Variance:
		{
			float4 moments = img_moments[co_render];
			debug_color = sqrt(max(0.001, moments.y - square(moments.x)));
		} break;

		case RT_DebugRenderMode_Bloom0:
		{
			debug_color = bloom0;
		} break;

		case RT_DebugRenderMode_Bloom1:
		{
			debug_color = bloom1;
		} break;

		case RT_DebugRenderMode_Bloom2:
		{
			debug_color = bloom2;
		} break;

		case RT_DebugRenderMode_Bloom3:
		{
			debug_color = bloom3;
		} break;

		case RT_DebugRenderMode_Bloom4:
		{
			debug_color = bloom4;
		} break;

		case RT_DebugRenderMode_Bloom5:
		{
			debug_color = bloom5;
		} break;

		case RT_DebugRenderMode_Bloom6:
		{
			debug_color = bloom6;
		} break;

		case RT_DebugRenderMode_Bloom7:
		{
			debug_color = bloom7;
		} break;

		case RT_DebugRenderMode_Fsr2Reactive:
		{
			debug_color = img_fsr2_reactive_mask[co_render].r;
		} break;
	}

	float3 final_color = lerp(color, debug_color, debug_blend_factor);
	img_postfx[co] = float4(final_color, 1.0);
}

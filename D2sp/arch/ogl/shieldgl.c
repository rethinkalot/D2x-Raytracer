/*
 * Shield effects for D2s (OpenGL): the same bubbles and first-person sweep the
 * D2X Raytracer draws (post_process.hlsl ShadeShieldBubble / ShieldSweep),
 * ported to GLSL. shieldfx.c decides what to show; this draws it.
 *
 *  - Bubbles: a sphere mesh per bubble, both faces, additive, hidden behind
 *    walls by the depth buffer (your own bubble ignores depth, as in the
 *    raytracer). The shading runs per pixel on the world-space surface point.
 *  - Sweep: a full-screen pass over the finished view.
 * The raytracer's colours are HDR (tonemapped afterwards); here they are
 * rolled off with 1 - exp(-c) so bright hits saturate instead of clipping.
 */

#include <math.h>
#include <string.h>
#include "ogl_init.h"
#include "oglprog.h"
#include "internal.h"
#include "3d.h"
#include "gr.h"
#include "dxxerror.h"
#include "../../main/shieldfx_gl.h"

extern vms_vector Viewer_eye; // render.c, valid during render

static RT_ShieldEffects Shield_fx;
static GLuint Bubble_prog, Sweep_prog;
static GLint  Bubble_umat, Bubble_ushield, Bubble_uimpact, Bubble_udir, Bubble_up0, Bubble_up1, Bubble_utime;
static GLint  Sweep_usize, Sweep_up0, Sweep_up1;

#define SHIELD_APOS    0
#define SHIELD_ANORMAL 1
#define SHIELD_AVIEW   2

// ------------------------------------------------------------------------------------------------
// GLSL (1.10, like the game's other programs)

static const char *Bubble_vs =
	"attribute vec3 apos;\n"
	"attribute vec3 anormal;\n"
	"attribute vec3 aview;\n"
	"uniform mat4 umat;\n"
	"varying vec3 vlocal;\n"
	"varying vec3 vview;\n"
	"void main() {\n"
	"  gl_Position = umat * vec4(apos, 1.0);\n"
	"  vlocal = anormal; vview = aview;\n"
	"}\n";

static const char *Bubble_fs =
	"varying vec3 vlocal;\n"
	"varying vec3 vview;\n"
	"uniform vec4 ushield;  // rgb shield colour, w visibility\n"
	"uniform vec4 uimpact;  // rgb impact colour, w impact strength\n"
	"uniform vec4 udir;     // xyz impact direction, w impact fade\n"
	"uniform vec4 up0;      // damage, invulnerable, shutdown fade, expiry warning\n"
	"uniform vec4 up1;      // activation fade, destabilizing, self bubble, charging\n"
	"uniform float utime;\n"
	"float sat(float v) { return clamp(v, 0.0, 1.0); }\n"
	"float flowf(vec3 l, float t) {\n"
	"  float a = sin(dot(l, vec3(4.1, 2.7, 3.6)) * 2.15 + t * 1.35);\n"
	"  float b = sin(dot(l, vec3(-2.4, 4.8, 3.1)) * 2.55 - t * 1.05);\n"
	"  float c = sin(dot(l, vec3(3.3, -2.1, 5.2)) * 1.85 + t * 0.72);\n"
	"  return smoothstep(0.34, 0.88, 0.5 + 0.5 * (a * 0.48 + b * 0.32 + c * 0.20));\n"
	"}\n"
	"float arcf(float d, float gate, float flicker) {\n"
	"  return (exp(-abs(d) * 72.0) + exp(-abs(d) * 19.0) * 0.24) * gate * flicker;\n"
	"}\n"
	"vec3 rx(vec3 p, float a) { float s = sin(a), c = cos(a); return vec3(p.x, p.y * c - p.z * s, p.y * s + p.z * c); }\n"
	"vec3 ry(vec3 p, float a) { float s = sin(a), c = cos(a); return vec3(p.x * c + p.z * s, p.y, -p.x * s + p.z * c); }\n"
	"vec3 rz(vec3 p, float a) { float s = sin(a), c = cos(a); return vec3(p.x * c - p.y * s, p.x * s + p.y * c, p.z); }\n"
	"void main() {\n"
	"  vec3 local = normalize(vlocal);\n"
	"  vec3 view = normalize(vview);\n"
	"  float time = utime;\n"
	"  vec3 shield_color = ushield.rgb;\n"
	"  float visibility = ushield.w;\n"
	"  float self_bubble = up1.z;\n"
	"  vec3 impact_color = mix(uimpact.rgb, ushield.rgb * 1.05, 0.8 * self_bubble);\n"
	"  float impact_strength = uimpact.w, impact_fade = udir.w;\n"
	"  float damage = up0.x, invulnerable = up0.y, shutdown_fade = up0.z, expiry_warning = up0.w;\n"
	"  float activation_fade = up1.x, destabilizing = up1.y, charging = up1.w;\n"
	"  float base_glow = 1.0 - self_bubble;\n"
	"  float self_cue = mix(1.0, 1.7, self_bubble);\n"
	"  float fresnel = pow(sat(1.0 - abs(dot(local, view))), 2.65);\n"
	"  vec3 hit_dir = udir.xyz;\n"
	"  float hit_facing = dot(local, hit_dir * inversesqrt(max(dot(hit_dir, hit_dir), 1e-8)));\n"
	"  float halo_start = mix(0.57, 0.74, self_bubble);\n"
	"  float core_start = mix(0.91, 0.965, self_bubble);\n"
	"  float halo = pow(sat((hit_facing - halo_start) / (1.0 - halo_start)), 2.15);\n"
	"  float core = pow(sat((hit_facing - core_start) / (1.0 - core_start)), 2.4) * mix(1.0, 0.45, self_bubble);\n"
	"  float impact = halo * impact_strength * impact_fade;\n"
	"  float flow = flowf(local, time);\n"
	"  float shimmer = 0.84 + 0.16 * sin(time * 7.0 + dot(local, vec3(9.1, 13.7, 17.3)));\n"
	"  float arcs = 0.0, warning_stutter = 0.0, warning_flash = 0.0;\n"
	"  if (invulnerable > 0.0 || destabilizing > 0.0) {\n"
	"    float flicker_a = sat(0.52 + 0.31 * sin(time * 13.7 + local.z * 19.0) + 0.24 * sin(time * 31.1 - local.x * 11.0));\n"
	"    float flicker_b = sat(0.48 + 0.34 * sin(time * 17.3 - local.x * 23.0) + 0.27 * sin(time * 27.7 + local.y * 15.0));\n"
	"    float burst_a = smoothstep(0.24, 0.68, 0.5 + 0.31 * sin(time * 2.17) + 0.19 * sin(time * 5.83 + 1.7));\n"
	"    float burst_b = smoothstep(0.30, 0.72, 0.5 + 0.28 * sin(time * 1.73 + 2.4) + 0.22 * sin(time * 7.11));\n"
	"    float burst_c = smoothstep(0.34, 0.76, 0.5 + 0.33 * sin(time * 2.63 + 4.1) + 0.17 * sin(time * 8.37 + 0.8));\n"
	"    float burst_d = smoothstep(0.28, 0.70, 0.5 + 0.26 * sin(time * 1.31 + 5.0) + 0.25 * sin(time * 6.49 + 2.1));\n"
	"    float jitter_a = sin(time * 19.0 + sin(time * 3.7) * 4.0) * 0.018;\n"
	"    float jitter_b = sin(time * 23.0 + sin(time * 4.3 + 1.0) * 3.0) * 0.015;\n"
	"    vec3 p_a = rz(ry(local, time * 0.91 + sin(time * 0.37) * 1.8), sin(time * 0.53 + 1.2) * 1.5);\n"
	"    vec3 p_b = rx(rz(local, -time * 0.73 + sin(time * 0.41) * 2.1), sin(time * 0.67 + 3.0) * 1.7);\n"
	"    vec3 p_c = ry(rx(local, time * 1.07 + sin(time * 0.29) * 2.4), sin(time * 0.47 + 4.4) * 1.9);\n"
	"    float path_a = p_a.y - (sin(p_a.x * 4.8 + time * 1.35) * 0.22 + sin(p_a.z * 10.0 - time * 2.1) * 0.055 + jitter_a);\n"
	"    float path_b = p_b.z - (sin(p_b.y * 5.3 - time * 1.05) * 0.19 + sin(p_b.x * 12.0 + time * 2.7) * 0.045 + jitter_b);\n"
	"    float path_branch = p_a.y - (sin(p_a.x * 6.5 + time * 1.35) * 0.12 + 0.34);\n"
	"    float path_c = p_c.x - (sin(p_c.z * 5.8 + time * 1.72) * 0.20 - sin(p_c.y * 13.0 - time * 2.4) * 0.040);\n"
	"    float path_d = p_b.z - (sin(p_b.x * 7.2 - time * 1.48) * 0.13 - 0.31);\n"
	"    float gate_a = smoothstep(-0.72, -0.30, p_a.x) * (1.0 - smoothstep(0.48, 0.82, p_a.x));\n"
	"    float gate_b = smoothstep(-0.68, -0.20, p_b.y) * (1.0 - smoothstep(0.36, 0.76, p_b.y));\n"
	"    float branch_gate = smoothstep(0.05, 0.28, p_a.x) * (1.0 - smoothstep(0.55, 0.82, p_a.x));\n"
	"    float gate_c = smoothstep(-0.78, -0.42, p_c.z) * (1.0 - smoothstep(0.18, 0.58, p_c.z));\n"
	"    float gate_d = smoothstep(-0.58, -0.18, p_b.y) * (1.0 - smoothstep(0.55, 0.86, p_b.y));\n"
	"    arcs = arcf(path_a, gate_a, flicker_a * burst_a) + arcf(path_b, gate_b, flicker_b * burst_b)\n"
	"         + arcf(path_branch, branch_gate, flicker_a * flicker_b * burst_c) * 0.72\n"
	"         + arcf(path_c, gate_c, flicker_b * burst_c) * 0.82 + arcf(path_d, gate_d, flicker_a * burst_d) * 0.74;\n"
	"    warning_stutter = smoothstep(0.05, 0.55, 0.5 + 0.5 * sin(time * 18.0 + sin(time * 6.7) * 2.8));\n"
	"    warning_flash = pow(sat(0.5 + 0.5 * sin(time * 10.5)), 5.0);\n"
	"    arcs *= 1.0 + expiry_warning * (1.25 + warning_stutter * 1.65);\n"
	"  }\n"
	"  float invul_pulse = 0.82 + 0.18 * sin(time * 3.4);\n"
	"  vec3 rgb = shield_color * (0.07 + fresnel * 0.52) * base_glow;\n"
	"  rgb += shield_color * flow * ((0.018 + fresnel * 0.055) * base_glow + impact * 0.13) * shimmer;\n"
	"  rgb += impact_color * impact * (0.55 + flow * 0.20) * self_cue;\n"
	"  rgb += mix(vec3(2.8, 2.8, 3.0), vec3(1.2, 1.6, 3.0), self_bubble) * core * impact_strength * impact_fade * 0.38 * self_cue;\n"
	"  rgb += shield_color * flow * damage * 0.035 * base_glow;\n"
	"  rgb += shield_color * invulnerable * ((0.018 + fresnel * 0.48) * base_glow + arcs * mix(0.72, 0.55, self_bubble)) * invul_pulse;\n"
	"  rgb += impact_color * invulnerable * arcs * (0.22 + impact * 0.62);\n"
	"  rgb += impact_color * invulnerable * impact * (0.34 + arcs * 0.75 + core * 0.45);\n"
	"  rgb += shield_color * invulnerable * expiry_warning * (fresnel * (0.30 + warning_flash * 0.75) + arcs * 0.24);\n"
	"  rgb += shield_color * destabilizing * (0.025 + fresnel * (0.20 + warning_flash * 0.30) + arcs * 0.58);\n"
	"  rgb += vec3(0.72, 0.92, 2.8) * destabilizing * arcs * 0.24;\n"
	"  float alpha = sat(0.30 * base_glow + 0.25 * self_bubble + fresnel * 0.30 + impact * 0.28 + core * 0.18 + invulnerable * (0.018 + arcs * 0.24 + fresnel * 0.16));\n"
	"  alpha += destabilizing * (0.035 + fresnel * 0.12 + arcs * 0.12);\n"
	"  if (charging > 0.0) {\n"
	"    float rise = sat(local.y * 0.5 + 0.5);\n"
	"    float swirl = sin(local.x * 7.0 + local.z * 5.0 + time * 3.1) * 0.06;\n"
	"    float phase = fract(local.y * 1.35 - time * 2.3 + swirl);\n"
	"    float band = pow(sat(1.0 - abs(phase - 0.5) * 4.5), 2.5);\n"
	"    float throb = 0.75 + 0.25 * sin(time * 11.0);\n"
	"    vec3 energy_color = mix(vec3(2.6, 1.7, 0.35), shield_color * 1.4, rise);\n"
	"    rgb += energy_color * band * 0.55 * charging;\n"
	"    rgb += shield_color * flow * (0.10 + fresnel * 0.20) * throb * charging;\n"
	"    alpha += charging * (0.10 + band * 0.30);\n"
	"  }\n"
	"  if (activation_fade < 1.0) {\n"
	"    float progress = smoothstep(0.0, 1.0, activation_fade);\n"
	"    float ripple = sin(local.x * 15.0 + local.z * 11.0 + time * 10.0) * 0.055;\n"
	"    float boundary = mix(1.16, -1.16, progress) + ripple;\n"
	"    float reveal = smoothstep(boundary - 0.075, boundary + 0.075, local.y);\n"
	"    float charge_edge = exp(-abs(local.y - boundary) * 22.0);\n"
	"    vec3 charge_color = mix(shield_color * 1.55, vec3(3.2, 1.5, 0.28), invulnerable);\n"
	"    rgb += charge_color * charge_edge * 0.62 * self_cue;\n"
	"    rgb *= reveal; alpha *= reveal;\n"
	"  }\n"
	"  if (shutdown_fade > 0.0) {\n"
	"    float progress = smoothstep(0.0, 1.0, shutdown_fade);\n"
	"    float ripple = sin(local.x * 15.0 + local.z * 11.0 + time * 9.0) * 0.055;\n"
	"    float boundary = mix(1.16, -1.16, progress) + ripple;\n"
	"    float remaining = 1.0 - smoothstep(boundary - 0.075, boundary + 0.075, local.y);\n"
	"    float discharge_edge = exp(-abs(local.y - boundary) * 24.0) * sat(shutdown_fade * 8.0);\n"
	"    float flicker = mix(0.42, 1.0, smoothstep(0.15, 0.70, 0.5 + 0.5 * sin(time * 24.0)));\n"
	"    vec3 discharge_color = mix(shield_color * 1.45, vec3(3.0, 1.35, 0.24), invulnerable);\n"
	"    rgb += discharge_color * discharge_edge * 0.46 * self_cue;\n"
	"    rgb *= remaining * flicker; alpha *= remaining * flicker;\n"
	"  }\n"
	"  float warning_power = mix(1.0, mix(0.48, 1.28, warning_stutter), expiry_warning);\n"
	"  float critical_power = mix(1.0, mix(0.58, 1.30, warning_stutter), destabilizing);\n"
	"  float scale = visibility * warning_power * critical_power * mix(1.0, 0.85, self_bubble);\n"
	"  vec3 c = rgb * scale * sat(alpha * mix(scale, 1.0, self_bubble));\n"
	"  gl_FragColor = vec4(vec3(1.0) - exp(-max(c, vec3(0.0))), 1.0);\n"
	"}\n";

static const char *Sweep_vs =
	"attribute vec3 apos;\n"
	"void main() { gl_Position = vec4(apos.xy * 2.0 - 1.0, 0.0, 1.0); }\n";

static const char *Sweep_fs =
	"uniform vec2 usize;\n"
	"uniform vec4 up0; // progress, fade, invulnerable, activating\n"
	"uniform vec4 up1; // elapsed, time\n"
	"float hashf(float v) { return mod(sin(v * 12.9898) * 43758.5453, 1.0) * 0.5 + 0.5; }\n"
	"vec3 band(float y, float center, float half_width, vec4 color) {\n"
	"  return color.rgb * color.a * (1.0 - smoothstep(0.55, 1.0, abs(y - center) / max(half_width, 0.5)));\n"
	"}\n"
	"void main() {\n"
	"  float progress = up0.x, fade = up0.y;\n"
	"  bool invulnerable = up0.z > 0.5, activating = up0.w > 0.5;\n"
	"  float elapsed = up1.x, time = up1.y;\n"
	"  vec2 uv = vec2(gl_FragCoord.x / usize.x, 1.0 - gl_FragCoord.y / usize.y);\n"
	"  vec4 field_color = invulnerable ? vec4(1.45, 0.58, 0.10, 0.006) : vec4(0.15, 0.31, 1.35, 0.005);\n"
	"  vec4 glow_color  = invulnerable ? vec4(2.5, 1.08, 0.20, 0.17)   : vec4(0.28, 0.58, 2.35, 0.15);\n"
	"  vec4 core_color  = invulnerable ? vec4(3.0, 1.85, 0.68, 0.38)   : vec4(0.85, 1.16, 2.8, 0.34);\n"
	"  float y = uv.y * usize.y;\n"
	"  float nx = uv.x * 2.0 - 1.0;\n"
	"  float base_y = mix(-usize.y * 0.16, usize.y * 1.10, progress);\n"
	"  float curve_y = base_y + sin(nx * 11.0 + time * 12.0) * usize.y * 0.007 + nx * nx * usize.y * 0.14;\n"
	"  vec3 rgb = field_color.rgb * field_color.a;\n"
	"  float trail_offset = usize.y * (activating ? -0.050 : 0.050);\n"
	"  rgb += band(y, curve_y + trail_offset, usize.y * 0.009, vec4(glow_color.rgb, glow_color.a * 0.32));\n"
	"  rgb += band(y, curve_y, usize.y * 0.015, glow_color);\n"
	"  rgb += band(y, curve_y, max(1.25, usize.y * 0.0028), core_color);\n"
	"  if (!activating && progress > 0.12) {\n"
	"    float segment = floor(uv.x * 64.0);\n"
	"    float breakup = hashf(segment * 17.0 + floor(elapsed * 32.0) * 29.0);\n"
	"    if (breakup < 0.12 + progress * 0.10) {\n"
	"      float drift = usize.y * (0.018 + 0.045 * hashf(segment * 31.0 + floor(elapsed * 19.0)));\n"
	"      float lean = fract(uv.x * 64.0);\n"
	"      float fragment_y = curve_y - drift * mix(1.0, 0.82, lean);\n"
	"      rgb += band(y, fragment_y, usize.y * 0.0065, vec4(glow_color.rgb, glow_color.a * 0.46 * (1.0 - progress * 0.35)));\n"
	"      rgb += band(y, fragment_y, max(0.8, usize.y * 0.0015), vec4(core_color.rgb, core_color.a * 0.52 * (1.0 - progress * 0.35)));\n"
	"    }\n"
	"  }\n"
	"  vec3 c = rgb * fade;\n"
	"  gl_FragColor = vec4(vec3(1.0) - exp(-max(c, vec3(0.0))), 1.0);\n"
	"}\n";

// ------------------------------------------------------------------------------------------------

static GLuint shield_mk_prog(const char *vs_src, const char *fs_src)
{
	char msg[2048];
	GLint ok = 0;
	GLuint vs = glCreateShader(GL_VERTEX_SHADER), fs = glCreateShader(GL_FRAGMENT_SHADER), prog;
	glShaderSource(vs, 1, &vs_src, NULL);
	glCompileShader(vs);
	glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
	if (!ok) { glGetShaderInfoLog(vs, sizeof(msg), NULL, msg); Warning("Shield effects: vertex shader: %s", msg); return 0; }
	glShaderSource(fs, 1, &fs_src, NULL);
	glCompileShader(fs);
	glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
	if (!ok) { glGetShaderInfoLog(fs, sizeof(msg), NULL, msg); Warning("Shield effects: fragment shader: %s", msg); return 0; }
	prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glBindAttribLocation(prog, SHIELD_APOS, "apos");
	glBindAttribLocation(prog, SHIELD_ANORMAL, "anormal");
	glBindAttribLocation(prog, SHIELD_AVIEW, "aview");
	glLinkProgram(prog);
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	glDeleteShader(vs);
	glDeleteShader(fs);
	if (!ok) { glGetProgramInfoLog(prog, sizeof(msg), NULL, msg); Warning("Shield effects: link: %s", msg); glDeleteProgram(prog); return 0; }
	return prog;
}

// A shader problem only turns the effects off (Warning), it never stops the game.
void ogl_init_shield_prog(void)
{
	Bubble_prog = shield_mk_prog(Bubble_vs, Bubble_fs);
	if (Bubble_prog)
	{
		Bubble_umat    = glGetUniformLocation(Bubble_prog, "umat");
		Bubble_ushield = glGetUniformLocation(Bubble_prog, "ushield");
		Bubble_uimpact = glGetUniformLocation(Bubble_prog, "uimpact");
		Bubble_udir    = glGetUniformLocation(Bubble_prog, "udir");
		Bubble_up0     = glGetUniformLocation(Bubble_prog, "up0");
		Bubble_up1     = glGetUniformLocation(Bubble_prog, "up1");
		Bubble_utime   = glGetUniformLocation(Bubble_prog, "utime");
	}
	Sweep_prog = shield_mk_prog(Sweep_vs, Sweep_fs);
	if (Sweep_prog)
	{
		Sweep_usize = glGetUniformLocation(Sweep_prog, "usize");
		Sweep_up0   = glGetUniformLocation(Sweep_prog, "up0");
		Sweep_up1   = glGetUniformLocation(Sweep_prog, "up1");
	}
}

void ogl_done_shield_prog(void)
{
	if (Bubble_prog) glDeleteProgram(Bubble_prog);
	if (Sweep_prog) glDeleteProgram(Sweep_prog);
	Bubble_prog = Sweep_prog = 0;
}

void ogl_set_shield_effects(const RT_ShieldEffects *effects)
{
	Shield_fx = *effects;
}

// ------------------------------------------------------------------------------------------------
// Bubbles

#define SPHERE_RINGS   16
#define SPHERE_SEGS    32
#define SPHERE_VERTS   (SPHERE_RINGS * SPHERE_SEGS * 6)

static float Unit_sphere[SPHERE_VERTS][3];
static int   Unit_sphere_ready;

static void build_unit_sphere(void)
{
	int r, s, n = 0;
	for (r = 0; r < SPHERE_RINGS; r++)
		for (s = 0; s < SPHERE_SEGS; s++)
		{
			const int corner[6][2] = { {0,0}, {1,0}, {1,1}, {0,0}, {1,1}, {0,1} };
			int k;
			for (k = 0; k < 6; k++)
			{
				const float th = (float)(r + corner[k][0]) / SPHERE_RINGS * 3.14159265f;
				const float ph = (float)(s + corner[k][1]) / SPHERE_SEGS * 6.28318531f;
				Unit_sphere[n][0] = sinf(th) * cosf(ph);
				Unit_sphere[n][1] = cosf(th);
				Unit_sphere[n][2] = sinf(th) * sinf(ph);
				n++;
			}
		}
	Unit_sphere_ready = 1;
}

void ogl_draw_shield_bubbles(void)
{
	static GLfloat pos[SPHERE_VERTS][3], nrm[SPHERE_VERTS][3], view[SPHERE_VERTS][3];
	GLfloat mat[16];
	GLboolean depth_test;
	unsigned b;
	int i;

	if (!Bubble_prog || Shield_fx.bubble_count == 0)
		return;
	if (!Unit_sphere_ready)
		build_unit_sphere();

	glGetFloatv(GL_PROJECTION_MATRIX, mat);
	depth_test = glIsEnabled(GL_DEPTH_TEST);
	OGL_DISABLE(TEXTURE_2D);
	glDisable(GL_CULL_FACE);          // both faces, as in the raytracer
	glDepthMask(GL_FALSE);
	glBlendFunc(GL_ONE, GL_ONE);      // additive

	glUseProgram(Bubble_prog);
	glUniformMatrix4fv(Bubble_umat, 1, GL_FALSE, mat);
	glUniform1f(Bubble_utime, Shield_fx.time);
	glEnableVertexAttribArray(SHIELD_APOS);
	glEnableVertexAttribArray(SHIELD_ANORMAL);
	glEnableVertexAttribArray(SHIELD_AVIEW);
	glVertexAttribPointer(SHIELD_APOS, 3, GL_FLOAT, GL_FALSE, 0, pos);
	glVertexAttribPointer(SHIELD_ANORMAL, 3, GL_FLOAT, GL_FALSE, 0, nrm);
	glVertexAttribPointer(SHIELD_AVIEW, 3, GL_FLOAT, GL_FALSE, 0, view);

	for (b = 0; b < Shield_fx.bubble_count && b < RT_MAX_SHIELD_BUBBLES; b++)
	{
		const RT_ShieldBubble *s = &Shield_fx.bubbles[b];
		const float ex = f2fl(Viewer_eye.x), ey = f2fl(Viewer_eye.y), ez = f2fl(Viewer_eye.z);

		for (i = 0; i < SPHERE_VERTS; i++)
		{
			const float wx = s->center.x + Unit_sphere[i][0] * s->radius;
			const float wy = s->center.y + Unit_sphere[i][1] * s->radius;
			const float wz = s->center.z + Unit_sphere[i][2] * s->radius;
			float vx = ex - wx, vy = ey - wy, vz = ez - wz;
			const float len = sqrtf(vx * vx + vy * vy + vz * vz);
			vms_vector world;
			g3s_point p;
			world.x = fl2f(wx); world.y = fl2f(wy); world.z = fl2f(wz);
			g3_rotate_point(&p, &world);
			pos[i][0] = f2fl(p.p3_vec.x);
			pos[i][1] = f2fl(p.p3_vec.y);
			pos[i][2] = -f2fl(p.p3_vec.z);
			nrm[i][0] = Unit_sphere[i][0]; nrm[i][1] = Unit_sphere[i][1]; nrm[i][2] = Unit_sphere[i][2];
			if (len > 1e-4f) { vx /= len; vy /= len; vz /= len; }
			view[i][0] = vx; view[i][1] = vy; view[i][2] = vz;
		}

		glUniform4f(Bubble_ushield, s->shield_color.x, s->shield_color.y, s->shield_color.z, s->visibility);
		glUniform4f(Bubble_uimpact, s->impact_color.x, s->impact_color.y, s->impact_color.z, s->impact_strength);
		glUniform4f(Bubble_udir, s->impact_direction.x, s->impact_direction.y, s->impact_direction.z, s->impact_fade);
		glUniform4f(Bubble_up0, s->damage, s->invulnerable, s->shutdown_fade, s->expiry_warning);
		glUniform4f(Bubble_up1, s->activation_fade, s->destabilizing, s->self_bubble, s->charging);

		// Your own bubble surrounds the camera: nothing can be in front of it.
		if (s->self_bubble > 0.5f) glDisable(GL_DEPTH_TEST);
		else if (depth_test) glEnable(GL_DEPTH_TEST);
		glDrawArrays(GL_TRIANGLES, 0, SPHERE_VERTS);
	}

	glDisableVertexAttribArray(SHIELD_APOS);
	glDisableVertexAttribArray(SHIELD_ANORMAL);
	glDisableVertexAttribArray(SHIELD_AVIEW);
	glUseProgram(0);
	if (depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	glEnable(GL_CULL_FACE);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

// ------------------------------------------------------------------------------------------------
// First-person sweep (shields collapse / come back, invulnerability starts / ends)

void ogl_draw_shield_sweep(void)
{
	static const GLfloat quad[6][3] = { {0,0,0}, {1,0,0}, {1,1,0}, {0,0,0}, {1,1,0}, {0,1,0} };
	GLboolean depth_test;

	if (!Sweep_prog || Shield_fx.sweep_progress < 0.0f || !grd_curscreen)
		return;

	depth_test = glIsEnabled(GL_DEPTH_TEST);
	glDisable(GL_DEPTH_TEST);
	OGL_DISABLE(TEXTURE_2D);
	glBlendFunc(GL_ONE, GL_ONE);
	glUseProgram(Sweep_prog);
	glUniform2f(Sweep_usize, (float)grd_curscreen->sc_w, (float)grd_curscreen->sc_h);
	glUniform4f(Sweep_up0, Shield_fx.sweep_progress, Shield_fx.sweep_fade, Shield_fx.sweep_invulnerable, Shield_fx.sweep_activating);
	glUniform4f(Sweep_up1, Shield_fx.sweep_elapsed, Shield_fx.time, 0.0f, 0.0f);
	glEnableVertexAttribArray(SHIELD_APOS);
	glVertexAttribPointer(SHIELD_APOS, 3, GL_FLOAT, GL_FALSE, 0, quad);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glDisableVertexAttribArray(SHIELD_APOS);
	glUseProgram(0);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	if (depth_test) glEnable(GL_DEPTH_TEST);
}

/*
 * D2s (OpenGL) side of the shield effects. shieldfx.c is shared with the
 * D2X Raytracer, which hands its bubbles to the raytracer through
 * RT_SetShieldEffects(); here the same structures go to the OpenGL renderer
 * (arch/ogl/shieldgl.c). Field for field the same as the raytracer's
 * RT_ShieldBubble / RT_ShieldEffects (Renderer.h), so shieldfx.c builds
 * unchanged in both games.
 */

#ifndef _SHIELDFX_GL_H
#define _SHIELDFX_GL_H

#include <stdint.h>
#include "vecmat.h"
#include "maths.h"

typedef struct RT_Vec3 { float x, y, z; } RT_Vec3;

static inline RT_Vec3 RT_Vec3Make(float x, float y, float z) { RT_Vec3 v = { x, y, z }; return v; }
static inline RT_Vec3 RT_Vec3Fromvms_vector(const vms_vector *v) { return RT_Vec3Make(f2fl(v->x), f2fl(v->y), f2fl(v->z)); }
static inline float RT_VRObjectScale(void) { return 1.0f; } // no VR in D2s

#define RT_MAX_SHIELD_BUBBLES 8
typedef struct RT_ShieldBubble
{
	RT_Vec3 center;           float radius;          // world units
	RT_Vec3 shield_color;     float visibility;      // HDR colour, 0..1
	RT_Vec3 impact_color;     float impact_strength;
	RT_Vec3 impact_direction; float impact_fade;     // unit vector from the centre
	float damage;             // 1 - shield fraction
	float invulnerable;       // 1 for the gold invulnerability layer
	float shutdown_fade;      // 0..1 discharge sweep (collapse or invulnerability end)
	float expiry_warning;     // 0..1 over the last 4 s of invulnerability
	float activation_fade;    // 0..1 charge-up sweep; 1 = fully on
	float destabilizing;      // 1 when shields are critical
	float self_bubble;        // 1 = your own ship seen from inside
	float charging;           // 0..1 energy-to-shield transfer in progress
} RT_ShieldBubble;

typedef struct RT_ShieldEffects
{
	uint32_t bubble_count;
	float time;               // seconds, drives the animation
	float sweep_progress;     // first-person sweep 0..1; < 0 = none
	float sweep_fade;
	float sweep_invulnerable; // 1 = gold invulnerability colours
	float sweep_activating;   // 1 = power-up (no peeling fragments)
	float sweep_elapsed;      // seconds since the sweep started
	float sweep_pad;
	RT_ShieldBubble bubbles[RT_MAX_SHIELD_BUBBLES];
} RT_ShieldEffects;

// arch/ogl/shieldgl.c
void ogl_set_shield_effects(const RT_ShieldEffects *effects);
#define RT_SetShieldEffects ogl_set_shield_effects
void ogl_draw_shield_bubbles(void);   // inside the 3D frame, after the mine is drawn
void ogl_draw_shield_sweep(void);     // over the finished view (first-person sweep)
void ogl_init_shield_prog(void);      // with the other GLSL programs (oglprog.c)
void ogl_done_shield_prog(void);

#endif

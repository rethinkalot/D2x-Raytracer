
#ifndef _RT_LIGHTS_H
#define _RT_LIGHTS_H

#include "RTgr.h"
#include "Core/MiniMath.h"
#include "Renderer.h"

bool g_pending_light_update;
bool g_light_visual_debug;
int  g_active_lights;
// Light multiplier to tweak brightness of all the lights.
// Because this might change in runtime (end of level sequence), the ImGui windows should make use of the g_light_multiplier_default.
float g_light_multiplier;
// Default light multiplier (not changed during runtime)
float g_light_multiplier_default;

typedef struct RT_HeadlightSettings
{
	float pos_offset_horz;
	float pos_offset_vert;
	float skew_horz;
	float skew_vert;
	float radius;
	float brightness;
	float spot_angle;
	float spot_softness;
} RT_HeadlightSettings;

RT_HeadlightSettings g_headlights;

typedef struct RT_LightDefinition
{
	const char*  name;
	RT_LightKind kind;
	RT_Vec3      emission;
	float        radius;
	RT_Vec2		 size;
	float		 spot_angle;
	float		 spot_softness;
} RT_LightDefinition;

extern RT_LightDefinition g_light_definitions[];

void RT_InitLightStuff(void);
void RT_ResetLightSettings(void);
// Returns g_light_definition index if light, otherwise -1.
int RT_IsLight(int tmap);
// Re-reads the light textures' bitmap indices (call when a level's bitmaps change).
void RT_CacheLightBitmaps(void);
RT_Light RT_InitLight(RT_LightDefinition definition, RT_Vertex* vertices, RT_Vec3 normal);
void RT_ShowLightMenu(void);
void RT_VisualizeLight(RT_Light* light);
void RT_LoadLightSettings(void);
void RT_SaveLightSettings(void);
void RT_ResetHeadLightSettings(void);
void RT_LoadHeadLightSettings(void);
void RT_SaveHeadLightSettings(void);

// Per-profile lights: each render profile (assets/render_presets/<Name>.vars) can have its own
// lights/<Name>/ folder. Loading uses the profile's file when it has one, else the shared lights/ file.
struct RT_Arena;
const char *RT_LightsProfile(void);                                    // "" = shared only
void  RT_SetLightsProfileName(const char *render_profile);             // just the name ("X.vars" or "X")
char *RT_LightsLoadPath(struct RT_Arena *arena, const char *file);    // lights/<profile>/<file> if present, else lights/<file>
char *RT_LightsSavePath(struct RT_Arena *arena, const char *file);    // where the next save goes (see below)
void  RT_SetLightsSaveTarget(const char *profile);                     // NULL = current profile, "" = shared
bool  RT_LightsSaveToProfilePopup(const char *popup_id);               // "Save to profile:" popup; true once one is picked
void  RT_SetLightsProfile(const char *render_profile);                 // RTgr.c: switch + reload all light settings

#endif
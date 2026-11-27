/*
 * Raytracer stand-ins for D2s features that live in the OpenGL renderer.
 *
 * D2s draws replacement 3D models (xmodel/) and PNG textures (misc/pngfile.c)
 * through OpenGL. The DX12 raytracer has its own model and material system, so
 * in this build every custom model reports "not available" and the game falls
 * back to its normal polygon models.
 */
#include "xmodel.h"

void xmodel_load_all(void) {}
void xmodel_free_all(void) {}
void xmodel_load_gl_all(void) {}
void xmodel_free_gl_all(void) {}

int xmodel_exists(enum xmodel_type type, int modelnum)
{
	(void)type; (void)modelnum;
	return 0;
}

int xmodel_show_if_loaded(enum xmodel_type type, int modelnum, vms_vector *pos, vms_matrix *orient, int mpcolor, g3s_lrgb *light)
{
	(void)type; (void)modelnum; (void)pos; (void)orient; (void)mpcolor; (void)light;
	return 0;
}

void free_png_cache(void) {}

/*
 * Display queries D2s's resolution menu and frame limiter use. D2s implements
 * them in its OpenGL arch/ogl/gr.c; the raytracer's display code is RT/RTgr.c.
 */
#include <windows.h>
#include <SDL.h>
#include <SDL_syswm.h>

// Size of the monitor the game window is on (the primary one before the window exists).
int gr_monitor_size(int *w, int *h)
{
	HWND hw = NULL;
	HMONITOR mon;
	MONITORINFO mi;
	SDL_SysWMinfo info;
	POINT origin = { 0, 0 };

	SDL_VERSION(&info.version);
	if (SDL_GetWMInfo(&info) > 0)
		hw = info.window;
	mon = hw ? MonitorFromWindow(hw, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfo(mon, &mi))
		return 0;
	*w = mi.rcMonitor.right - mi.rcMonitor.left;
	*h = mi.rcMonitor.bottom - mi.rcMonitor.top;
	return 1;
}

// The raytracer has its own VSync switch in its render settings, so D2s's
// GameCfg.VSync item stays hidden and the frame cap follows PlayerCfg.maxFps.
int gr_vsync_supported(void)
{
	return 0;
}

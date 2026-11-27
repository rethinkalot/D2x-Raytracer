#ifdef RT_DX12
#include "Renderer.h"
static void vr_draw_graphics_timing(void) {
    RT_VRGraphicsState s=RT_VRGetGraphicsState();
    if(!s.active || !s.timing_enabled) return;
    gr_set_curfont(GAME_FONT);gr_set_fontcolor(BM_XRGB(0,31,0),-1);
    if(s.refresh_hz>0 && s.gpu_ms>0) gr_printf((int)FSPACX(1),(int)(LINE_SPACING*5),"Eye GPU %.1f / %.1f ms",s.gpu_ms,1000.0/s.refresh_hz);
    if(s.runtime_gpu_ms>0) gr_printf((int)FSPACX(1),(int)(LINE_SPACING*6),"SteamVR GPU %.1f ms",s.runtime_gpu_ms);
    if(s.frame_interval_ms>0) gr_printf((int)FSPACX(1),(int)(LINE_SPACING*7),"App interval %.1f ms",s.frame_interval_ms);
}
#endif

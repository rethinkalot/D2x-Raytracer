#ifdef RT_DX12
#include "Renderer.h"
static int opt_gr_vrmenu;
static int vr_menu_test_frames;

typedef struct VRMenuContext { RT_VRGraphicsSettings settings; } VRMenuContext;

static int vr_fsr_handler(newmenu* menu,d_event* event,void* userdata) {
    (void)menu;(void)userdata;
    if(vr_menu_test_frames && event->type==EVENT_WINDOW_DRAW && ++vr_menu_test_frames>7) return -2;
    return 0;
}
static void vr_fsr_config(RT_VRGraphicsSettings* s) {
    newmenu_item m[6]={0};
    char* labels[]={"Off (native)", "Quality", "Balanced", "Performance", "", "Back to apply changes"};
    int original=s->resolution_mode>=3 ? s->resolution_mode-2 : 0;
    for(int i=0;i<6;++i) { m[i].type=NM_TYPE_TEXT;m[i].text=labels[i]; }
    for(int i=0;i<4;++i) { m[i].type=NM_TYPE_RADIO;m[i].group=0;m[i].value=original==i; }
    newmenu_do1(NULL,"FSR 3.1",6,m,vr_fsr_handler,NULL,original);
    int selected=m[1].value ? 1 : (m[2].value ? 2 : (m[3].value ? 3 : 0));
    // Preserve old custom preferences until a different option is actually selected.
    if(selected!=original) s->resolution_mode=selected ? selected+2 : 0;
}

static void vr_monitor_config(RT_VRGraphicsSettings* s) {
    newmenu_item m[3]={0};
    char* labels[]={"Fill window", "Fit entire view", "Off during gameplay"};
    for(int i=0;i<3;++i) { m[i].type=NM_TYPE_RADIO;m[i].group=0;m[i].text=labels[i];m[i].value=s->mirror_mode==i; }
    newmenu_do1(NULL,"VR Monitor View",3,m,NULL,NULL,0);
    s->mirror_mode=m[1].value ? 1 : (m[2].value ? 2 : 0);
}

static int vr_graphics_handler(newmenu* menu,d_event* event,void* userdata) {
    VRMenuContext* c=(VRMenuContext*)userdata;
    if(vr_menu_test_frames && event->type==EVENT_WINDOW_DRAW && ++vr_menu_test_frames>7) return -2;
    if(event->type==EVENT_NEWMENU_SELECTED) {
        int selected=newmenu_get_citem(menu);
        if(selected==0) vr_fsr_config(&c->settings);
        if(selected==1) vr_monitor_config(&c->settings);
        return 1;
    }
    return 0;
}

static void vr_graphics_config(void) {
    VRMenuContext c={0};c.settings=RT_VRGetGraphicsState().settings;
    RT_VRGraphicsSettings original=c.settings;
    newmenu_item m[5]={0};
    char* labels[]={"FSR 3.1...", "Monitor view...", "Show performance", "", "Back to apply changes."};
    for(int i=0;i<5;++i) { m[i].type=NM_TYPE_TEXT;m[i].text=labels[i]; }
    m[0].type=m[1].type=NM_TYPE_MENU;
    m[2].type=NM_TYPE_CHECK;m[2].value=c.settings.show_timing;
    newmenu_do1(NULL,"VR Graphics",5,m,vr_graphics_handler,&c,0);
    c.settings.show_timing=m[2].value;
    // Like the other graphics menus, save when leaving; merely opening it changes nothing.
    if(c.settings.resolution_mode!=original.resolution_mode || c.settings.scale_percent!=original.scale_percent ||
       c.settings.mirror_mode!=original.mirror_mode || c.settings.show_timing!=original.show_timing) {
        if(!RT_VRSaveGraphicsSettings(&c.settings)) nm_messagebox(TXT_ERROR,1,TXT_OK,"Could not save VR settings.");
    }
}
void vr_graphics_menu_test(void) { vr_menu_test_frames=1;
    RT_VRGraphicsSettings s=RT_VRGetGraphicsState().settings;
    vr_fsr_config(&s);
    vr_menu_test_frames=0; }
#endif

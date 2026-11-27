namespace {
bool graphicsLoaded;
VRGraphics::Settings graphicsSaved;
unsigned graphicsRecommendedWidth,graphicsRecommendedHeight;
unsigned graphicsLegacyCap=1664;
float graphicsRefresh;
char graphicsHeadset[128]={};
void LoadGraphicsSettings() {
    if(!graphicsLoaded) { graphicsSaved=VRGraphics::Load("vr_graphics.cfg");graphicsLoaded=true; }
}
uint64_t EyeAllocationBytes(unsigned w,unsigned h) {
    uint64_t bytes=0;
    const auto render=VRGraphics::RenderSize(graphicsRunning,w,h);
#define ESTIMATE_EYE(name,reg,sx,sy,output,type,format) \
    { auto* reference=g_d3d.render_targets[RenderTarget_##name]; \
      if(reference) { auto desc=reference->GetDesc(); \
        desc.Width=((output ? w : render.width)+sx-1)/sx;desc.Height=((output ? h : render.height)+sy-1)/sy; \
        const uint64_t allocation=g_d3d.device->GetResourceAllocationInfo(0,1,&desc).SizeInBytes; \
        if(allocation==UINT64_MAX) return UINT64_MAX; \
        bytes+=allocation*2u; } }
    RT_RENDER_TARGETS(ESTIMATE_EYE)
#undef ESTIMATE_EYE
    return bytes;
}
void ConfigureGraphics(unsigned width,unsigned height,unsigned cap) {
    LoadGraphicsSettings();graphicsRunning=graphicsSaved;
    graphicsRecommendedWidth=width;graphicsRecommendedHeight=height;graphicsLegacyCap=cap;
    const auto size=VRGraphics::Resolve(graphicsRunning,width,height,cap);
    if(size.width && size.height) { eyeWidth=size.width;eyeHeight=size.height; }
    DXGI_QUERY_VIDEO_MEMORY_INFO memory={};
    if(SUCCEEDED(g_d3d.dxgi_adapter4->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&memory))) {
        const uint64_t reserve=512ull*1024*1024;
        const uint64_t available=memory.Budget>memory.CurrentUsage+reserve ? memory.Budget-memory.CurrentUsage-reserve : 0;
        if(EyeAllocationBytes(eyeWidth,eyeHeight)>available)
            Log("VR memory budget tight at the requested output; output resolution retained for all presets.");
    }
    if(VRGraphics::Upscaling(graphicsRunning)) {
        const auto render=VRGraphics::RenderSize(graphicsRunning,eyeWidth,eyeHeight);
        char message[180];std::snprintf(message,sizeof(message),"VR FSR 3.1: %s, render %ux%u -> output %ux%u per eye; independent histories, no frame generation.",
            graphicsRunning.mode==3 ? "Quality" : (graphicsRunning.mode==4 ? "Balanced" : "Performance"),render.width,render.height,eyeWidth,eyeHeight);Log(message);
    }
    if(systemVR) {
    systemVR->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,vr::Prop_ModelNumber_String,graphicsHeadset,sizeof(graphicsHeadset));
    graphicsRefresh=systemVR->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,vr::Prop_DisplayFrequency_Float);
    if(!std::isfinite(graphicsRefresh) || graphicsRefresh<10 || graphicsRefresh>500) graphicsRefresh=0;
    }
    if(size.limited) Log("VR resolution limited to 3072 pixels on the longest edge.");
}
void ApplyPendingGraphics() {
    if(!active || !graphicsPending) return;
    graphicsPending=false;
    const bool resize=graphicsSaved.mode!=graphicsRunning.mode ||
        (graphicsSaved.mode==2 && graphicsSaved.percent!=graphicsRunning.percent);
    if(resize) {
        // Called at the frame boundary after tracking, before any UI/eye commands.
        // Finish old GPU work, then replace both eye buffers and their FSR histories.
        ReleaseTargets();
        ConfigureGraphics(graphicsRecommendedWidth ? graphicsRecommendedWidth : eyeWidth,
                          graphicsRecommendedHeight ? graphicsRecommendedHeight : eyeHeight,graphicsLegacyCap);
        resetHistory=true;haveAim=false;
        lastEyeGPU[0]=lastEyeGPU[1]=0;
        Log("VR graphics applied at frame boundary; eye buffers will rebuild before rendering.");
    } else graphicsRunning=graphicsSaved;
}
void QueueGraphicsSettingsForTest(int mode) {
    // Headless fixture only; exercise the same deferred apply path without writing preferences.
    graphicsLoaded=true;graphicsSaved=graphicsRunning;graphicsSaved.mode=mode;
    graphicsPending=true;ApplyPendingGraphics();
}
}
RT_VRGraphicsState OpenVRPrototype::GraphicsState() {
    LoadGraphicsSettings();RT_VRGraphicsState s={};
    s.settings={graphicsSaved.mode,graphicsSaved.percent,graphicsSaved.mirror,graphicsSaved.timing};
    s.active=active && !offlineTest;
    if(s.active) {
        s.recommended_width=graphicsRecommendedWidth;s.recommended_height=graphicsRecommendedHeight;
        s.eye_width=eyeWidth;s.eye_height=eyeHeight;s.refresh_hz=graphicsRefresh;
        s.gpu_ms=static_cast<float>(lastEyeGPU[0]+lastEyeGPU[1]);
        s.runtime_gpu_ms=lastRuntimeGPU;s.frame_interval_ms=lastFrameInterval;
        s.timing_enabled=graphicsRunning.timing;
        std::snprintf(s.headset,sizeof(s.headset),"%s",graphicsHeadset);
    } else std::snprintf(s.headset,sizeof(s.headset),"Start VR to detect a headset");
    s.legacy_cap=graphicsLegacyCap;return s;
}
bool OpenVRPrototype::SaveGraphicsSettings(const RT_VRGraphicsSettings* in) {
    if(!in) return false;
    VRGraphics::Settings s={in->resolution_mode,in->scale_percent,in->mirror_mode,in->show_timing};s=VRGraphics::Normalize(s);
    if(!VRGraphics::WriteFile("vr_graphics.cfg.tmp",s) ||
       !MoveFileExA("vr_graphics.cfg.tmp","vr_graphics.cfg",MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) return false;
    graphicsSaved=s;graphicsLoaded=true;graphicsPending=true;
    Log("VR graphics preferences saved; applying at the next frame boundary.");return true;
}

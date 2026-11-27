#pragma once
// A fixed logical VR canvas: monitor dimensions only scale the final image.
#define RT_VR_MIRROR_WIDTH 1920u
#define RT_VR_MIRROR_HEIGHT 1080u
typedef struct RT_VRMirrorRect { float x,y,width,height; } RT_VRMirrorRect;
typedef struct RT_VRMousePoint { float x,y; } RT_VRMousePoint;
static inline RT_VRMirrorRect RT_VRMirrorPresentationRect(float width,float height) {
    RT_VRMirrorRect r={0,0,(float)RT_VR_MIRROR_WIDTH,(float)RT_VR_MIRROR_HEIGHT};
    if(width<=0 || height<=0) return r;
    float aspect=width/height,canvas=(float)RT_VR_MIRROR_WIDTH/RT_VR_MIRROR_HEIGHT;
    if(aspect>canvas) { r.width*=canvas/aspect;r.x=(RT_VR_MIRROR_WIDTH-r.width)*0.5f; }
    else { r.height*=aspect/canvas;r.y=(RT_VR_MIRROR_HEIGHT-r.height)*0.5f; }
    return r;
}
static inline RT_VRMousePoint RT_VRMirrorMouse(float x,float y,float width,float height) {
    RT_VRMousePoint p={-1,-1};if(width<=0 || height<=0) return p;
    float sx=width/RT_VR_MIRROR_WIDTH,sy=height/RT_VR_MIRROR_HEIGHT;
    float scale=sx<sy ? sx : sy;
    p.x=(x-(width-RT_VR_MIRROR_WIDTH*scale)*0.5f)/scale;
    p.y=(y-(height-RT_VR_MIRROR_HEIGHT*scale)*0.5f)/scale;return p;
}

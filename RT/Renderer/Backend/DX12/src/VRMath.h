#pragma once
#include <cmath>

// Column-vector rigid transforms in OpenVR's right-handed, metre-based space.
// Kept independent of the engine so tracking/eye math can be tested without a headset.
namespace VRMath {
struct Pose { float m[3][4]; };
// LH eye-space point to the same asymmetric NDC used by primary world rays.
inline bool ProjectEye(float x,float y,float z,float left,float right,float top,float bottom,float& nx,float& ny) {
    if(z<=0.001f || !std::isfinite(z)) return false;
    nx=(2*x/z-right-left)/(right-left);
    ny=(2*y/z+bottom+top)/(bottom-top);
    return std::isfinite(nx) && std::isfinite(ny);
}
inline Pose Identity() { return {{{1,0,0,0},{0,1,0,0},{0,0,1,0}}}; }
inline Pose Multiply(const Pose& a, const Pose& b) {
    Pose r = {};
    for (int i=0;i<3;++i) {
        for (int j=0;j<3;++j)
            for (int k=0;k<3;++k) r.m[i][j] += a.m[i][k]*b.m[k][j];
        r.m[i][3]=a.m[i][3];
        for (int k=0;k<3;++k) r.m[i][3] += a.m[i][k]*b.m[k][3];
    }
    return r;
}
inline Pose Inverse(const Pose& p) {
    Pose r={};
    for(int i=0;i<3;++i) {
        for(int j=0;j<3;++j) r.m[i][j]=p.m[j][i];
        for(int j=0;j<3;++j) r.m[i][3]-=r.m[i][j]*p.m[j][3];
    }
    return r;
}
inline bool Finite(const Pose& p) {
    for (const auto& row:p.m) for(float x:row) if(!std::isfinite(x)) return false;
    return true;
}
inline Pose ToLeftHanded(const Pose& p) {
    Pose r=p;
    const float sign[3]={1,1,-1};
    for(int i=0;i<3;++i) {
        for(int j=0;j<3;++j) r.m[i][j]=sign[i]*p.m[i][j]*sign[j];
        r.m[i][3]=sign[i]*p.m[i][3];
    }
    return r;
}
// Seated recentering keeps the head's position and heading only. Pitch and roll
// at the moment of recentering (looking down at the keyboard, or a headset not
// yet worn) must not tilt the world horizon. OpenVR is right-handed, +Y up,
// and the head looks along -Z.
inline Pose YawOnly(const Pose& p) {
    float fx=-p.m[0][2],fz=-p.m[2][2];
    float len=std::sqrt(fx*fx+fz*fz);
    if(len<1e-3f) {
        // Straight down: head-up points along the heading. Straight up: opposite.
        const float sign=-p.m[1][2]>0 ? -1.0f : 1.0f;
        fx=sign*p.m[0][1];fz=sign*p.m[2][1];len=std::sqrt(fx*fx+fz*fz);
    }
    Pose r=Identity();
    if(len>=1e-3f && std::isfinite(len)) {
        const float s=-fx/len,c=-fz/len;
        r.m[0][0]=c; r.m[0][2]=s;
        r.m[2][0]=-s; r.m[2][2]=c;
    }
    for(int i=0;i<3;++i) r.m[i][3]=p.m[i][3];
    return r;
}
// A bad/lost tracking sample must not create a non-finite or extreme camera.
inline Pose RelativeEye(const Pose& origin, const Pose& head, const Pose& eye) {
    return ToLeftHanded(Multiply(Multiply(Inverse(origin),head),eye));
}
}

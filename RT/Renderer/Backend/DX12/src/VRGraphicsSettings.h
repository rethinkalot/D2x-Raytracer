#pragma once
#include <cstdio>
#include <algorithm>
#include <cstdint>
#include <cstring>
namespace VRGraphics {
struct Settings { int mode=4, percent=100, mirror=0, timing=0; };
struct Size { unsigned width=0,height=0; bool limited=false; };
inline Settings Normalize(Settings s) {
    if(s.mode<0 || s.mode>5) s.mode=0;
    s.percent=std::max(50,std::min(150,s.percent));
    if(s.mirror<0 || s.mirror>2) s.mirror=0;
    s.timing=s.timing ? 1 : 0;return s;
}
inline Settings Load(const char* path) {
    Settings s;
    if(FILE* f=std::fopen(path,"r")) {
        char line[256],key[64],extra;int value;
        while(std::fgets(line,sizeof(line),f)) {
            if(std::sscanf(line,"%63[^=]=%d %c",key,&value,&extra)!=2) continue;
            if(!std::strcmp(key,"mode")) s.mode=value;
            if(!std::strcmp(key,"percent")) s.percent=value;
            if(!std::strcmp(key,"mirror")) s.mirror=value;
            if(!std::strcmp(key,"timing")) s.timing=value;
        }
        std::fclose(f);
    }
    return Normalize(s);
}
inline bool WriteFile(const char* path,Settings s) {
    s=Normalize(s);FILE* f=std::fopen(path,"w");if(!f) return false;
    bool wrote=std::fprintf(f,"mode=%d\npercent=%d\nmirror=%d\ntiming=%d\n",s.mode,s.percent,s.mirror,s.timing)>0;
    bool closed=std::fclose(f)==0;return wrote && closed;
}
inline bool Upscaling(Settings s) { return s.mode>=3 && s.mode<=5; }
inline double Ratio(Settings s) { return s.mode==3 ? 1.5 : (s.mode==4 ? 1.7 : (s.mode==5 ? 2.0 : 1.0)); }
inline Size RenderSize(Settings s,unsigned w,unsigned h) {
    return {std::max(1u,unsigned(w/Ratio(s))),std::max(1u,unsigned(h/Ratio(s))),false};
}
// All current presets share SteamVR's recommended output. Old percentage
// preferences and the old reduced-resolution cap remain serializable but do
// not change output size; only RenderSize selects a preset's internal size.
inline Size Resolve(Settings /*settings*/,unsigned w,unsigned h,unsigned /*legacyCap*/) {
    if(!w || !h) return {};
    const unsigned cap=3072;
    const unsigned longest=std::max(w,h);
    const double scale=longest>cap ? double(cap)/longest : 1.0;
    Size out;out.limited=longest>cap;
    out.width=std::max(256u,unsigned(w*scale));out.height=std::max(256u,unsigned(h*scale));return out;
}
}

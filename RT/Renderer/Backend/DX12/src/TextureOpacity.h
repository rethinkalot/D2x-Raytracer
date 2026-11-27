#pragma once
#include "ApiTypes.h"
#include <cstring>

namespace RT {
// Conservative proof of level-zero alpha=1, matching the renderer's tightly
// packed source uploads. Unknown encodings stay on the ordinary alpha path.
// BC7 endpoint layout: https://learn.microsoft.com/en-us/windows/win32/direct3d11/bc7-format
inline unsigned OpacityBits(const uint8_t* p,unsigned& position,unsigned count) {
    if(!count) return 0;
    unsigned byte=position/8, shift=position%8;
    unsigned value=p[byte]>>shift;
    if(shift+count>8) value|=unsigned(p[byte+1])<<(8-shift);
    position+=count;
    return value&((1u<<count)-1);
}
inline bool OpaqueBC7Block(const uint8_t* p) {
    unsigned mode=0;while(mode<8 && !(p[0]&(1u<<mode))) ++mode;
    if(mode==8) return false;
    if(mode<4) return true; // RGB-only modes have implicit alpha=1.
    unsigned bit=mode+1, endpoints=2, color_bits=7, alpha_bits=7, channel=3, pbits=0;
    if(mode==4 || mode==5) {
        unsigned rotation=OpacityBits(p,bit,2);
        if(mode==4) OpacityBits(p,bit,1);
        channel=rotation ? rotation-1 : 3;
        color_bits=mode==4 ? 5 : 7;alpha_bits=mode==4 ? 6 : 8;
    } else if(mode==6) pbits=2;
    else { OpacityBits(p,bit,6);endpoints=4;color_bits=alpha_bits=5;pbits=4; }
    unsigned start=bit;
    bit+=channel*endpoints*color_bits;
    unsigned bits=channel==3 ? alpha_bits : color_bits;
    for(unsigned e=0;e<endpoints;++e)
        if(OpacityBits(p,bit,bits)!=((1u<<bits)-1)) return false;
    bit=start+endpoints*(3*color_bits+alpha_bits);
    for(unsigned i=0;i<pbits;++i) if(!OpacityBits(p,bit,1)) return false;
    return true;
}
inline bool TextureHasOpaqueAlpha(const RT_Image& image) {
    if(!image.pixels || !image.width || !image.height) return false;
    const auto* p=static_cast<const uint8_t*>(image.pixels);
    const size_t pixels=size_t(image.width)*image.height;
    switch(image.format) {
        case RT_TextureFormat_R8:case RT_TextureFormat_BC4:case RT_TextureFormat_BC5:return true;
        case RT_TextureFormat_RGBA8:case RT_TextureFormat_RGBA8_SRGB:
            for(size_t i=0;i<pixels;++i) if(p[4*i+3]!=255) return false;
            return true;
        default:break;
    }
    // Non-block-aligned inputs are not classified: the legacy upload path's
    // byte-count assumptions cannot establish their complete alpha contents.
    if((image.width%4) || (image.height%4)) return false;
    const size_t blocks=size_t(image.width/4)*(image.height/4);
    for(size_t b=0;b<blocks;++b) {
        switch(image.format) {
            case RT_TextureFormat_BC1:case RT_TextureFormat_BC1_SRGB: {
                const uint8_t* q=p+8*b;
                unsigned c0=q[0]|(unsigned(q[1])<<8),c1=q[2]|(unsigned(q[3])<<8);
                if(c0<=c1) for(unsigned i=0;i<4;++i) for(unsigned k=0;k<4;++k)
                    if(((q[4+i]>>(2*k))&3u)==3) return false;
                break;
            }
            case RT_TextureFormat_BC2:case RT_TextureFormat_BC2_SRGB:
                for(unsigned i=0;i<8;++i) if(p[16*b+i]!=255) return false;
                break;
            case RT_TextureFormat_BC3:case RT_TextureFormat_BC3_SRGB: {
                const uint8_t* q=p+16*b;unsigned bit=16;
                for(unsigned i=0;i<16;++i) {
                    unsigned index=OpacityBits(q,bit,3);
                    bool full=(index==0 && q[0]==255) || (index==1 && q[1]==255) ||
                        (q[0]<=q[1] && (index==7 || (index>=2 && index<=5 && q[0]==255 && q[1]==255)));
                    if(!full) return false;
                }
                break;
            }
            case RT_TextureFormat_BC7:case RT_TextureFormat_BC7_SRGB:
                if(!OpaqueBC7Block(p+16*b)) return false;
                break;
            default:return false;
        }
    }
    return true;
}
}

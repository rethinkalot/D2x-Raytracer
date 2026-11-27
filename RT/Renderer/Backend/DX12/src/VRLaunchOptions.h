#pragma once
// Match complete argv tokens, ignoring ASCII case, without accepting prefixes.
namespace VRLaunch {
inline bool EqualOption(const wchar_t* a,const wchar_t* b) {
    while(*a && *b) {
        wchar_t x=*a++,y=*b++;
        if(x>=L'A' && x<=L'Z') x+=L'a'-L'A';
        if(y>=L'A' && y<=L'Z') y+=L'a'-L'A';
        if(x!=y) return false;
    }
    return *a==*b;
}
inline bool HasOption(int argc,const wchar_t* const* argv,const wchar_t* option) {
    for(int i=1;i<argc;++i) if(EqualOption(argv[i],option)) return true;
    return false;
}
}

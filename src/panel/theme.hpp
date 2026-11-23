#pragma once
#include <algorithm>
#include <cmath>

namespace sinan::panel {
enum class Theme { system, light, dark };

struct Palette {
    unsigned background, surface, input, border, divider, foreground, muted,
        accent, on_accent, selected, hover, icon_surface, folder, success, success_surface;
};

inline double luminance(unsigned rgb) {
    auto channel=[](unsigned value) {
        double s=value/255.0;
        return s<=.04045?s/12.92:std::pow((s+.055)/1.055,2.4);
    };
    return .2126*channel((rgb>>16)&255)+.7152*channel((rgb>>8)&255)+.0722*channel(rgb&255);
}
inline double contrast(unsigned first,unsigned second) {
    auto a=luminance(first),b=luminance(second);
    return (std::max(a,b)+.05)/(std::min(a,b)+.05);
}
inline unsigned mix(unsigned from,unsigned to,double amount) {
    unsigned value=0;
    for(int shift:{16,8,0}) {
        auto a=(from>>shift)&255,b=(to>>shift)&255;
        value|=static_cast<unsigned>(std::lround(a+(static_cast<double>(b)-a)*amount))<<shift;
    }
    return value;
}
inline Palette fluent_palette(bool dark,unsigned system_accent=0) {
    Palette p=dark?
        Palette{0x202020,0x272727,0x2D2D2D,0x454545,0x383838,0xFFFFFF,0xC5C5C5,
            0x60CDFF,0x000000,0x383838,0x323232,0x333333,0xFCE100,0x6CCB5F,0x263526}:
        Palette{0xF3F3F3,0xFFFFFF,0xFFFFFF,0xD1D1D1,0xEBEBEB,0x1A1A1A,0x616161,
            0x0067C0,0xFFFFFF,0xEDEDED,0xF5F5F5,0xF3F3F3,0x9D5D00,0x0F6C20,0xEAF5EA};
    if(system_accent) {
        auto original=system_accent&0xFFFFFF;
        p.accent=original;
        for(int step=1;contrast(p.accent,p.input)<4.5&&step<=100;++step)
            p.accent=mix(original,dark?0xFFFFFF:0x000000,step/100.0);
    }
    p.on_accent=contrast(0xFFFFFF,p.accent)>=contrast(0x000000,p.accent)?0xFFFFFF:0x000000;
    return p;
}
}

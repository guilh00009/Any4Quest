// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cmath>
#include "core/libraries/hmd/wide_near_preview.h"

int main() {
    using namespace Libraries::Hmd::Preview;
    unsigned checks = 0;
    auto check = [&](bool ok) { ++checks; if (!ok) std::printf("FAIL %u\n", checks); return ok; };
    const EyeMaps left{{{.104656108f,.197961479f,.126364931f,.75f},
                        {.208380505f,.416761011f,.125f,.25f},
                        {.418624432f,.395922958f,.505459726f,.5f}}};
    // Deliberately asymmetric fixture: each eye occupies its own atlas quarter.
    auto right = left;
    right[0][2] += .25f;
    right[1][2] += .25f;
    if (!check(SelectEyeMaps(0,left,right) == left)) return 1;
    if (!check(SelectEyeMaps(1,left,right) == right)) return 1;
    bool distinct=true, covered=true, aligned=true;
    for(unsigned y=0;y<108;++y) for(unsigned x=0;x<96;++x) {
        float eye_color[2]{};
        for(unsigned eye=0;eye<2;++eye) {
            const auto maps=SelectEyeMaps(eye,left,right);
            const auto t=Tangent(maps[2],(x+.5f)/96,(y+.5f)/108);
            const float w=WideWeight(t[0],t[1],.473576993f,.599864185f);
            const auto n=Uv(maps[1],t[0],t[1]), a=Uv(maps[0],t[0],t[1]);
            const float lo=eye*.25f, hi=lo+.25f;
            if(w<1) covered &= n[0]>=lo&&n[0]<=hi&&n[1]>=0&&n[1]<=.5f;
            if(w>0) covered &= a[0]>=lo&&a[0]<=hi&&a[1]>=.5f&&a[1]<=1;
            // Atlas rendered with red left / blue right markers. Duplicated-left fails.
            const auto color=[](float u){return u>=.25f?1.0f:0.0f;};
            eye_color[eye]=(1-w)*color(n[0])+w*color(a[0]);
            const float ramp=(1-w)*(n[0]-maps[1][2])/maps[1][0]+
                              w*(a[0]-maps[0][2])/maps[0][0];
            aligned &= std::abs(ramp-t[0])<1e-5f;
        }
        distinct &= eye_color[0]==0 && eye_color[1]==1;
    }
    if(!check(distinct)||!check(covered)||!check(aligned)) return 1;
    // Right full-view mapping must not silently reuse the left eye's view.
    right[2]={.45f,.4f,.47f,.52f};
    const auto actual=SelectEyeMaps(1,left,right);
    const auto t=Tangent(actual[2],.47f,.52f);
    if(!check(std::abs(t[0])<1e-6f&&std::abs(t[1])<1e-6f)) return 1;
    std::printf("PASS %u checks including 20736 per-eye samples; duplicate-left marker regression covered\n",checks);
}

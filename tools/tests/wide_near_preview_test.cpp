// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <limits>
#include "core/libraries/hmd/wide_near_preview.h"
int main() {
    using namespace Libraries::Hmd::Preview;
    unsigned checks=0;
    auto check=[&](bool ok){++checks;if(!ok)std::printf("FAIL %u\n",checks);return ok;};
    if(!check(WideWeight(0,0,.4f,.6f)==0))return 1;
    if(!check(WideWeight(.7f,0,.4f,.6f)==1))return 1;
    if(!check(std::abs(WideWeight(.3f,.4f,.4f,.6f)-.5f)<1e-5f))return 1;
    if(!check(!ValidBand(.6f,.4f)&&!ValidBand(.4f,.4f)))return 1;
    if(!check(!ValidBand(0,std::numeric_limits<float>::infinity())))return 1;
    if(!check(!ValidMap({0,1,0,0})&&!ValidMap({1,NAN,0,0})))return 1;
    const std::array<float,4> wide{.104656108f,.197961479f,.126364931f,.75f};
    const std::array<float,4> near{.208380505f,.416761011f,.125f,.25f};
    const std::array<float,4> view{.418624432f,.395922958f,.505459726f,.5f};
    bool covered=true,aligned=true;
    for(unsigned y=0;y<108;++y)for(unsigned x=0;x<96;++x){
        const auto t=Tangent(view,(x+.5f)/96,(y+.5f)/108);
        float w=WideWeight(t[0],t[1],.473576993f,.599864185f);
        const auto a=Uv(near,t[0],t[1]),b=Uv(wide,t[0],t[1]);
        covered &= w>=0&&w<=1;
        if(w<1) covered &= a[0]>=0&&a[0]<=.25f&&a[1]>=0&&a[1]<=.5f;
        if(w>0) covered &= b[0]>=0&&b[0]<=.25f&&b[1]>=.5f&&b[1]<=1;
        // A synthetic world-coordinate ramp reconstructs the same geometry in both layers.
        float reconstructed=(1-w)*(a[0]-near[2])/near[0]+w*(b[0]-wide[2])/wide[0];
        aligned &= std::abs(reconstructed-t[0])<1e-5f;
    }
    if(!check(covered)||!check(aligned))return 1;
    std::printf("PASS %u checks including 10368 coverage/alignment samples\n",checks);
}

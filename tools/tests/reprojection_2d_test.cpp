// SPDX-License-Identifier: GPL-2.0-or-later
#include <limits>
#include <cstdio>
#include "core/libraries/hmd/reprojection_2d.h"
int main() {
    using namespace Libraries::Hmd;
    Reprojection2dParam p{}; p.time_us=3000; p.uv={1,1,0,0};
    unsigned checks=0;
    auto check=[&](bool ok) { ++checks; if(!ok) { std::printf("FAIL %u\n", checks); return false; } return true; };
    if(!check(Valid2dValues(p))) return 1;
    for(auto time : {1999u,2000u,6999u,7000u}) {
        p.time_us=time; if(!check(Valid2dValues(p)==(time>=2000&&time<7000))) return 1;
    }
    p.time_us=3000;
    for(unsigned i=0;i<4;++i) {p.reserved[i]=1;if(!check(!Valid2dValues(p)))return 1;p.reserved[i]=0;}
    p.uv[0]=std::numeric_limits<float>::quiet_NaN();if(!check(!Valid2dValues(p)))return 1;
    if(!check(Transform2dUv({1,1,0,0},0.25f,0.75f)==std::array{0.25f,0.75f}))return 1;
    if(!check(Transform2dUv({0.5f,0.5f,0.25f,0.125f},1,1)==std::array{0.75f,0.625f}))return 1;
    if(!check(Transform2dUv({-1,1,1,0},0.25f,0.75f)==std::array{0.75f,0.75f}))return 1;
    std::printf("PASS %u reprojection 2D checks\n",checks);
}

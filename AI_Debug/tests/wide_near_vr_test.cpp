// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/libraries/hmd/wide_near_vr.h"
#include <cstdio>
#include <limits>
int main() {
  using namespace Libraries::Hmd::Preview;
  unsigned checks = 0;
  auto check = [&](bool value) {
    ++checks;
    if (!value)
      std::printf("FAIL %u\n", checks);
    return value;
  };
  const std::array<float, 4> left{.418624432f, .395922958f, .505459726f, .5f};
  const std::array<float, 4> right{.45f, .4f, .47f, .52f};
  std::array<float, 4> view{};
  if (!check(CommonVrView(left, right, view)))
    return 1;
  bool contained = true, matching = true;
  for (unsigned y = 0; y <= 108; ++y)
    for (unsigned x = 0; x <= 96; ++x) {
      const auto tangent = Tangent(view, x / 96.f, y / 108.f);
      for (const auto &source : {left, right}) {
        const auto uv = Uv(source, tangent[0], tangent[1]);
        contained &= uv[0] >= -1e-6f && uv[0] <= 1.000001f && uv[1] >= -1e-6f &&
                     uv[1] <= 1.000001f;
      }
      matching &=
          std::abs(tangent[0] - (2 * x / 96.f - 1) * (.5f / view[0])) < 1e-5f;
    }
  if (!check(contained) || !check(matching))
    return 1;
  auto bad = left;
  bad[0] = 0;
  if (!check(!CommonVrView(bad, right, view)))
    return 1;
  bad = left;
  bad[2] = 2;
  if (!check(!CommonVrView(bad, right, view)))
    return 1;
  bad = left;
  bad[1] = std::numeric_limits<float>::quiet_NaN();
  if (!check(!CommonVrView(bad, right, view)))
    return 1;
  float p[]{0, 1, 0}, q[]{0, 0, 0, 1};
  if (!check(ValidRenderPose(p, q)))
    return 1;
  q[3] = 0;
  if (!check(!ValidRenderPose(p, q)))
    return 1;
  q[3] = 1;
  p[0] = std::numeric_limits<float>::infinity();
  if (!check(!ValidRenderPose(p, q)))
    return 1;
  std::printf("PASS %u checks; 10573 stereo projection samples; invalid "
              "pose/FOV rejected\n",
              checks);
}

#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
"${CXX:-c++}" -std=c++20 -O1 -g -Wall -Wextra -Werror -Wno-missing-field-initializers \
  ${ANY4QUEST_TEST_CXXFLAGS:-} \
  -I "$root/quest-host/tests/include" \
  -I "$root/quest-host/cpp" \
  -I "$root/shadps4-arm64-main/src/core/vr" \
  -I "$root/shadps4-arm64-main/externals/openxr-sdk/include" \
  "$root/quest-host/tests/move_controllers_test.cpp" \
  "$root/quest-host/cpp/move_controllers.cpp" \
  -o "$out/test-move-actions"
"$out/test-move-actions"

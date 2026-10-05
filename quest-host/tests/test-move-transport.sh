#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
mkdir -p "$out/include/common/logging" "$out/include/core/libraries/system"
cp "$root/tools/tests/any4quest_stubs/common/logging/log.h" "$out/include/common/logging/"
cp "$root/tools/tests/any4quest_stubs/common/path_util.h" "$out/include/common/"
cp "$root/tools/tests/any4quest_stubs/core/libraries/system/systemservice.h" \
   "$out/include/core/libraries/system/"
printf '#pragma once\nnamespace Common { inline void SetCurrentThreadName(const char*) {} }\n' \
    > "$out/include/common/thread.h"
mock=()
if [[ "${1:-}" == "--mock-socket" ]]; then
    mock=(-DMOCK_HOST_SOCKET -Wl,--wrap=socket,--wrap=connect,--wrap=send,--wrap=recvmsg,--wrap=recv,--wrap=close)
fi
"${CXX:-c++}" -std=c++23 -O1 -g -Wall -Wextra -Werror \
  -Wno-unused-variable -Wno-unused-parameter -pthread \
  ${ANY4QUEST_TEST_CXXFLAGS:-} \
  "${mock[@]}" \
  -I "$out/include" -I "$root/shadps4-arm64-main/src" \
  -I "$root/shadps4-arm64-main/externals/json/single_include" \
  "$root/quest-host/tests/move_transport_test.cpp" \
  "$root/shadps4-arm64-main/src/core/vr/vr_runtime.cpp" \
  "$root/shadps4-arm64-main/src/core/vr/vr_host_link.cpp" \
  -o "$out/test-move-transport"
"$out/test-move-transport" "$out/test.sock"

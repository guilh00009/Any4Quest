#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
json=shadps4-arm64-main/externals/json/single_include
if [ ! -f "$json/nlohmann/json.hpp" ]; then
    echo 'Missing pinned JSON source. Run: git submodule update --init shadps4-arm64-main/externals/json' >&2
    exit 2
fi
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
"${CXX:-c++}" -std=c++23 -Wall -Wextra -Werror -Wno-unused-variable -Wno-unused-parameter \
    -pthread ${ANY4QUEST_TEST_CXXFLAGS:-} -Itools/tests/any4quest_stubs -Ishadps4-arm64-main/src -I"$json" \
    tools/tests/any4quest_runtime_test.cpp shadps4-arm64-main/src/core/vr/vr_runtime.cpp \
    -o "$out/runtime-test"
"$out/runtime-test"

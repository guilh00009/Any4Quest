# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile the asset-free runtime fixture with an existing C++23 compiler."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="any4quest-runtime-") as temp:
    exe = Path(temp) / ("runtime-test.exe" if os.name == "nt" else "runtime-test")
    command = [args.cxx, "-std=c++23", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-variable", "-Wno-unused-parameter"]
    command += ["-D_CRT_SECURE_NO_WARNINGS"] if os.name == "nt" else ["-pthread"]
    command += ["-I" + str(root / p) for p in
                ("AI_Debug/tests/any4quest_stubs", "shadps4-arm64-main/src",
                 "shadps4-arm64-main/externals/json/single_include")]
    command += [str(root / "AI_Debug/tests/any4quest_runtime_test.cpp"),
                str(root / "shadps4-arm64-main/src/core/vr/vr_runtime.cpp"), "-o", str(exe)]
    subprocess.run(command, check=True)
    subprocess.run([str(exe)], check=True)

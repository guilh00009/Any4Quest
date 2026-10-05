#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the shader module metadata ownership regression using an existing Ninja build's include paths.

Windows: run from an x64 Visual Studio developer prompt with clang++ on PATH.
No downloads, installation or changes to the emulator build configuration.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--compiler', default='clang++')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
source = root / 'shadps4-arm64-main'
build = args.build_dir.resolve()
line = next(line.strip().removeprefix('INCLUDES = ') for line in
            (build / 'build.ninja').read_text().splitlines()
            if line.strip().startswith('INCLUDES = '))
includes = shlex.split(line.replace('-imsvc', '-I'))
exe = build / ('shader-module-metadata-test.exe' if os.name == 'nt' else 'shader-module-metadata-test')
command = [args.compiler, '-std=c++23', '-O1', '-ffunction-sections', '-DNDEBUG',
           '-DFMT_HEADER_ONLY', '-I' + str(root / 'AI_Debug/tests/cxa_stubs'), *includes,
           str(root / 'AI_Debug/tests/shader_module_metadata_test.cpp')]
command += ['-Xlinker', '/OPT:REF'] if os.name == 'nt' else ['-Wl,--gc-sections']
subprocess.run([*command, '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)

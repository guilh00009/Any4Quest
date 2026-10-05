#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Asset-free test of the actual GetVideoData methods extracted from production.

Mocks only queues, playback clock/state, allocation storage and logging; no FFmpeg,
game assets or headset. Run with a configured C++23 compiler (CXX or --cxx).
"""
from pathlib import Path
import argparse, os, subprocess, tempfile
p=argparse.ArgumentParser();p.add_argument('--cxx',default=os.environ.get('CXX','c++'));p.add_argument('--source',type=Path);a=p.parse_args()
r=Path(__file__).resolve().parents[2]
source=(a.source or r/'shadps4-arm64-main/src/core/libraries/avplayer/avplayer_source.cpp').read_text()
start=source.index('bool AvPlayerSource::GetVideoData(AvPlayerFrameInfo&')
end=source.index('bool AvPlayerSource::GetAudioData(',start)
methods=source[start:end]
fixture=Path(__file__).with_name('avplayer_video_lifetime_fixture.cpp').read_text()
assert fixture.count('// PRODUCTION_METHODS')==1
assert 'std::min(std::max(2, init_data.num_output_video_framebuffers), 16)' in source
with tempfile.TemporaryDirectory(prefix='avplayer-frame-lifetime-') as tmp:
    cpp=Path(tmp)/'test.cpp'; exe=Path(tmp)/('test.exe' if os.name=='nt' else 'test')
    cpp.write_text(fixture.replace('// PRODUCTION_METHODS',methods))
    subprocess.run([a.cxx,'-std=c++23','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
                    '-I'+str(r/'tools/tests/any4quest_stubs'),
                    '-I'+str(r/'shadps4-arm64-main/src'),str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)

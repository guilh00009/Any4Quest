# SPDX-License-Identifier: GPL-2.0-or-later
# Windows runner: existing clang++ in PATH, vcvars64 initialized; no sanitizers.
from pathlib import Path
import sys, tempfile, subprocess, csv
repo=Path(__file__).resolve().parents[2];root=repo/'shadps4-arm64-main'
sys.path.insert(0,str(root/'tests/libraries'))
import test_move_runtime_integration as test
stubs=dict(test.STUBS)
stubs['core/libraries/kernel/time.h']='#pragma once\n#include "common/types.h"\nnamespace Libraries::Kernel { u64 PS4_SYSV_ABI sceKernelGetProcessTime(); }\n'
source=('#include "core/vr/move_capture.h"\n'+test.TEST).replace('setenv("SHADPS4_VR_INPUT_MODE", value, 1);','_putenv_s("SHADPS4_VR_INPUT_MODE", value);').replace('setenv("SHADPS4_VR", "0", 1);','_putenv_s("SHADPS4_VR", "0");')
anchor='    std::cout << "PASS: real host state'
assert anchor in source
extra='''    Mode("move");
    _putenv_s("SHADPS4_MOVE_DIAGNOSTICS", "1");
    head={}; head.tracked=true;
    head.pose.position={1.0f,1.6f,2.0f};
    runtime.UpdateHead(head); runtime.RecenterSeat();
    for (unsigned i=0;i<240;++i) {
        process_time += 13889;
        for (unsigned hand=0;hand<2;++hand) {
            MoveHostState motion{}; motion.connected=motion.device.tracked=true;
            motion.device.pose.position={1.0f+(hand?.25f:-.25f),1.6f-.4f+float(i)*.002f,1.5f};
            runtime.UpdateMove(hand,motion);
            auto read=runtime.GetMove(hand);
            assert(read.device.tracked);
            assert(Near(read.device.pose.position.x,hand?.25f:-.25f));
            assert(Near(read.device.pose.position.y,-.4f+float(i)*.002f));
            assert(Near(read.device.pose.position.z,1.0f));
        }
    }
    std::cout << "PASS 480 dual-hand trajectory samples: exact grip-to-tracker position, no lag/droop\\n";
'''
source=source.replace(anchor,extra+anchor)
source=source.replace('    auto& runtime = Runtime::Instance();', '    _putenv_s("SHADPS4_MOVE_DIAGNOSTICS", "1");\n    auto& runtime = Runtime::Instance();')
with tempfile.TemporaryDirectory(prefix='move-runtime-windows-') as tmp:
 w=Path(tmp)
 for n,c in stubs.items():
  p=w/n;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(c)
 capture_path=w/'capture.csv'
 capture_source=source.replace('int main() {', 'int main() {\n    assert(Core::Vr::Diagnostics::Capture::Instance().Start(R"CAP('+str(capture_path)+')CAP"));',1)
 capture_source=capture_source.replace(anchor, '    Core::Vr::Diagnostics::Capture::Instance().Stop();\n    Core::Vr::Diagnostics::Capture::Instance().Wait();\n'+anchor,1)
 (w/'test.cpp').write_text(capture_source)
 exe=w/'integration-test.exe'
 subprocess.run(['clang++','-std=c++23','-D_CRT_SECURE_NO_WARNINGS','-O1','-Wall','-Wextra','-Werror','-Wno-unused-variable','-Wno-unused-parameter','-Wno-missing-field-initializers','-I'+str(w),'-I'+str(repo/'AI_Debug/tests/any4quest_stubs'),'-I'+str(root/'src'),'-I'+str(root/'externals/json/single_include'),str(w/'test.cpp'),str(root/'src/core/libraries/move/move.cpp'),str(root/'src/core/libraries/vr_tracker/vr_tracker.cpp'),str(root/'src/core/vr/vr_runtime.cpp'),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)



 with capture_path.open() as stream:
  stream.readline()
  records=list(csv.DictReader(stream))
 runtime_rows=[r for r in records if r['kind']=='2']
 tracker_rows=[r for r in records if r['kind']=='3']
 assert len(runtime_rows)>=480 and tracker_rows
 last=runtime_rows[-1]
 assert abs(float(last['v14'])-.25)<1e-5 and abs(float(last['v15'])-.078)<1e-5 and abs(float(last['v16'])-1)<1e-5
 assert any(abs(float(r['v28'])-1)<1e-5 for r in runtime_rows)
 assert all(int(r['returned_us'])>0 for r in tracker_rows)
 print('PASS active diagnostic capture: unchanged real runtime/tracker behavior, exported pose/times/IMU; rows',len(records))

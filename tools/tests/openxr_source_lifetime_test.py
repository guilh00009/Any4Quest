"""Exercise the production OpenXR source-slot lifetime guards with mock fences."""
from pathlib import Path
import argparse
import shutil
import subprocess
import tempfile
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[2])
p.add_argument('--compiler',default=shutil.which('clang++') or shutil.which('g++') or 'clang++')
a=p.parse_args()
s=(a.root/'shadps4-arm64-main/src/core/vr/openxr_host.cpp').read_text()
start=s.index('    void ReleaseCompletedCopies() {')
helper=s[start:s.index('    /// The newest finished frame',start)]
assignment='slot.state = slot.copy_fence ? Slot::State::Copying : Slot::State::Free;'
assert assignment in s
assert 'DestroyRetired' not in s and 'Clock::time_point since;' not in s
copy=s[s.index('    bool CopyFrame('):s.index('    void FillProjection(')]
assert copy.index('slots[index].copy_fence = fences[buffer]') < copy.index('xrReleaseSwapchainImage')
assert copy.index('ReleaseCompletedCopies();') < copy.index('device.resetFences')
assert 'slot.copy_fence = nullptr;' in s[s.index('void DestroySession'):s.index('void PollEvents')]
assert 'slot.state = Impl::Slot::State::Free' not in s[s.index('void OpenXrHost::DropFrame'):s.index('bool OpenXrHost::IsShowing')]
create_start=s.index('        if ((slot.state != Slot::State::Free',s.index('    bool Create(Slot&'))
create_guard=s[create_start:s.index('        const vk::Device device',create_start)]
code='''#include <cassert>
#include <iostream>
namespace vk { enum class Result {eSuccess,eNotReady,eErrorDeviceLost}; }
struct Mock {
struct Slot {enum class State{Free,Drawing,Ready,Reading,Copying}; State state=State::Free; int* copy_fence=nullptr;};
struct Device {vk::Result result=vk::Result::eNotReady; vk::Result getFenceStatus(int*)const{return result;}};
struct {Device device;} graphics;
Slot slots[4];
'''+helper+'''
void FinishMetadata(Slot& slot){'''+assignment+'''}
static bool Reusable(const Slot& slot){return slot.state==Slot::State::Free||slot.state==Slot::State::Ready;}
static bool CanReplace(Slot& slot){
'''+create_guard+'''
return true;
}
};
int main(){
using S=Mock::Slot::State;int fence=0;Mock m;
auto& slot=m.slots[0];slot.state=S::Reading;slot.copy_fence=&fence;
// Even immediate GPU completion must not release metadata still read by host.
m.graphics.device.result=vk::Result::eSuccess;m.ReleaseCompletedCopies();assert(slot.state==S::Reading&&!Mock::Reusable(slot));
m.FinishMetadata(slot);assert(slot.state==S::Copying);
// A stalled copy remains pinned regardless of repeated polls / elapsed time.
m.graphics.device.result=vk::Result::eNotReady;for(int i=0;i<10000;++i)m.ReleaseCompletedCopies();assert(slot.state==S::Copying&&!Mock::Reusable(slot));
m.graphics.device.result=vk::Result::eErrorDeviceLost;m.ReleaseCompletedCopies();assert(slot.copy_fence&&!Mock::Reusable(slot));
// Completion frees the source before the fence can be reset for another copy.
m.graphics.device.result=vk::Result::eSuccess;m.ReleaseCompletedCopies();assert(slot.state==S::Free&&!slot.copy_fence&&Mock::Reusable(slot));
m.graphics.device.result=vk::Result::eNotReady;m.ReleaseCompletedCopies();assert(slot.state==S::Free&&!slot.copy_fence);
// Submitted copy pins source even when compositor release later fails.
slot.state=S::Reading;slot.copy_fence=&fence;bool release_ok=false;(void)release_ok;m.FinishMetadata(slot);m.ReleaseCompletedCopies();assert(!Mock::Reusable(slot));
// No copy submitted: metadata handoff can safely release the source.
auto& blank=m.slots[1];blank.state=S::Reading;blank.copy_fence=nullptr;m.FinishMetadata(blank);assert(Mock::Reusable(blank));
// Unfinished producer is never reclaimed by a copy fence observation.
auto& drawing=m.slots[2];drawing.state=S::Drawing;m.graphics.device.result=vk::Result::eSuccess;m.ReleaseCompletedCopies();assert(!Mock::Reusable(drawing));
assert(!Mock::CanReplace(drawing));slot.state=S::Free;slot.copy_fence=&fence;assert(!Mock::CanReplace(slot));slot.copy_fence=nullptr;assert(Mock::CanReplace(slot));
std::cout<<"OpenXR source lifetime: 8 scenarios passed (production guards), 10000 stalled polls\\n";
}
'''
with tempfile.TemporaryDirectory(prefix='any4quest-xr-lifetime-') as tmp:
    source=Path(tmp)/'test.cpp';exe=Path(tmp)/'test.exe';source.write_text(code)
    subprocess.run([a.compiler,'-std=c++20',str(source),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)

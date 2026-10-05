# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile exact production guard blocks against fault-injected API mocks."""
from pathlib import Path
import argparse
import shutil
import subprocess
import tempfile
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[2])
parser.add_argument('--compiler',default=shutil.which('clang++') or shutil.which('g++') or 'clang++')
args=parser.parse_args()
root=args.root
s=(root/'shadps4-arm64-main/src/core/vr/openxr_host.cpp').read_text()
fence=s[s.index('        const vk::Device device = graphics.device;',s.index('    bool CopyFrame(')):s.index('        if (!EnsureSwapchain(width, height))',s.index('    bool CopyFrame('))]
wait=s[s.index('        if (result != XR_SUCCESS)',s.index('    bool CopyFrame(')):s.index('        const vk::Image target',s.index('    bool CopyFrame('))]
drain=s[s.index('        std::vector<vk::Fence> initialized_fences;',s.index('    void DestroySession(')):s.index('        DestroySwapchain();',s.index('    void DestroySession('))]
resize_start=s.index('        if (swapchain != XR_NULL_HANDLE) {',s.index('    bool EnsureSwapchain('))
resize=s[resize_start:s.index('\n        uint32_t count = 0;',resize_start)]
assert s.index('ReleaseControllers("the headset\'s session ended")',s.index('    void DestroySession(')) < s.index('if (command_pool',s.index('    void DestroySession('))
assert "wait.timeout = 1'000'000'000;" in s
assert s.index('fence_wait =',s.index('bool CopyFrame')) < s.index('xrAcquireSwapchainImage(',s.index('bool CopyFrame'))
assert '&& !impl->retained_gpu_resources' in s
assert '!exit_requested && !retained_gpu_resources' in s
code='''#include <cassert>
#include <iostream>
#include <vector>
#define LOG_ERROR(...) ((void)0)
namespace vk { enum class Result {eSuccess, eTimeout, eErrorDeviceLost};
using Fence=unsigned;
struct Device { Result result; template<class T> Result waitForFences(T, bool, unsigned long long) const {return result;}
Result waitForFences(const std::vector<Fence>& handles,bool,unsigned long long) const {assert(!handles.empty()); for(auto h:handles)assert(h); return result;} }; }
using u32=unsigned; constexpr int XR_SUCCESS=0, XR_NULL_HANDLE=0;
struct Mock {
struct {vk::Device device;} graphics;
unsigned fences[4]{1,2,3,4}, next_command_buffer{}; bool command_pool=true;
bool accepting=true,have_frame=true,session_lost=false,retained_gpu_resources=false;
int copies=0,resets=0,releases=0,acquires=0,destroys=0,failures=0;
int swapchain=1;
void DestroySwapchain(){++destroys;swapchain=0;}
void NoteFrameFailure(const char*,int){++failures;}
bool Copy(int result) {
'''+fence+'''
++acquires;
'''+wait+'''
++resets; ++copies; ++releases; return true;
}
void Cleanup() { accepting=false; have_frame=false;
'''+drain+'''
++destroys;
}
bool Resize(){
'''+resize+'''
return true;
}
};
int main(){
for(auto result:{vk::Result::eTimeout,vk::Result::eErrorDeviceLost}) {
Mock m{{{result}}}; assert(!m.Copy(0)); assert(!m.acquires&&!m.resets&&!m.copies&&!m.releases);
m.Cleanup(); assert(m.retained_gpu_resources&&!m.destroys&&!m.accepting&&!m.have_frame);
}
for(int result:{-1,1}) {Mock m{{{vk::Result::eSuccess}}};
assert(!m.Copy(result)); assert(m.acquires==1&&!m.resets&&!m.copies&&!m.releases);
assert(m.session_lost&&!m.accepting&&!m.have_frame);m.Cleanup();assert(m.destroys==1&&!m.retained_gpu_resources);}
Mock good{{{vk::Result::eSuccess}}};assert(good.Copy(0));assert(good.copies==1&&good.resets==1&&good.releases==1);good.Cleanup();assert(good.destroys==1&&!good.retained_gpu_resources);
Mock retry{{{vk::Result::eTimeout}}};assert(!retry.Copy(0));retry.graphics.device.result=vk::Result::eSuccess;assert(retry.Copy(0));assert(retry.acquires==1&&retry.copies==1);
for(auto result:{vk::Result::eTimeout,vk::Result::eErrorDeviceLost}) {Mock m{{{result}}};assert(!m.Resize());assert(m.swapchain==1&&!m.destroys&&m.have_frame);}
Mock resize{{{vk::Result::eSuccess}}};assert(resize.Resize());assert(resize.swapchain==0&&resize.destroys==1&&!resize.have_frame);
Mock partial{{{vk::Result::eSuccess}}};partial.fences[2]=partial.fences[3]=0;partial.Cleanup();assert(partial.destroys==1&&!partial.retained_gpu_resources);
Mock empty{{{vk::Result::eTimeout}}};for(auto& f:empty.fences)f=0;empty.Cleanup();assert(empty.destroys==1&&!empty.retained_gpu_resources);
std::cout<<"OpenXR wait fault injection: 11 scenarios passed (production guard blocks)\\n";
}
'''
with tempfile.TemporaryDirectory(prefix='any4quest-xr-waits-') as tmp:
    source=Path(tmp)/'xr-waits-test.cpp'
    executable=Path(tmp)/'xr-waits-test.exe'
    source.write_text(code)
    subprocess.run([args.compiler,'-std=c++20',str(source),'-o',str(executable)],check=True)
    subprocess.run([str(executable)],check=True)

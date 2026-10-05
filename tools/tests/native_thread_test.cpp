// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>
#include <atomic>
#include <cstdio>
#include "core/thread.h"

// Test-local fatal assertion handler; failures must still terminate the test.
void assert_fail_impl() { std::abort(); }

struct State {
    Core::NativeThread thread;
    std::atomic<bool> announced{};
    std::atomic<bool> finish{};
};
DWORD Worker(void* arg) {
    auto& state = *static_cast<State*>(arg);
    state.announced.store(true);
    while (!state.finish.load()) SwitchToThread();
    state.thread.Exit();
    std::abort();
}
DWORD FastWorker(void* arg) {
    static_cast<Core::NativeThread*>(arg)->Exit();
    std::abort();
}
int main() {
    DWORD before{}, after{};
    GetProcessHandleCount(GetCurrentProcess(), &before);
    for (unsigned i = 0; i < 2000; ++i) {
        State state;
        if (state.thread.Create(Worker, &state)) return 1;
        while (!state.announced.load()) SwitchToThread();
        if (state.thread.HasExited()) return 2;
        state.finish.store(true);
        const auto handle = reinterpret_cast<HANDLE>(state.thread.GetHandle());
        if (WaitForSingleObject(handle, 5000) != WAIT_OBJECT_0) return 3;
        if (!state.thread.HasExited()) return 4;
    }
    for (unsigned i = 0; i < 2000; ++i) {
        Core::NativeThread thread;
        if (thread.Create(FastWorker, &thread)) return 6;
        if (WaitForSingleObject(reinterpret_cast<HANDLE>(thread.GetHandle()), 5000) != WAIT_OBJECT_0) return 7;
        if (!thread.HasExited()) return 8;
    }
    GetProcessHandleCount(GetCurrentProcess(), &after);
    if (after > before + 2) return 5;
    std::printf("PASS 4000 native thread lifecycles; handles before=%lu after=%lu\n", before, after);
}

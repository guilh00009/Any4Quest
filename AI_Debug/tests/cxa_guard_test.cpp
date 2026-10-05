// SPDX-License-Identifier: GPL-2.0-or-later
// Build with libc_internal_cxa.cpp and AI_Debug/tests/cxa_stubs first on the include path.
#include <atomic>
#include <barrier>
#include <cstdio>
#include <thread>
#include <vector>
#include "core/libraries/libc_internal/libc_internal_cxa.h"
#include "core/libraries/libs.h"
using namespace Libraries::LibcInternal;
static int failures;
static void Check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    failures += !ok;
}
int main() {
    Core::Loader::SymbolsResolver resolver;
    RegisterLibcCxaGuards(&resolver);
    Check(resolver.exports.size() == 6, "both libc namespaces expose all three guards");
    Check(resolver.exports.contains("libSceLibcInternal:3GPpjQdAMTw"), "NP Toolkit acquire import resolves");
    Check(resolver.exports.contains("libSceLibcInternal:9rAeANT2tyE"), "NP Toolkit release import resolves");
    u64 guard = 0;
    Check(fex_libc_cxa_guard_acquire(&guard) == 1, "first acquire requests construction");
    Check(reinterpret_cast<u8*>(&guard)[0] == 0, "in-progress guard is not initialized");
    fex_libc_cxa_guard_abort(&guard);
    Check(fex_libc_cxa_guard_acquire(&guard) == 1, "abort permits constructor retry");
    fex_libc_cxa_guard_release(&guard);
    Check(reinterpret_cast<u8*>(&guard)[0] == 1, "release sets ABI initialized byte");
    Check(fex_libc_cxa_guard_acquire(&guard) == 0, "completed guard skips construction");
    u64 shared_guard = 0;
    int payload = 0;
    std::atomic<int> constructions{0}, observed{0};
    std::barrier start{8};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&] {
            start.arrive_and_wait();
            if (fex_libc_cxa_guard_acquire(&shared_guard)) {
                ++constructions;
                payload = 42;
                fex_libc_cxa_guard_release(&shared_guard);
            }
            if (payload == 42) ++observed;
        });
    }
    for (auto& thread : threads) thread.join();
    Check(constructions == 1, "contending threads construct exactly once");
    Check(observed == 8, "release publishes constructed payload to all threads");
    u64 other = 0;
    Check(fex_libc_cxa_guard_acquire(&other) == 1, "independent static may initialize");
    fex_libc_cxa_guard_release(&other);
    return failures ? 1 : 0;
}

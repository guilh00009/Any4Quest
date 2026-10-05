// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/alignment.h"
#include "common/arch.h"
#include "common/assert.h"
#ifdef _WIN64
#include <windows.h>
#else
#include <csignal>
#include <pthread.h>
#include <unistd.h>
#ifdef ARCH_X86_64
#include <xmmintrin.h>
#endif
#endif

#include "thread.h"

namespace Core {

static constexpr u32 ORBIS_MXCSR = 0x9fc0;
static constexpr u32 ORBIS_FPUCW = 0x037f;

NativeThread::NativeThread() : native_handle{0} {}

NativeThread::~NativeThread() {
#ifdef _WIN64
    if (native_handle) {
        CloseHandle(native_handle);
    }
#endif
}

int NativeThread::Create(ThreadFunc func, void* arg) {
#ifndef _WIN64
    pthread_t* pthr = reinterpret_cast<pthread_t*>(&native_handle);
    return pthread_create(pthr, nullptr, func, arg);
#else
    native_handle = CreateThread(nullptr, 0, func, arg, CREATE_SUSPENDED, nullptr);
    if (native_handle == nullptr) {
        return GetLastError();
    }
    // Publish the handle before the callback can finish and be collected.
    const auto previous_count = ResumeThread(native_handle);
    ASSERT(previous_count != static_cast<DWORD>(-1));
    return 0;
#endif
}

void NativeThread::Exit() {
    if (!native_handle) {
        return;
    }

    tid = 0;

#ifdef _WIN64
    // Retain the handle until collection observes actual OS-thread completion.
    ExitThread(0);
#else
    // Disable and free the signal stack.
    constexpr stack_t sig_stack = {
        .ss_flags = SS_DISABLE,
    };
    sigaltstack(&sig_stack, nullptr);

    if (sig_stack_ptr) {
        free(sig_stack_ptr);
        sig_stack_ptr = nullptr;
    }
    pthread_exit(nullptr);
#endif
}

#ifdef _WIN64
bool NativeThread::HasExited() const {
    return native_handle && WaitForSingleObject(native_handle, 0) == WAIT_OBJECT_0;
}
#endif

void NativeThread::Initialize() {
#ifdef ARCH_X86_64
    // Set MXCSR and FPUCW registers to the values used by Orbis.
    _mm_setcsr(ORBIS_MXCSR);
    asm volatile("fldcw %0" : : "m"(ORBIS_FPUCW));
#endif
#if _WIN64
    tid = GetCurrentThreadId();
#else
    tid = (u64)pthread_self();

    // Set up an alternate signal handler stack to avoid overflowing small thread stacks.
    const size_t page_size = getpagesize();
    const size_t sig_stack_size = Common::AlignUp(std::max<size_t>(64_KB, MINSIGSTKSZ), page_size);
    ASSERT_MSG(posix_memalign(&sig_stack_ptr, page_size, sig_stack_size) == 0,
               "Failed to allocate signal stack: {}", errno);

    stack_t sig_stack;
    sig_stack.ss_sp = sig_stack_ptr;
    sig_stack.ss_size = sig_stack_size;
    sig_stack.ss_flags = 0;
    ASSERT_MSG(sigaltstack(&sig_stack, nullptr) == 0, "Failed to set signal stack: {}", errno);
#endif
}

} // namespace Core

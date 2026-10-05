// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::LibcInternal {

int PS4_SYSV_ABI fex_libc_cxa_guard_acquire(u64* guard_object);
void PS4_SYSV_ABI fex_libc_cxa_guard_release(u64* guard_object);
void PS4_SYSV_ABI fex_libc_cxa_guard_abort(u64* guard_object);

int PS4_SYSV_ABI fex_libc_cxa_atexit(void (*func)(void*), void* arg, void* dso_handle);

// Register only when using HLE libc; native guest libc retains its own guards.
void RegisterLibcCxaGuards(Core::Loader::SymbolsResolver* sym);

void RegisterFexLibcCxaAliases(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::LibcInternal

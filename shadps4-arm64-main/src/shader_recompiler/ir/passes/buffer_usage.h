// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/assert.h"
#include "shader_recompiler/ir/opcodes.h"

namespace Shader::Optimization {
constexpr bool IsBufferAccess(IR::Opcode opcode) {
    switch (opcode) {
    case IR::Opcode::BufferAtomicIAdd32:
    case IR::Opcode::BufferAtomicIAdd64:
    case IR::Opcode::BufferAtomicISub32:
    case IR::Opcode::BufferAtomicSMin32:
    case IR::Opcode::BufferAtomicSMin64:
    case IR::Opcode::BufferAtomicUMin32:
    case IR::Opcode::BufferAtomicUMin64:
    case IR::Opcode::BufferAtomicFMin32:
    case IR::Opcode::BufferAtomicSMax32:
    case IR::Opcode::BufferAtomicSMax64:
    case IR::Opcode::BufferAtomicUMax32:
    case IR::Opcode::BufferAtomicUMax64:
    case IR::Opcode::BufferAtomicFMax32:
    case IR::Opcode::BufferAtomicInc32:
    case IR::Opcode::BufferAtomicDec32:
    case IR::Opcode::BufferAtomicAnd32:
    case IR::Opcode::BufferAtomicOr32:
    case IR::Opcode::BufferAtomicXor32:
    case IR::Opcode::BufferAtomicSwap32:
    case IR::Opcode::BufferAtomicCmpSwap32:
    case IR::Opcode::BufferAtomicFCmpSwap32:
    case IR::Opcode::StoreBufferU8:
    case IR::Opcode::StoreBufferU16:
    case IR::Opcode::StoreBufferU32:
    case IR::Opcode::StoreBufferU32x2:
    case IR::Opcode::StoreBufferU32x3:
    case IR::Opcode::StoreBufferU32x4:
    case IR::Opcode::StoreBufferU64:
    case IR::Opcode::StoreBufferF32:
    case IR::Opcode::StoreBufferF32x2:
    case IR::Opcode::StoreBufferF32x3:
    case IR::Opcode::StoreBufferF32x4:
    case IR::Opcode::StoreBufferFormatF32:
    case IR::Opcode::LoadBufferU8:
    case IR::Opcode::LoadBufferU16:
    case IR::Opcode::LoadBufferU32:
    case IR::Opcode::LoadBufferU32x2:
    case IR::Opcode::LoadBufferU32x3:
    case IR::Opcode::LoadBufferU32x4:
    case IR::Opcode::LoadBufferU64:
    case IR::Opcode::LoadBufferF32:
    case IR::Opcode::LoadBufferF32x2:
    case IR::Opcode::LoadBufferF32x3:
    case IR::Opcode::LoadBufferF32x4:
    case IR::Opcode::LoadBufferFormatF32:
    case IR::Opcode::ReadConstBuffer:
        return true;
    default:
        return false;
    }
}

// Keep resource indices and format metadata intact: specialization still needs
// descriptors whose format lowering removed every actual memory access.
inline void CollectBufferUsage(auto& buffers, const auto& blocks) {
    for (auto& buffer : buffers) {
        buffer.is_used = buffer.IsSpecial();
    }
    for (const auto* block : blocks) {
        for (const auto& inst : block->Instructions()) {
            if (IsBufferAccess(inst.GetOpcode())) {
                const auto index = inst.Arg(0).U32();
                ASSERT(index < buffers.size());
                buffers[index].is_used = true;
            }
        }
    }
}
} // namespace Shader::Optimization
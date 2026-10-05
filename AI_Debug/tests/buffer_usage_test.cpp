// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cstdlib>
#include <cstdio>
#include <vector>
#include "shader_recompiler/ir/passes/buffer_usage.h"
void assert_fail_impl() { std::abort(); }
[[noreturn]] void unreachable_impl() { std::abort(); }
using Shader::IR::Opcode;
struct Operand { u32 index; u32 U32() const { return index; } };
struct Instruction {
    Opcode opcode; u32 binding;
    Opcode GetOpcode() const { return opcode; }
    Operand Arg(int) const { return {binding}; }
};
struct Block {
    std::vector<Instruction> instructions;
    const auto& Instructions() const { return instructions; }
};
struct Buffer {
    bool special = false, is_used = true;
    u32 sharp = 68;
    bool IsSpecial() const { return special; }
};
static int failures;
void Check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    failures += !ok;
}
int main() {
    std::array<Buffer, 6> buffers{};
    buffers[5].special = true;
    Block entry{{{Opcode::ReadConstBuffer, 0}, {Opcode::StoreBufferU32, 2},
                 {Opcode::BufferAtomicIAdd64, 3}, {Opcode::LoadBufferF32x4, 4}}};
    std::array blocks{&entry};
    Shader::Optimization::CollectBufferUsage(buffers, blocks);
    Check(buffers[0].is_used, "constant buffer remains bound");
    Check(!buffers[1].is_used, "lowered-away buffer has no memory binding");
    Check(buffers[1].sharp == 68 && buffers.size() == 6, "specialization descriptor and binding indices preserved");
    Check(buffers[2].is_used, "write-only buffer remains bound");
    Check(buffers[3].is_used, "atomic buffer remains bound");
    Check(buffers[4].is_used, "raw/typed lowerings retain live loads");
    Check(buffers[5].is_used, "special buffer retained");
    entry.instructions.clear();
    Shader::Optimization::CollectBufferUsage(buffers, blocks);
    Check(!buffers[0].is_used && !buffers[2].is_used && buffers[5].is_used,
          "usage is recomputed, including empty shaders");
    entry.instructions.push_back({Opcode::LoadBufferU32, 1});
    Shader::Optimization::CollectBufferUsage(buffers, blocks);
    Check(buffers[1].is_used, "later specialization can restore a live buffer");
    Check(!Shader::Optimization::IsBufferAccess(Opcode::ImageRead), "image handle is not a buffer handle");
    return failures ? 1 : 0;
}

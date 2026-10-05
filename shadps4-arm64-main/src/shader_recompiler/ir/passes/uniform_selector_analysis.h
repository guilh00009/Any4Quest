// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <bit>
#include <unordered_set>
#include "shader_recompiler/ir/program.h"

namespace Shader::Optimization {
class UniformSelectorBuilder {
public:
    Liveness::UniformSelector proof;
    int Build(IR::Value value, unsigned depth = 0) {
        using P = Liveness::UniformSelector;
        using O = IR::Opcode;
        if (depth > 64 || value.IsEmpty()) return -1;
        if (value.IsImmediate()) {
            if (value.Type() == IR::Type::U32) return Add({P::Op::Constant, 0, 0, value.U32()});
            if (value.Type() == IR::Type::F32) return Add({P::Op::Constant, 0, 0, std::bit_cast<u32>(value.F32())});
            return -1;
        }
        const auto* inst = value.Inst();
        if (!active.insert(inst).second) return -1;
        const int result = BuildInst(*inst, depth);
        active.erase(inst);
        return result;
    }
private:
    std::unordered_set<const IR::Inst*> active;
    int Add(Liveness::UniformSelector::Node node) {
        if (proof.count >= proof.nodes.size()) return -1;
        proof.nodes[proof.count] = node;
        return proof.count++;
    }
    int Binary(Liveness::UniformSelector::Op op, int a, int b) {
        if (a < 0 || b < 0) return -1;
        return Add({op, static_cast<u8>(a), static_cast<u8>(b), 0});
    }
    int BuildInst(const IR::Inst& inst, unsigned depth) {
        using P = Liveness::UniformSelector;
        using O = IR::Opcode;
        const auto arg = [&](unsigned n) { return Build(inst.Arg(n), depth + 1); };
        switch (inst.GetOpcode()) {
        case O::Identity:
        case O::BitCastF32U32:
        case O::BitCastU32F32: return arg(0);
        case O::ReadConstBuffer: {
            if (!inst.Arg(0).IsImmediate() || !inst.Arg(1).IsImmediate()) return -1;
            const auto buffer = inst.Arg(0).U32(), word = inst.Arg(1).U32();
            if ((proof.buffer != ~0u && proof.buffer != buffer) || word >= 16384) return -1;
            proof.buffer = buffer;
            proof.required_words = std::max(proof.required_words, word + 1);
            return Add({P::Op::Read, 0, 0, word});
        }
        case O::BitwiseAnd32: return Binary(P::Op::And, arg(0), arg(1));
        case O::BitwiseOr32: return Binary(P::Op::Or, arg(0), arg(1));
        case O::BitwiseXor32: return Binary(P::Op::Xor, arg(0), arg(1));
        case O::ShiftLeftLogical32: return Binary(P::Op::Left, arg(0), arg(1));
        case O::ShiftRightLogical32: return Binary(P::Op::Right, arg(0), arg(1));
        case O::Phi: {
            if (!inst.NumArgs()) return -1;
            int result = arg(0);
            for (unsigned i = 1; i < inst.NumArgs() && result >= 0; ++i)
                result = Binary(P::Op::Merge, result, arg(i));
            return result;
        }
        default: return -1;
        }
    }
};

inline void CollectUniformSelector(IR::Program& program) {
    using T = IR::AbstractSyntaxNode::Type;
    auto& info = program.info;
    info.uniform_selector = {};
    if (info.l_stage != LogicalStage::Vertex) return;
    std::array<IR::Value, 32> exports{};
    std::array<unsigned, 32> writes{};
    std::array<bool, 32> guaranteed{};
    std::unordered_set<const IR::Block*> visited;
    unsigned nesting = 0, returns = 0;
    bool terminated = false;
    for (const auto& node : program.syntax_list) {
        if (node.type == T::If || node.type == T::Loop) ++nesting;
        if (node.type == T::EndIf || node.type == T::Repeat) {
            if (!nesting) return;
            --nesting;
        }
        if (node.type == T::Return || node.type == T::Unreachable) {
            ++returns;
            terminated = true;
        }
        if (node.type != T::Block) continue;
        visited.insert(node.data.block);
        for (const auto& inst : node.data.block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::SetAttribute) continue;
            const auto attr = inst.Arg(0).Attribute();
            if (attr < IR::Attribute::Param0 || attr > IR::Attribute::Param31) continue;
            if (!inst.Arg(2).IsImmediate()) return;
            if (inst.Arg(2).U32() != 0) continue;
            const unsigned index = static_cast<u32>(attr) - static_cast<u32>(IR::Attribute::Param0);
            ++writes[index];
            exports[index] = inst.Arg(1);
            guaranteed[index] = !nesting && !terminated;
        }
    }
    if (nesting || returns != 1) return;
    for (const auto* block : program.blocks)
        if (!visited.contains(block))
            for (const auto& inst : block->Instructions())
                if (inst.GetOpcode() == IR::Opcode::SetAttribute) return;
    for (unsigned index = 0; index < 32; ++index) {
        if (writes[index] != 1 || !guaranteed[index]) continue;
        UniformSelectorBuilder builder;
        const int root = builder.Build(exports[index]);
        auto& proof = builder.proof;
        if (root < 0 || proof.buffer >= info.buffers.size()) continue;
        const auto& buffer = info.buffers[proof.buffer];
        if (buffer.IsSpecial() || buffer.is_written || !buffer.used_as_readconst) continue;
        proof.attribute = index;
        proof.root = root;
        info.uniform_selector = proof;
        return; // A single proved source is sufficient; others stay conservative.
    }
}
} // namespace Shader::Optimization

// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <bit>
#include <unordered_map>
#include <unordered_set>
#include "shader_recompiler/ir/passes/buffer_usage.h"
#include "shader_recompiler/ir/passes/known_bits.h"
#include "shader_recompiler/ir/program.h"

namespace Shader::Optimization {
// Only immutable SSA expressions with proved bit semantics are evaluated.
// Memory, Phi, arithmetic, and unsupported instructions remain unknown.
class LivenessEvaluator {
public:
    LivenessEvaluator(bool vertex_, unsigned input_, unsigned residue_)
        : vertex{vertex_}, input{input_}, residue{residue_} {}
    Liveness::Bits Eval(IR::Value value, unsigned depth = 0) {
        using B = Liveness::Bits;
        if (depth > 128 || value.IsEmpty())
            return {};
        if (value.IsImmediate()) {
            switch (value.Type()) {
            case IR::Type::U1:
                return B::Constant(value.U1());
            case IR::Type::U32:
                return B::Constant(value.U32());
            case IR::Type::F32:
                return B::Constant(std::bit_cast<u32>(value.F32()));
            default:
                return {};
            }
        }
        const auto* inst = value.Inst();
        if (const auto it = cache.find(inst); it != cache.end())
            return it->second;
        cache.emplace(inst, B{}); // Cycles cannot establish facts.
        const auto arg = [&](unsigned n) { return Eval(inst->Arg(n), depth + 1); };
        B result{};
        using O = IR::Opcode;
        switch (inst->GetOpcode()) {
        case O::Identity:
        case O::ConditionRef:
        case O::BitCastF32U32:
        case O::BitCastU32F32:
            result = arg(0);
            break;
        case O::GetAttributeU32:
            if (vertex && inst->Arg(0).Attribute() == IR::Attribute::InstanceId)
                result = B::Residue(residue);
            break;
        case O::GetAttribute:
            if (!vertex && inst->Arg(0).Attribute() == IR::Attribute::Param0 + input &&
                inst->Arg(1).IsImmediate() && inst->Arg(1).U32() == 0)
                result = B::Residue(residue);
            break;
        case O::BitwiseAnd32:
            result = Liveness::And(arg(0), arg(1));
            break;
        case O::BitwiseOr32:
            result = Liveness::Or(arg(0), arg(1));
            break;
        case O::BitwiseXor32:
            result = Liveness::Xor(arg(0), arg(1));
            break;
        case O::ShiftLeftLogical32:
            result = Liveness::Left(arg(0), arg(1));
            break;
        case O::ShiftRightLogical32:
            result = Liveness::Right(arg(0), arg(1));
            break;
        case O::IEqual32:
            result = Liveness::Equal(arg(0), arg(1));
            break;
        case O::INotEqual32:
            result = Liveness::Not(Liveness::Equal(arg(0), arg(1)));
            break;
        case O::LogicalNot:
            result = Liveness::Not(arg(0));
            break;
        case O::LogicalAnd:
            result = Liveness::LogicalAnd(arg(0), arg(1));
            break;
        case O::LogicalOr:
            result = Liveness::LogicalOr(arg(0), arg(1));
            break;
        default:
            break;
        }
        cache[inst] = result;
        return result;
    }

private:
    bool vertex;
    unsigned input, residue;
    std::unordered_map<const IR::Inst*, Liveness::Bits> cache;
};

inline void CollectResourceLiveness(IR::Program& program) {
    auto& info = program.info;
    info.instance_export_mask = 0;
    for (auto& buffer : info.buffers) {
        buffer.instance_input = 255;
        buffer.instance_mask = 15;
    }
    using T = IR::AbstractSyntaxNode::Type;
    if (info.l_stage == LogicalStage::Vertex) {
        u32 valid = ~0u, guaranteed = 0;
        unsigned nesting = 0, returns = 0;
        bool terminated = false;
        for (const auto& node : program.syntax_list) {
            if (node.type == T::If || node.type == T::Loop)
                ++nesting;
            if (node.type == T::EndIf || node.type == T::Repeat) {
                if (!nesting)
                    return;
                --nesting;
            }
            if (node.type == T::Return || node.type == T::Unreachable) {
                ++returns;
                terminated = true;
            }
            if (node.type != T::Block)
                continue;
            for (const auto& inst : node.data.block->Instructions()) {
                if (inst.GetOpcode() != IR::Opcode::SetAttribute)
                    continue;
                const auto attr = inst.Arg(0).Attribute();
                if (attr < IR::Attribute::Param0 || attr > IR::Attribute::Param31)
                    continue;
                if (!inst.Arg(2).IsImmediate()) {
                    valid = 0;
                    continue;
                }
                if (inst.Arg(2).U32() != 0)
                    continue;
                const u32 bit =
                    1u << (static_cast<u32>(attr) - static_cast<u32>(IR::Attribute::Param0));
                for (unsigned r = 0; r < 4; ++r) {
                    LivenessEvaluator eval{true, 0, r};
                    const auto bits = eval.Eval(inst.Arg(1));
                    if (((bits.zero | bits.one) & 3) != 3 || (bits.one & 3) != r)
                        valid &= ~bit;
                }
                if (!nesting && !terminated)
                    guaranteed |= bit;
            }
        }
        if (!nesting && returns == 1)
            info.instance_export_mask = valid & guaranteed;
        return;
    }
    if (info.l_stage != LogicalStage::Fragment)
        return;
    // Independently prove each candidate flat input. A union of every access
    // includes reads in subsequent loops and unconditional/unknown paths.
    for (unsigned input = 0; input < 32; ++input) {
        std::vector<unsigned> used(info.buffers.size());
        bool valid = true;
        for (unsigned r = 0; r < 4 && valid; ++r) {
            LivenessEvaluator eval{false, input, r};
            std::vector<bool> parents;
            bool reachable = true;
            std::unordered_set<const IR::Block*> visited;
            for (const auto& node : program.syntax_list) {
                if (node.type == T::If) {
                    parents.push_back(reachable);
                    reachable = reachable && eval.Eval(node.data.if_node.cond).Truth() != 0;
                } else if (node.type == T::EndIf) {
                    if (parents.empty()) {
                        valid = false;
                        break;
                    }
                    reachable = parents.back();
                    parents.pop_back();
                } else if (node.type == T::Block) {
                    visited.insert(node.data.block);
                    for (const auto& inst : node.data.block->Instructions()) {
                        if (!IsBufferAccess(inst.GetOpcode()))
                            continue;
                        if (!inst.Arg(0).IsImmediate() || inst.Arg(0).U32() >= used.size()) {
                            valid = false;
                            break;
                        }
                        if (reachable)
                            used[inst.Arg(0).U32()] |= 1u << r;
                    }
                }
                // Loop repetition, Break, Return are deliberately ignored:
                // this overestimates reachability and never excludes an access.
            }
            if (!parents.empty())
                valid = false;
            for (const auto* block : program.blocks) {
                if (visited.contains(block))
                    continue;
                for (const auto& inst : block->Instructions())
                    if (IsBufferAccess(inst.GetOpcode()))
                        valid = false;
            }
        }
        if (!valid)
            continue;
        for (unsigned b = 0; b < used.size(); ++b) {
            auto& buffer = info.buffers[b];
            if (!buffer.IsSpecial() && !buffer.is_written && used[b] != 15 &&
                buffer.instance_input == 255) {
                buffer.instance_input = input;
                buffer.instance_mask = used[b];
            }
        }
    }
}
} // namespace Shader::Optimization

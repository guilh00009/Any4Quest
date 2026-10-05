// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstring>
#include <span>
#include "shader_recompiler/ir/passes/known_bits.h"

namespace Shader::Liveness {
// A bounded, pointer-free proof, suitable for the shader metadata cache.
// Merge intersects known bits from every incoming SSA value: it never assumes
// which control-flow edge executes. Only one read-only buffer is supported.
struct UniformSelector {
    enum class Op : std::uint8_t { Constant, Read, And, Or, Xor, Left, Right, Merge };
    struct Node {
        Op op{};
        std::uint8_t a{}, b{};
        std::uint32_t value{};
    };
    std::array<Node, 64> nodes{};
    std::uint32_t buffer{~0u}, attribute{~0u};
    std::uint32_t count{}, root{}, required_words{};

    unsigned Mask(std::span<const std::byte> bytes) const {
        if (!count || count > nodes.size() || root >= count || attribute >= 32 ||
            required_words > bytes.size() / 4)
            return 15;
        std::array<Bits, 64> values{};
        for (unsigned i = 0; i < count; ++i) {
            const auto& n = nodes[i];
            if (n.op == Op::Constant) {
                values[i] = Bits::Constant(n.value);
            } else if (n.op == Op::Read) {
                if (n.value >= bytes.size() / 4) return 15;
                std::uint32_t word;
                std::memcpy(&word, bytes.data() + n.value * 4, 4);
                values[i] = Bits::Constant(word);
            } else {
                if (n.a >= i || n.b >= i) return 15;
                const auto a = values[n.a], b = values[n.b];
                switch (n.op) {
                case Op::And: values[i] = And(a, b); break;
                case Op::Or: values[i] = Or(a, b); break;
                case Op::Xor: values[i] = Xor(a, b); break;
                case Op::Left: values[i] = Left(a, b); break;
                case Op::Right: values[i] = Right(a, b); break;
                case Op::Merge: values[i] = {a.zero & b.zero, a.one & b.one}; break;
                default: return 15;
                }
            }
        }
        unsigned mask = 0;
        const auto bits = values[root];
        for (unsigned r = 0; r < 4; ++r)
            if (!(r & bits.zero & 3) && !((~r) & bits.one & 3)) mask |= 1u << r;
        return mask;
    }
};
} // namespace Shader::Liveness

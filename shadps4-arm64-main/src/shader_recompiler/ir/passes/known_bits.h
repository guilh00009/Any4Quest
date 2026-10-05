// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
namespace Shader::Liveness {
struct Bits {
    std::uint32_t zero{}, one{};
    static constexpr Bits Constant(std::uint32_t v) {
        return {~v, v};
    }
    static constexpr Bits Residue(unsigned v) {
        return {(~v) & 3u, v & 3u};
    }
    constexpr bool Complete() const {
        return (zero | one) == ~0u;
    }
    constexpr int Truth() const {
        return one ? 1 : zero == ~0u ? 0 : -1;
    }
};
constexpr Bits And(Bits a, Bits b) {
    return {a.zero | b.zero, a.one & b.one};
}
constexpr Bits Or(Bits a, Bits b) {
    return {a.zero & b.zero, a.one | b.one};
}
constexpr Bits Xor(Bits a, Bits b) {
    return {(a.zero & b.zero) | (a.one & b.one), (a.zero & b.one) | (a.one & b.zero)};
}
constexpr Bits Left(Bits a, Bits b) {
    if (!b.Complete() || b.one >= 32)
        return {};
    if (!b.one)
        return a;
    return {(a.zero << b.one) | ((1u << b.one) - 1), a.one << b.one};
}
constexpr Bits Right(Bits a, Bits b) {
    if (!b.Complete() || b.one >= 32)
        return {};
    if (!b.one)
        return a;
    return {(a.zero >> b.one) | (~0u << (32 - b.one)), a.one >> b.one};
}
constexpr Bits Equal(Bits a, Bits b) {
    if ((a.zero & b.one) || (a.one & b.zero))
        return Bits::Constant(0);
    if (a.Complete() && b.Complete())
        return Bits::Constant(a.one == b.one);
    return {};
}
constexpr Bits Not(Bits a) {
    return a.Truth() < 0 ? Bits{} : Bits::Constant(!a.Truth());
}
constexpr Bits LogicalAnd(Bits a, Bits b) {
    if (a.Truth() == 0 || b.Truth() == 0)
        return Bits::Constant(0);
    if (a.Truth() == 1 && b.Truth() == 1)
        return Bits::Constant(1);
    return {};
}
constexpr Bits LogicalOr(Bits a, Bits b) {
    if (a.Truth() == 1 || b.Truth() == 1)
        return Bits::Constant(1);
    if (a.Truth() == 0 && b.Truth() == 0)
        return Bits::Constant(0);
    return {};
}
struct InstanceRange {
    std::uint32_t first, count;
};
constexpr unsigned InstanceMask(InstanceRange range) {
    if (range.count >= 4)
        return 15;
    unsigned mask{};
    for (unsigned i = 0; i < range.count; ++i)
        mask |= 1u << ((range.first + i) & 3);
    return mask;
}
constexpr bool Inactive(unsigned used, unsigned drawn, unsigned input, unsigned exports,
                        unsigned offset, bool flat) {
    return input < 32 && offset < 32 && flat && (exports & (1u << offset)) && used <= 15 &&
           drawn <= 15 && !(used & drawn);
}
} // namespace Shader::Liveness

// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include <iostream>
#include <random>
#define LOG_DEBUG(...) ((void)0)
#include "shader_recompiler/ir/passes/resource_liveness.h"
#include "shader_recompiler/resource_proofs.h"
using namespace Shader;
using namespace Shader::IR;
using namespace Shader::Liveness;
static unsigned checks;
void Check(bool ok) {
  ++checks;
  if (!ok) {
    std::cerr << "FAIL " << checks << '\n';
    std::exit(1);
  }
}
void assert_fail_impl() { std::abort(); }
[[noreturn]] void unreachable_impl() { std::abort(); }
struct Fixture {
  Common::ObjectPool<Inst> pool;
  Info info;
  Program program{info};
  Block first{pool}, body{pool}, tail{pool};
  Fixture(LogicalStage stage) {
    info.l_stage = stage;
    info.buffers.emplace_back();
    program.blocks = {&first, &body, &tail};
  }
  Value Add(Block &b, Opcode op, std::initializer_list<Value> args) {
    b.AppendNewInst(op, args);
    return Value{&b.Instructions().back()};
  }
  void BlockNode(Block &b) {
    AbstractSyntaxNode n{};
    n.type = AbstractSyntaxNode::Type::Block;
    n.data.block = &b;
    program.syntax_list.push_back(n);
  }
  void Node(AbstractSyntaxNode::Type t) {
    AbstractSyntaxNode n{};
    n.type = t;
    program.syntax_list.push_back(n);
  }
  void If(Value cond) {
    AbstractSyntaxNode n{};
    n.type = AbstractSyntaxNode::Type::If;
    n.data.if_node.cond = U1{cond};
    program.syntax_list.push_back(n);
  }
};
int main() {
  std::mt19937 rng(1729);
  for (unsigned i = 0; i < 10000; ++i) {
    unsigned a = rng(), b = rng(), ka = rng(), kb = rng();
    Bits x{~a & ka, a & ka}, y{~b & kb, b & kb};
    auto sound = [&](Bits z, unsigned value) {
      Check(!(z.zero & value) && !(z.one & ~value));
    };
    sound(And(x, y), a & b);
    sound(Or(x, y), a | b);
    sound(Xor(x, y), a ^ b);
    unsigned shift = rng() % 32;
    sound(Left(x, Bits::Constant(shift)), a << shift);
    sound(Right(x, Bits::Constant(shift)), a >> shift);
    sound(Equal(x, y), a == b);
    sound(LogicalAnd(x, y), a && b);
    sound(LogicalOr(x, y), a || b);
  }
  for (unsigned first : {0u, 1u, 2u, 3u, 7u, 0xffffffffu})
    for (unsigned count = 0; count < 9; ++count) {
      unsigned expected = 0;
      for (unsigned i = 0; i < count; ++i)
        expected |= 1u << ((first + i) & 3);
      Check(InstanceMask({first, count}) == expected);
    }
  Check(!Inactive(8, 8, 2, 16, 4, true));
  Check(Inactive(8, 1, 2, 16, 4, true));
  Check(!Inactive(8, 1, 2, 16, 4, false));
  Check(!Inactive(8, 1, 2, 0, 4, true));
  Check(!Inactive(8, 15, 2, 16, 4, true));
  Check(!Inactive(8, 1, 255, 16, 4, true));
  using T = AbstractSyntaxNode::Type;
  using O = Opcode;
  {
    Fixture f{LogicalStage::Vertex};
    auto id = f.Add(f.first, O::GetAttributeU32,
                    {Value{Attribute::InstanceId}, Value{0u}});
    auto low = f.Add(f.first, O::BitwiseAnd32, {id, Value{3u}});
    auto unknown = f.Add(f.first, O::ReadConstBuffer, {Value{0u}, Value{0u}});
    auto high = f.Add(f.first, O::ShiftLeftLogical32, {unknown, Value{31u}});
    auto packed = f.Add(f.first, O::BitwiseOr32, {low, high});
    auto bits = f.Add(f.first, O::BitCastF32U32, {packed});
    f.Add(f.tail, O::SetAttribute, {Value{Attribute::Param4}, bits, Value{0u}});
    f.BlockNode(f.first);
    f.BlockNode(f.body);
    f.BlockNode(f.tail);
    f.Node(T::Return);
    Optimization::CollectResourceLiveness(f.program);
    Check(f.info.instance_export_mask == 16);
    f.Add(f.tail, O::SetAttribute,
          {Value{Attribute::Param4}, Value{0.0f}, Value{0u}});
    Optimization::CollectResourceLiveness(f.program);
    Check(f.info.instance_export_mask == 0);
  }
  for (unsigned input : {0u, 2u, 3u, 31u})
    for (unsigned selected = 0; selected < 4; ++selected) {
      Fixture f{LogicalStage::Fragment};
      auto attr =
          f.Add(f.first, O::GetAttribute,
                {Value{Attribute::Param0 + int(input)}, Value{0u}, Value{0u}});
      auto bits = f.Add(f.first, O::BitCastU32F32, {attr});
      auto low = f.Add(f.first, O::BitwiseAnd32, {bits, Value{3u}});
      auto neq = f.Add(f.first, O::INotEqual32, {low, Value{selected}});
      auto cond = f.Add(f.first, O::LogicalNot, {neq});
      f.Add(f.body, O::ReadConstBuffer, {Value{0u}, Value{0u}});
      f.BlockNode(f.first);
      f.If(cond);
      f.BlockNode(f.body);
      f.Node(T::EndIf);
      f.BlockNode(f.tail);
      f.Node(T::Return);
      Optimization::CollectResourceLiveness(f.program);
      Check(f.info.buffers[0].instance_input == input);
      Check(f.info.buffers[0].instance_mask == (1u << selected));
      f.Add(f.tail, O::ReadConstBuffer, {Value{0u}, Value{1u}});
      Optimization::CollectResourceLiveness(f.program);
      Check(f.info.buffers[0].instance_input == 255);
      Check(f.info.buffers[0].instance_mask == 15);
    }
  {
    Fixture f{LogicalStage::Fragment};
    auto memory = f.Add(f.first, O::ReadConstBuffer, {Value{0u}, Value{0u}});
    auto cond = f.Add(f.first, O::IEqual32, {memory, Value{3u}});
    f.Add(f.body, O::ReadConstBuffer, {Value{0u}, Value{1u}});
    f.BlockNode(f.first);
    f.If(cond);
    f.Node(T::Loop);
    f.BlockNode(f.body);
    f.Node(T::Repeat);
    f.Node(T::EndIf);
    f.BlockNode(f.tail);
    f.Node(T::Return);
    Optimization::CollectResourceLiveness(f.program);
    Check(f.info.buffers[0].instance_input ==
          255); // Unknown predicate is active.
  }
  {
    Fixture f{LogicalStage::Fragment};
    f.Add(f.body, O::ReadConstBuffer, {Value{0u}, Value{0u}});
    f.BlockNode(f.first);
    f.If(Value{false});
    f.BlockNode(f.body);
    f.Node(T::EndIf);
    f.BlockNode(f.tail);
    f.Node(T::Return);
    Optimization::CollectResourceLiveness(f.program);
    Check(f.info.buffers[0].instance_mask == 0);
    f.info.buffers[0].is_written = true;
    Optimization::CollectResourceLiveness(f.program);
    Check(f.info.buffers[0].instance_input == 255);
    f.info.buffers[0].is_written = false;
    f.program.syntax_list.erase(f.program.syntax_list.begin() +
                                2); // Unvisited access block.
    Optimization::CollectResourceLiveness(f.program);
    Check(f.info.buffers[0].instance_input == 255);
  }
  {
    Fixture f{LogicalStage::Vertex};
    auto id = f.Add(f.first, O::GetAttributeU32,
                    {Value{Attribute::InstanceId}, Value{0u}});
    auto bits = f.Add(f.first, O::BitCastF32U32, {id});
    f.Add(f.body, O::SetAttribute, {Value{Attribute::Param4}, bits, Value{0u}});
    f.BlockNode(f.first);
    f.If(Value{true});
    f.BlockNode(f.body);
    f.Node(T::EndIf);
    f.BlockNode(f.tail);
    f.Node(T::Return);
    Optimization::CollectResourceLiveness(f.program);
    Check(!f.info.instance_export_mask);
    f.program.syntax_list.insert(f.program.syntax_list.begin(),
                                 {.type = T::Return});
    Optimization::CollectResourceLiveness(f.program);
    Check(!f.info.instance_export_mask);
  }

  // Uniform metadata is proved from IR, independently of shader hashes,
  // descriptor slots, and export numbers. Phi unions include untaken inputs.
  for (unsigned attribute : {0u, 5u, 17u, 31u}) {
    Fixture f{LogicalStage::Vertex};
    f.info.buffers[0].used_as_readconst = true;
    auto enable = f.Add(f.first, O::ReadConstBuffer, {Value{0u}, Value{0u}});
    auto select = f.Add(f.first, O::ReadConstBuffer, {Value{0u}, Value{2u}});
    auto phi = f.Add(f.first, O::Phi, {});
    phi.Inst()->AddPhiOperand(&f.first, select);
    phi.Inst()->AddPhiOperand(&f.body, Value{0u});
    auto high = f.Add(f.first, O::ShiftLeftLogical32, {enable, Value{31u}});
    auto combined = f.Add(f.first, O::BitwiseOr32, {phi, high});
    auto bits = f.Add(f.first, O::BitCastF32U32, {combined});
    auto output = f.Add(f.tail, O::SetAttribute,
        {Value{Attribute::Param0 + attribute}, bits, Value{0u}});
    f.BlockNode(f.first); f.BlockNode(f.body); f.BlockNode(f.tail); f.Node(T::Return);
    Optimization::CollectResourceLiveness(f.program);
    const auto proof = f.info.uniform_selector;
    Check(proof.count && proof.buffer == 0 && proof.attribute == attribute);
    Check(proof.required_words == 3);
    for (unsigned i = 0; i < 1000; ++i) {
      std::array<u32, 3> words{rng(), 0, rng()};
      const unsigned mask = proof.Mask(std::as_bytes(std::span{words}));
      // Both actual predecessor choices must be included; no branch guessing.
      Check(mask & 1u);
      Check(mask & (1u << (words[2] & 3)));
      if ((words[2] & 3) == 0) Check(mask == 1);
    }
    std::array<u32, 3> words{0, 0, 2};
    Check(proof.Mask(std::as_bytes(std::span{words})) == 5);
    Check(proof.Mask(std::as_bytes(std::span{words}).first(8)) == 15);
    auto corrupt = proof; corrupt.count = 65;
    Check(corrupt.Mask(std::as_bytes(std::span{words})) == 15);
    corrupt = proof; corrupt.nodes[proof.root].a = 63;
    Check(corrupt.Mask(std::as_bytes(std::span{words})) == 15);
    f.info.buffers[0].is_written = true;
    Optimization::CollectResourceLiveness(f.program);
    Check(!f.info.uniform_selector.count);
    f.info.buffers[0].is_written = false;
    // Multiple writes and cyclic/unsupported expressions cannot prove exclusion.
    f.Add(f.tail, O::SetAttribute,
        {Value{Attribute::Param0 + attribute}, bits, Value{0u}});
    Optimization::CollectResourceLiveness(f.program);
    Check(!f.info.uniform_selector.count);
    phi.Inst()->AddPhiOperand(&f.tail, phi);
    Optimization::UniformSelectorBuilder cyclic;
    Check(cyclic.Build(phi) < 0);
    Optimization::UniformSelectorBuilder unknown;
    auto id = f.Add(f.first, O::GetAttributeU32, {Value{Attribute::InstanceId}, Value{0u}});
    Check(unknown.Build(id) < 0);
  }
  {
    Fixture f{LogicalStage::Vertex};
    f.info.buffers.emplace_back();
    f.info.buffers[0].used_as_readconst = f.info.buffers[1].used_as_readconst = true;
    auto a = f.Add(f.first, O::ReadConstBuffer, {Value{0u}, Value{0u}});
    auto b = f.Add(f.first, O::ReadConstBuffer, {Value{1u}, Value{0u}});
    auto both = f.Add(f.first, O::BitwiseOr32, {a,b});
    Optimization::UniformSelectorBuilder builder;
    Check(builder.Build(both) < 0); // No cross-buffer snapshot assumption.
    auto dynamic = f.Add(f.first, O::ReadConstBuffer, {Value{0u}, a});
    Optimization::UniformSelectorBuilder offsets;
    Check(offsets.Build(dynamic) < 0);
    f.Add(f.body, O::SetAttribute, {Value{Attribute::Param5}, a, Value{0u}});
    f.BlockNode(f.first); f.If(Value{true}); f.BlockNode(f.body); f.Node(T::EndIf);
    f.BlockNode(f.tail); f.Node(T::Return);
    Optimization::CollectResourceLiveness(f.program);
    Check(!f.info.uniform_selector.count); // Conditional export is not guaranteed.
  }

  {
    Fixture f{LogicalStage::Vertex};
    for (unsigned i = 0; i < 3; ++i) f.info.buffers.emplace_back();
    f.info.buffers[3].used_as_readconst = true;
    auto value = f.Add(f.first, O::ReadConstBuffer, {Value{3u}, Value{7u}});
    f.Add(f.tail, O::SetAttribute, {Value{Attribute::Param17}, value, Value{0u}});
    f.BlockNode(f.first); f.BlockNode(f.body); f.BlockNode(f.tail); f.Node(T::Return);
    Optimization::CollectResourceLiveness(f.program);
    Check(f.info.uniform_selector.count && f.info.uniform_selector.buffer == 3);
    Check(f.info.uniform_selector.attribute == 17 && f.info.uniform_selector.required_words == 8);
    std::array<u32,8> words{}; words[7] = 3;
    Check(f.info.uniform_selector.Mask(std::as_bytes(std::span{words})) == 8);
    InvalidateResourceProofs(f.info);
    Check(!f.info.resource_proofs_valid && !f.info.uniform_selector.count);
  }
  std::cout << "PASS " << checks
            << " checks; real IR adapter and known-bit soundness\n";
}

#pragma once
// === bc_defs.hpp ========================================
// the bytecode DEFINITION layer (split from vm.hpp, checkpoint 21 —
// the vm tripart needed a standalone base): the Bc opcode enum, bc_name
// (the listing), bc_rd_u32/bc_rd_i64/bc_put_u32 (image accessors), and
// bc_instr_size (per-instr encoding sizes). vm.hpp (the builder) and
// vm_step.hpp (the machine) both include this — one definition, two
// consumers, each standalone-compilable.

#include "../ir/instr.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

enum class Bc : uint8_t {
  Halt = 0,
  Push,
  Load,
  Store,
  Add,
  Sub,
  Mul,
  Div,
  Mod,
  Lt,
  Le,
  Gt,
  Ge,
  Eq,
  Ne,
  Jmp,
  Jz,
  Print,
  ArrNew,
  ArrLd,
  ArrSt,
  ArrLen,
  In,
  Call,
  Ret, // Call u32 fnidx u32 nargs; Ret (value via the operand stack)
  And,
  Or,
  Xor,
  Shl,
  Lshr,
  Asr,
  MulHi, // optimizer-lowered bit ops
  Depth, // push the frame count (call depth)
  Bkpt   // breakpoint v1 v2 ... vk: observation point; v* are SLOT ids
         // (u32 count follows the opcode). No effect on execution —
         // the debugger stops on it and reads the slots
};
constexpr const char *bc_name(Bc op) {
  switch (op) {
  case Bc::Halt:
    return "halt";
  case Bc::Push:
    return "push";
  case Bc::Load:
    return "load";
  case Bc::Store:
    return "store";
  case Bc::Add:
    return "add";
  case Bc::Sub:
    return "sub";
  case Bc::Mul:
    return "mul";
  case Bc::Div:
    return "div";
  case Bc::Mod:
    return "mod";
  case Bc::Lt:
    return "lt";
  case Bc::Le:
    return "le";
  case Bc::Gt:
    return "gt";
  case Bc::Ge:
    return "ge";
  case Bc::Eq:
    return "eq";
  case Bc::Ne:
    return "ne";
  case Bc::Jmp:
    return "jmp";
  case Bc::Jz:
    return "jz";
  case Bc::Print:
    return "print";
  case Bc::ArrNew:
    return "arrnew";
  case Bc::ArrLd:
    return "arrld";
  case Bc::ArrSt:
    return "arrst";
  case Bc::ArrLen:
    return "arrlen";
  case Bc::In:
    return "in";
  case Bc::Call:
    return "call";
  case Bc::Ret:
    return "ret";
  case Bc::And:
    return "and";
  case Bc::Or:
    return "or";
  case Bc::Xor:
    return "xor";
  case Bc::Shl:
    return "shl";
  case Bc::Lshr:
    return "lshr";
  case Bc::Asr:
    return "asr";
  case Bc::MulHi:
    return "mulhi";
  case Bc::Depth:
    return "depth";
  case Bc::Bkpt:
    return "bkpt";
  }
  return "?";
}
constexpr void bc_put_u32(std::vector<uint8_t> &c, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    c.push_back((uint8_t)(v & 0xff));
    v >>= 8;
  }
}
constexpr void bc_put_i64(std::vector<uint8_t> &c, int64_t v) {
  uint64_t u = (uint64_t)v;
  for (int i = 0; i < 8; ++i) {
    c.push_back((uint8_t)(u & 0xff));
    u >>= 8;
  }
}
constexpr uint32_t bc_rd_u32(const std::vector<uint8_t> &c, size_t p) {
  return (uint32_t)c[p] | ((uint32_t)c[p + 1] << 8) |
         ((uint32_t)c[p + 2] << 16) | ((uint32_t)c[p + 3] << 24);
}
constexpr int64_t bc_rd_i64(const std::vector<uint8_t> &c, size_t p) {
  uint64_t u = 0;
  for (int i = 0; i < 8; ++i)
    u |= (uint64_t)c[p + (size_t)i] << (8 * i);
  return (int64_t)u;
}
constexpr Bc bc_of_bin(B b) {
  switch (b) {
  case B::Add:
    return Bc::Add;
  case B::Sub:
    return Bc::Sub;
  case B::Mul:
    return Bc::Mul;
  case B::Div:
    return Bc::Div;
  case B::Mod:
    return Bc::Mod;
  case B::Lt:
    return Bc::Lt;
  case B::Le:
    return Bc::Le;
  case B::Gt:
    return Bc::Gt;
  case B::Ge:
    return Bc::Ge;
  case B::Eq:
    return Bc::Eq;
  case B::Ne:
    return Bc::Ne;
  case B::And:
    return Bc::And;
  case B::Or:
    return Bc::Or;
  case B::Xor:
    return Bc::Xor;
  case B::Shl:
    return Bc::Shl;
  case B::LShr:
    return Bc::Lshr;
  case B::AShr:
    return Bc::Asr;
  case B::MulHi:
    return Bc::MulHi;
  }
  return Bc::Halt;
}
constexpr uint32_t bc_instr_size(const Instr &I) {
  switch (I.k) {
  case K::Const:
    return 9 + 5; // Push + Store
  case K::Copy:
    return 5 + 5; // Load + Store
  case K::Bin:
    return 5 + 5 + 1 + 5; // Load,Load,op,Store
  case K::Br:
    return 5; // Jmp
  case K::Cbr:
    return 5 + 5 + 5; // Load,Jz,Jmp
  case K::Print:
    return 5 + 1; // Load,Print
  case K::Exit:
    return 1; // Halt
  case K::Phi:
    return 0; // gone (destroyed)
  case K::Alloc:
    return 5 + 1 + 5; // Load,ArrNew,Store
  case K::Load:
    return 5 + 5 + 1 + 5; // Load,Load,ArrLd,Store
  case K::Store:
    return 5 + 5 + 5 + 1; // Load,Load,Load,ArrSt
  case K::Len:
    return 5 + 1 + 5; // Load,ArrLen,Store
  case K::Input:
    return 1 + 5; // In,Store
  case K::Depth:
    return 1 + 5; // Depth,Store
  case K::Call:   // Load*argc,Call,Store
    return 1 + 4 + 4 + 5 * (uint32_t)I.args.size() + 5;
  case K::Ret:
    return (I.a == NONE ? 9 : 5) + 1; // (Push|Load),Ret
  case K::Bkpt:
    return 1 + 4 + 4 * (uint32_t)I.args.size(); // Bkpt,n,slots*
  }
  return 0;
}

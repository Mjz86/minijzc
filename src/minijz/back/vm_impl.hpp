#pragma once
// === vm_impl.hpp ========================================
// the emit_bytecode bodies (see vm.hpp for the algorithm + theory).
// Included by vm.hpp in the header-only build (constexpr, via
// MJZ_RUNNER_FN) and by tu/runners.cpp in the multi-TU build (plain
// external linkage) -- include it nowhere else.

#include "vm.hpp"

MJZ_RUNNER_FN std::vector<uint8_t> emit_bytecode(const std::vector<Fn> &fns,
                                                 std::vector<uint32_t> *srcs) {
  const uint32_t nf = (uint32_t)fns.size();
  std::vector<std::vector<uint32_t>> off(nf);
  std::vector<uint32_t> fn_off(nf, 0);
  uint32_t cur = 4 + 8 * nf;
  for (uint32_t f = 0; f < nf; ++f) {
    const Fn &fn = fns[f];
    off[f].assign(fn.nblocks(), 0);
    for (uint32_t b = 0; b < fn.nblocks(); ++b) {
      off[f][b] = cur;
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
        cur += bc_instr_size(fn.code[i]);
    }
    fn_off[f] = fn.nblocks() ? off[f][0] : cur;
  }
  std::vector<uint8_t> c{};
  bc_put_u32(c, nf);
  for (uint32_t f = 0; f < nf; ++f) {
    bc_put_u32(c, fn_off[f]);
    bc_put_u32(c, slot_count(fns[f])); // VM slots (== vregs w/o regalloc)
  }
  for (uint32_t f = 0; f < nf; ++f) {
    const Fn &fn = fns[f];
    for (uint32_t b = 0; b < fn.nblocks(); ++b) {
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        switch (I.k) {
        case K::Phi:
          break;
        case K::Const:
          c.push_back((uint8_t)Bc::Push);
          bc_put_i64(c, I.c);
          c.push_back((uint8_t)Bc::Store);
          bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Copy:
          c.push_back((uint8_t)Bc::Load);
          bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::Store);
          bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Bin:
          c.push_back((uint8_t)Bc::Load);
          bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::Load);
          bc_put_u32(c, slot_id(fns[f], I.b2));
          c.push_back((uint8_t)bc_of_bin(I.b));
          c.push_back((uint8_t)Bc::Store);
          bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Br:
          c.push_back((uint8_t)Bc::Jmp);
          bc_put_u32(c, I.t == NONE ? off[f][b] : off[f][I.t]);
          break;
        case K::Cbr:
          c.push_back((uint8_t)Bc::Load);
          bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::Jz);
          bc_put_u32(c, I.f == NONE ? off[f][b] : off[f][I.f]);
          c.push_back((uint8_t)Bc::Jmp);
          bc_put_u32(c, I.t == NONE ? off[f][b] : off[f][I.t]);
          break;
        case K::Print:
          c.push_back((uint8_t)Bc::Load);
          bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::Print);
          break;
        case K::Alloc:
          c.push_back((uint8_t)Bc::Load);
          bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::ArrNew);
          c.push_back((uint8_t)Bc::Store);
          bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Load:
          c.push_back((uint8_t)Bc::Load);
          bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::Load);
          bc_put_u32(c, slot_id(fns[f], I.b2));
          c.push_back((uint8_t)Bc::ArrLd);
          c.push_back((uint8_t)Bc::Store);
          bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Store:
          c.push_back((uint8_t)Bc::Load);
          bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::Load);
          bc_put_u32(c, slot_id(fns[f], I.b2));
          c.push_back((uint8_t)Bc::Load);
          bc_put_u32(c, slot_id(fns[f], I.c2));
          c.push_back((uint8_t)Bc::ArrSt);
          break;
        case K::Len:
          c.push_back((uint8_t)Bc::Load);
          bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::ArrLen);
          c.push_back((uint8_t)Bc::Store);
          bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Input:
          c.push_back((uint8_t)Bc::In);
          c.push_back((uint8_t)Bc::Store);
          bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Depth:
          c.push_back((uint8_t)Bc::Depth);
          c.push_back((uint8_t)Bc::Store);
          bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Call:
          for (uint32_t av : I.args) { // args pushed in parameter order
            c.push_back((uint8_t)Bc::Load);
            bc_put_u32(c, slot_id(fns[f], av));
          }
          c.push_back((uint8_t)Bc::Call);
          bc_put_u32(c, (uint32_t)I.c);
          bc_put_u32(c, (uint32_t)I.args.size());
          c.push_back((uint8_t)Bc::Store);
          bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Ret:
          if (I.a == NONE) {
            c.push_back((uint8_t)Bc::Push);
            bc_put_i64(c, 0);
          } else {
            c.push_back((uint8_t)Bc::Load);
            bc_put_u32(c, slot_id(fns[f], I.a));
          }
          c.push_back((uint8_t)Bc::Ret);
          break;
        case K::Exit:
          c.push_back((uint8_t)Bc::Halt);
          break;
        case K::Bkpt:
          c.push_back((uint8_t)Bc::Bkpt);
          bc_put_u32(c, (uint32_t)I.args.size());
          for (uint32_t av : I.args)
            bc_put_u32(c, slot_id(fns[f], av));
          break;
        }
        if (srcs) { // every byte of the op maps to this instr's source
          while (srcs->size() < c.size())
            srcs->push_back(I.src);
        }
      }
    }
  }
  if (srcs)
    while (srcs->size() < c.size())
      srcs->push_back(NONE);
  return c;
}

// the historical one-argument form (no source map)
MJZ_RUNNER_FN std::vector<uint8_t> emit_bytecode(const std::vector<Fn> &fns) {
  return emit_bytecode(fns, nullptr);
}

// ---- checkpoint 21: the vm tripart ---------------------------------------
// vm.hpp keeps the bytecode builder (emit_bytecode, both forms); the
// opcode/accessor layer lives in bc_defs.hpp (included above), the
// steppable machine in vm_step.hpp (VmCtx + vm_init + vm_step_op +
// vm_run — the debugger's live view) and the SSA interpreter in
// interp_ssa.hpp (run_prog).

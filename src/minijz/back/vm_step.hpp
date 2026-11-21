#pragma once
// === vm_step.hpp ========================================
// the steppable bytecode machine (split from vm.hpp, checkpoint 21 —
// the vm/steppable/interpreter tripart): VmCtx + vm_init + vm_step_op +
// vm_run. Depends on the bytecode definition layer (bc_defs.hpp) and the
// IR — standalone-compilable on its own; vm.hpp includes this after the
// same layer, so the parent-child order preserves the historical
// definition order exactly (all includers unchanged).
//
// ALGORITHM:
//   * vm_init(S, c, step_limit, ins): validate the header, push main's
//     zeroed frame of fn_slots(0) slots, set pc = fn_off(0).
//   * vm_step_op(S): fetch and run ONE opcode -- Push/Load/ArrLen/In/
//     Depth push; Store pops into frames.back()[slot]; the 18 binops
//     pop y, x and push the w_* wrap/trap result; Jmp/Jz rewrite pc;
//     Call pops na args, pushes a callee frame with them in slots
//     0..na-1 and saves the return pc on rets (cap
//     MJZ_MAX_CALL_DEPTH = 512 callee activations); Ret pops a frame
//     and re-pushes the value for the caller's Store; Bkpt counts one
//     step, then skips its operand bytes.
//   * vm_run: the historical one-shot API -- vm_init plus a while loop
//     over vm_step_op, returning S.R.
//
// DATA STRUCTURES: VmCtx{c, pc, frames (slot vector per activation),
// rets, fr_fn (debugger), st (operand stack), R (output + arena +
// input tape), steps/step_limit, started/halted/trapped/timeout};
// fn_at/fn_off/fn_slots/nfns re-read the directory; "steps" count
// IR-equivalent ops (Store/Jmp/Jz/Print/Call/Ret/ArrSt/Halt/Bkpt),
// so one limit means the same as in the SSA interpreter.
//
// THEORY: the machine must be steppable because the debugger must
// observe state BETWEEN ops: stops at Bkpt slots and line breakpoints
// (the per-byte srcs map), and value-change watchpoints re-resolve
// the watched names after EVERY step -- vm_step_op exposes pc/frames/
// st/arena for that stop/inspect/resume cycle (vm_run cannot).
//
// CONSUMERS: dbg_sess.hpp (run_bounded + watch_step) and dbg_support
// drive VmCtx directly; cli_run.hpp and the tours use vm_run.

#include "bc_defs.hpp"

// ============================================================================
// 9h2. steppable VM state — the debugger's live view. vm_run() below is the
//      historical one-shot API (a thin loop over vm_step_op); VmCtx + vm_step
//      expose the machine single-op-at-a-time so an interactive session can
//      watch every slot, the operand stack, the arena, and the pc move.
// ============================================================================
struct VmCtx {
  const std::vector<uint8_t> *c{};        // the bytecode image (not owned)
  size_t pc{};                            // next op to fetch
  std::vector<std::vector<val_t>> frames; // one slot vector per activation
  std::vector<uint32_t> rets;             // return pcs of suspended callers
  std::vector<uint32_t> fr_fn;            // fn index of each frame (debugger)
  std::vector<val_t> st{};                // the operand stack
  RunOut R{}; // output + arena + input tape position
  uint64_t steps{}, step_limit{};
  bool started{}, halted{}, trapped{}, timeout{};
  // which function is pc in? (bytecode layout: [u32 nfns][nfns x (u32 off,
  // u32 nslots)][code...]; fns[0] = main body)
  constexpr uint32_t fn_at(size_t p) const {
    uint32_t nf = bc_rd_u32(*c, 0);
    uint32_t best = 0;
    for (uint32_t f = 0; f < nf; ++f) {
      uint32_t off = bc_rd_u32(*c, 4 + (size_t)8 * f);
      if (off <= p)
        best = f;
    }
    return best;
  }
  constexpr uint32_t fn_off(uint32_t f) const {
    return bc_rd_u32(*c, 4 + (size_t)8 * f);
  }
  constexpr uint32_t fn_slots(uint32_t f) const {
    return bc_rd_u32(*c, 4 + (size_t)8 * f + 4);
  }
  constexpr uint32_t nfns() const { return bc_rd_u32(*c, 0); }
};

#ifdef MJZ_MULTI_CPP
// the three bodies compiled ONCE in tu/runners.cpp (back/vm_step_impl.hpp)
bool vm_init(VmCtx &S, const std::vector<uint8_t> &c, uint64_t step_limit,
             const std::vector<val_t> &ins);
bool vm_step_op(VmCtx &S);
RunOut vm_run(const std::vector<uint8_t> &c, uint64_t step_limit = 200000,
              const std::vector<val_t> &ins = {});
#else
constexpr bool vm_init(VmCtx &S, const std::vector<uint8_t> &c,
                       uint64_t step_limit, const std::vector<val_t> &ins);
constexpr bool vm_step_op(VmCtx &S);
constexpr RunOut vm_run(const std::vector<uint8_t> &c,
                        uint64_t step_limit = 200000,
                        const std::vector<val_t> &ins = {});
#include "vm_step_impl.hpp"
#endif

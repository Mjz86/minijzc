#pragma once
// === interp_ssa.hpp ========================================
// the SSA interpreter (split from vm.hpp, checkpoint 21 — the
// vm/steppable/interpreter tripart): run_prog with an explicit frame
// stack — Call pushes a frame (params bound from the operand-side
// argument vregs), Ret pops back to the caller's resume point and
// stores the value into the result slot. Phi resolution by pred
// identity via t/f. Depends only on the IR + RunOut (ir/instr.hpp
// brings both), not on the bytecode layer — standalone-compilable.
//
// THEORY (executing SSA directly -- no destruction, no slots):
//   * FRAMES ARE ACTIVATIONS. Each Call pushes {fn, val[], has[]}
//     -- val is indexed by VREG ID (SSA values get a home per
//     activation; no register allocation needed), has[] tracks
//     definedness (undef reads are the interpreter's safety net).
//     Ret pops to the saved Resume point and writes the return
//     value into the caller's result slot (resumes is a parallel
//     stack of {fn, blk, i, dst}).
//   * PHIS RESOLVE ON THE EDGE, in parallel. A Br/Cbr transfer
//     stages the target block's phis FIRST: read every arg keyed
//     to the CURRENT block (t/f), then write all dsts -- the
//     read-all-then-write-all discipline is exactly destruct's
//     parallel-copy semantics (a phi is a simultaneous assignment
//     on the edge). Phis are skipped at +i (K::Phi case): by the
//     time control reaches them they have already been staged.
//   * SEMANTICS = THE RUNNER PARITY CHAIN. This is one of the
//     --check-all runners: AST oracle (astrun) vs THIS SSA
//     interpreter vs lowered C vs the bytecode VM must all agree
//     -- the five-way agreement is the compiler's correctness
//     oracle, so the interpreter mirrors the wrapping arithmetic
//     (w_*), the arena traps (with located trap_src), the 512
//     callee depth cap (stack.size() counts main, hence the
//     strict >), the step_limit fuel, and K::Depth = frame count
//     including main.

#include "../ir/instr.hpp"

// ============================================================================
// 10. SSA interpreter (phi resolution by pred identity via t/f)
// ============================================================================
// program-level SSA interpreter with an explicit frame stack: Call pushes a
// frame (params bound from the operand-side argument vregs), Ret pops back
// to the caller's resume point and stores the value into the result slot.
#ifdef MJZ_MULTI_CPP
// the body is compiled ONCE in tu/runners.cpp (back/interp_ssa_impl.hpp):
// the seven app TUs that pull the runner stack each used to re-parse,
// re-instantiate and re-emit this whole interpreter (~330 KB of COMDAT
// text per TU)
RunOut run_prog(const std::vector<Fn> &fns, uint64_t step_limit = 200000,
                const std::vector<val_t> &ins = {});
#else
constexpr RunOut run_prog(const std::vector<Fn> &fns,
                          uint64_t step_limit = 200000,
                          const std::vector<val_t> &ins = {});
#include "interp_ssa_impl.hpp"
#endif

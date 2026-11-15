#pragma once
// === dce.hpp ========================================
// SSA dead-code elimination.
//
// ALGORITHM: opt_dce is a mark-sweep plus a phi fixpoint. (1) Seed
//   scans build def_idx (vreg -> defining instr) and known/kval (const
//   temps); may_trap(I) flags a Div/Mod without I.nt whose divisor is
//   not a known nonzero const. (2) Mark: every root -- Print, Br, Cbr,
//   Exit, Store, Input, Alloc, Load, Ret, Call, Bkpt, may_trap -- goes
//   into live and the worklist wl; popping i marks def_idx of each
//   operand in I.args and {I.a, I.b2, I.c2} live and pushes it. (3)
//   Sweep: live instrs compact per block (os.dce++ per drop). (4) Up
//   to 10 rounds of trivial-phi collapse: phi(a,a) / phi(a,self) /
//   phi(a) becomes K::Copy of that arg; rounds cascade to a fixpoint.
// DATA STRUCTURES:
//   * known/kval: const temps, feeding may_trap's divisor test.
//   * def_idx: vreg -> def instr index -- the use->def edge the
//     worklist walk follows.
//   * live + wl: per-instr mark byte and the worklist; an instr is
//     pushed at most once, so the walk is linear in fn.code.size().
//   * nc: the compacted vector the sweep rebuilds per block range.
// THEORY: a def is live iff it transitively feeds a root, and roots
//   are exactly the observable instrs: control flow (Br/Cbr/Exit),
//   output (Print), arena mutation (Store), stdin consumption (Input),
//   possible traps (Div/Mod without nt, Alloc: n<0 or OOM, Load: out
//   of bounds) -- sweeping a dead def erases no observable behavior.
//   Bkpt stays so -O3 builds remain debuggable. The phi collapse is
//   sound because, with all non-self arguments equal, the phi has only
//   one possible value.
// ENABLES: the collapsed phis become plain copies (copyprop's food),
//   only genuine phis remain for later scans, and the sweep leaves a
//   smaller fn.code with per-block ranges intact.

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../../ir/sweep.hpp"
#include "../analysis/an.hpp"
#include "../opt_generic.hpp"

// --- 9c. SSA dead-code elimination -------------------------------------------
// roots: Print, Br, Cbr, Exit, and Div/Mod whose divisor is not a known
// nonzero constant (they may trap — removing them would erase the trap).
// The mark phase is the generic liveness kernel (opt_ns::
// calculate_live_sites over the def/use bipartite tables); the phi
// collapse picks come from opt_ns::calculate_trivial_phi_pick (the N-ary
// rule over the arg row). The policies (what is a root, what collapses)
// stay here with the compiler; the fixpoints are kernel code.
constexpr bool opt_dce(Fn &fn, OptStats &os) {
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  std::vector<char> known(nv, 0);
  std::vector<val_t> kval(nv, 0);
  for (const Instr &I : fn.code)
    if (I.k == K::Const) {
      known[I.dst] = 1;
      kval[I.dst] = I.c;
    }
  // def index + operand rows of each site (the whole IR knowledge the
  // kernel needs: which var each site defines, which vars it reads)
  std::vector<uintlen_t> def_idx(nv, opt_ns::none_v);
  std::vector<std::vector<uintlen_t>> args_of(fn.code.size());
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      if (fn.code[i].dst != NONE)
        def_idx[fn.code[i].dst] = i;
      for (uint32_t v : fn.code[i].args) // Call arguments
        if (v != NONE)
          args_of[i].push_back(v);
      for (uint32_t v : {fn.code[i].a, fn.code[i].b2, fn.code[i].c2})
        if (v != NONE)
          args_of[i].push_back(v);
    }
  auto may_trap = [&](const Instr &I) {
    if (I.k != K::Bin || (I.b != B::Div && I.b != B::Mod))
      return false;
    if (I.nt)
      return false; // proven trap-free: pure, removable
    return !(I.b2 < nv && known[I.b2] && kval[I.b2] != 0);
  };
  // the root scan: the POLICY lives here, the fixpoint in the kernel
  std::vector<uintlen_t> roots{};
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      // roots: side effects (Print is output, Store mutates the arena,
      // Input consumes the stdin tape), control flow, and anything that may
      // TRAP (Div/Mod with a non-constant divisor, Alloc: n<0 or OOM,
      // Load: out of bounds) — removing them would erase observable behavior.
      // Bkpt is a debugging observation point: it stays so -O3 builds remain
      // debuggable (its observed args are live uses, walked by the kernel).
      if (I.k == K::Print || I.k == K::Br || I.k == K::Cbr || I.k == K::Exit ||
          I.k == K::Store || I.k == K::Input || I.k == K::Alloc ||
          I.k == K::Load || I.k == K::Ret || I.k == K::Call || I.k == K::Bkpt ||
          may_trap(I))
        roots.push_back(i);
    }
  const std::vector<char> live =
      opt_ns::calculate_live_sites<V>(def_idx, args_of, roots);
  // sweep
  bool changed = false;
  {
    const uint32_t gone = sweep_survivors(fn, live);
    if (gone) {
      os.dce += gone;
      changed = true;
    }
  }
  // trivial-phi collapse (x = phi(a, a) / phi(self, a) -> copy), cascading
  // rounds; the pick is the kernel's N-ary rule over the phi's arg row
  for (int round = 0; round < 10; ++round) {
    bool ch = false;
    for (uint32_t b = 0; b < fn.nblocks(); ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        Instr &I = fn.code[i];
        if (I.k != K::Phi)
          continue;
        const uintlen_t pick = opt_ns::calculate_trivial_phi_pick(
            I.dst == NONE ? opt_ns::none_v : (uintlen_t)I.dst,
            std::array<uintlen_t, 2>{
                I.a == NONE ? opt_ns::none_v : (uintlen_t)I.a,
                I.b2 == NONE ? opt_ns::none_v : (uintlen_t)I.b2});
        if (pick == opt_ns::none_v)
          continue; // genuine phi / all self
        I.k = K::Copy;
        I.a = (uint32_t)pick;
        I.b2 = NONE;
        I.t = I.f = NONE;
        os.copies++;
        ch = true;
      }
    if (!ch)
      break;
    changed |= ch;
  }
  return changed;
}

// def instr index of vreg v (linear scan; diagnostics only)
constexpr uint32_t def_index_of(const Fn &fn, uint32_t v) {
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
      if (fn.code[i].dst == v)
        return i;
  return NONE;
}

// --- 9d. loop-invariant code motion
// -------------------------------------------

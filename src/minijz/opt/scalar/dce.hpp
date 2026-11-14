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
#include "../analysis/an.hpp"

// --- 9c. SSA dead-code elimination -------------------------------------------
// roots: Print, Br, Cbr, Exit, and Div/Mod whose divisor is not a known
// nonzero constant (they may trap — removing them would erase the trap).
constexpr bool opt_dce(Fn &fn, OptStats &os) {
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  std::vector<char> known(nv, 0);
  std::vector<val_t> kval(nv, 0);
  for (const Instr &I : fn.code)
    if (I.k == K::Const) {
      known[I.dst] = 1;
      kval[I.dst] = I.c;
    }
  // def index of each vreg
  std::vector<uint32_t> def_idx(nv, NONE);
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
      if (fn.code[i].dst != NONE)
        def_idx[fn.code[i].dst] = i;
  auto may_trap = [&](const Instr &I) {
    if (I.k != K::Bin || (I.b != B::Div && I.b != B::Mod))
      return false;
    if (I.nt)
      return false; // proven trap-free: pure, removable
    return !(I.b2 < nv && known[I.b2] && kval[I.b2] != 0);
  };
  std::vector<char> live(fn.code.size(), 0);
  std::vector<uint32_t> wl{};
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      // roots: side effects (Print is output, Store mutates the arena,
      // Input consumes the stdin tape), control flow, and anything that may
      // TRAP (Div/Mod with a non-constant divisor, Alloc: n<0 or OOM,
      // Load: out of bounds) — removing them would erase observable behavior.
      // Bkpt is a debugging observation point: it stays so -O3 builds remain
      // debuggable (its observed args are live uses, walked below).
      if (I.k == K::Print || I.k == K::Br || I.k == K::Cbr || I.k == K::Exit ||
          I.k == K::Store || I.k == K::Input || I.k == K::Alloc ||
          I.k == K::Load || I.k == K::Ret || I.k == K::Call || I.k == K::Bkpt ||
          may_trap(I)) {
        live[i] = 1;
        wl.push_back(i);
      }
    }
  while (!wl.empty()) {
    uint32_t i = wl.back();
    wl.pop_back();
    for (uint32_t v : fn.code[i].args) { // Call arguments
      if (v == NONE || v >= nv)
        continue;
      uint32_t d = def_idx[v];
      if (d != NONE && !live[d]) {
        live[d] = 1;
        wl.push_back(d);
      }
    }
    for (uint32_t v : {fn.code[i].a, fn.code[i].b2, fn.code[i].c2}) {
      if (v == NONE || v >= nv)
        continue;
      uint32_t d = def_idx[v];
      if (d != NONE && !live[d]) {
        live[d] = 1;
        wl.push_back(d);
      }
    }
  }
  // sweep
  bool changed = false;
  {
    std::vector<Instr> nc{};
    nc.reserve(fn.code.size());
    for (uint32_t b = 0; b < fn.nblocks(); ++b) {
      uint32_t ob = fn.blocks[b].begin, oe = fn.blocks[b].end;
      fn.blocks[b].begin = (uint32_t)nc.size();
      for (uint32_t i = ob; i < oe; ++i) {
        if (live[i])
          nc.push_back(fn.code[i]);
        else {
          changed = true;
          os.dce++;
        }
      }
      fn.blocks[b].end = (uint32_t)nc.size();
    }
    fn.code = std::move(nc);
  }
  // trivial-phi collapse (x = phi(a, a) / phi(self, a) -> copy), cascading
  for (int round = 0; round < 10; ++round) {
    bool ch = false;
    for (uint32_t b = 0; b < fn.nblocks(); ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        Instr &I = fn.code[i];
        if (I.k != K::Phi)
          continue;
        uint32_t x = (I.a != NONE && I.a != I.dst) ? I.a : NONE;
        uint32_t y = (I.b2 != NONE && I.b2 != I.dst) ? I.b2 : NONE;
        if (x == NONE && y == NONE)
          continue; // all self/undef
        if (x != NONE && y != NONE && x != y)
          continue;                          // genuine phi
        uint32_t pick = (x != NONE) ? x : y; // phi(a), phi(a,self)
        I.k = K::Copy;
        I.a = pick;
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

#pragma once
// === breq.hpp ========================================
// branch folding on RECONVERGING transparent arms (aggressive DCE,
// the control-dependence kind).
//
// ALGORITHM: opt_breq folds ONE Cbr per invocation (the tier-0 loop
//   fixpoints it) when the two arms reconverge with no observable
//   difference. An arm is a TRANSPARENT RELAY when it is private to
//   the branch (entered only from it), phi-free, and holds nothing
//   but a `br` to the join. The join J is then: the common relay
//   target of both arms, or one arm itself with the other relaying
//   into it. The join's phis (its preds are exactly the two arm
//   sides, by the <=2-pred invariant) must carry the SAME vreg on
//   both arm edges -- the folded `br` edge replaces both, and either
//   arm's value is the other's only when they agree. The phis rekey
//   to the single incoming edge (the duplicate-pred shape, both
//   slots b, the common value; phi_degens collapses it next round),
//   the Cbr becomes `br J`, the relay arms become unreachable code
//   for the unreach pass, and the condition vreg loses a use for
//   dce.
//
// DATA STRUCTURES: none beyond the recompute_preds scan -- the pass
//   reads fn.preds (freshly recomputed; one fold per call keeps the
//   sweep's rows honest), the terminator row of each candidate, and
//   the join's leading phis.
//
// THEORY: the guard makes the fold a control-flow no-op. Both arm
//   paths run exactly the relay `br` -- no instrs, no phis, no
//   observable effect -- and deliver the same phi values to the same
//   join, so replacing the two-edge branch with a single edge to the
//   join executes the same instrs in the same order and routes the
//   same values. fold's cbr_same owns the identical-TARGETS case
//   (T == F); this pass owns the RECONVERGING case (T != F), the
//   shape branch folding and splice leave behind: `if (c) {} else
//   {}` with the empty arms as relays, guard pairs whose arms fell
//   empty after DCE, if-conversion refusals whose arms were stripped
//   to the bone. The condition's remaining uses (if any) keep dce
//   honest; the entry is never a join (it has no preds to reconverge
//   from); a join that is the branching block itself (a self-loop)
//   is legal and folds like any other.
//
// ENABLES: kills the branch, its compare (when the cond dies), and
//   the relay blocks outright -- the CFG cleanup that neither fold
//   (const cond / identical targets) nor splice (single-succ
//   blocks only) can express; the freed joins give splice and the
//   loop analyses straighter shapes to work with.

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"

// --- 9b9. Cbr with reconverging transparent arms -> Br -----------------
// Both arms are private, phi-free `br`-only relays to the same join (or one
// arm IS the join and the other relays into it), and the join's phis carry
// the same vreg on both arm edges: the branch is unobservable, `br J`.
constexpr bool opt_breq(Fn &fn, OptStats &os) {
  fn.recompute_preds();
  const uint32_t nb = fn.nblocks();
  // an arm is a transparent relay: private to the branch, phi-free,
  // nothing but the `br` -- the pure-control shape
  auto relay = [&](uint32_t x, uint32_t b) noexcept -> uint32_t {
    if (x == NONE || x == 0 || x == b)
      return NONE;
    if (fn.preds[x].size() != 1 || fn.preds[x][0] != b)
      return NONE; // not private: others reach it too
    const Block &R = fn.blocks[x];
    if (R.end - R.begin != 1)
      return NONE; // holds more than the terminator
    const Instr &T = fn.code[R.begin];
    if (T.k != K::Br || T.t == NONE || T.t == 0)
      return NONE; // not a relay to a foldable join
    return T.t;
  };
  for (uint32_t b = 0; b < nb; ++b) {
    const Block &B = fn.blocks[b];
    if (B.begin == B.end)
      continue;
    Instr &I = fn.code[B.end - 1];
    if (I.k != K::Cbr || I.t == NONE || I.f == NONE || I.t == I.f)
      continue; // fold's cbr_same owns the identical-arms case
    const uint32_t T = I.t, F = I.f;
    const uint32_t rt = relay(T, b), rf = relay(F, b);
    // the join and its two arm-side pred ids: the join's preds are
    // exactly these two (the <=2-pred invariant plus the two live
    // arm edges), which is what makes the phi rekey total
    uint32_t J = NONE, s1 = NONE, s2 = NONE;
    if (rt != NONE && rf != NONE && rt == rf) {
      J = rt;
      s1 = T;
      s2 = F;
    } else if (rf != NONE && rf == T) { // the true arm IS the join
      J = T;
      s1 = b;
      s2 = F;
    } else if (rt != NONE && rt == F) { // the false arm IS the join
      J = F;
      s1 = T;
      s2 = b;
    }
    if (J == NONE)
      continue;
    // the join's phis: same vreg on both arm edges, then rekey to
    // the single folded edge (both slots b, the common value -- the
    // duplicate-pred shape phi_degens collapses next round)
    bool ok = true;
    for (uint32_t i = fn.blocks[J].begin; i < fn.blocks[J].end; ++i) {
      Instr &P = fn.code[i];
      if (P.k != K::Phi)
        break; // phis lead the block
      if ((P.t != s1 || P.f != s2) && (P.t != s2 || P.f != s1)) {
        ok = false; // a foreign pred slot: not this fold's shape
        break;
      }
      const uint32_t v1 = P.t == s1 ? P.a : P.b2;
      const uint32_t v2 = P.t == s2 ? P.a : P.b2;
      if (v1 != v2) { // arms deliver different values: observable
        ok = false;
        break;
      }
      P.t = b;
      P.f = b;
      P.a = v1;
      P.b2 = v1;
    }
    if (!ok)
      continue;
    // the fold: both arm paths ran only the relay `br` and delivered
    // the same phi values -- one edge to the join is the same program
    I.k = K::Br;
    I.t = J;
    I.f = NONE;
    fn.succ[2 * b] = J;
    fn.succ[2 * b + 1] = NONE;
    fn.recompute_preds();
    os.cbr_reconv++;
    return true; // one fold per call: the tier-0 loop fixpoints
  }
  return false;
}

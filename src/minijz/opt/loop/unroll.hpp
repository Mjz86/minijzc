#pragma once
// === unroll.hpp ========================================
// full loop unrolling for constant trip counts. This file is the
// slim driver (the code-quality continuation's cdfold/lcf treatment,
// round three): scan the fn once, then walk the headers
// nest-descending and try, per loop --
//   shape (edges/exit/arms/hphis/induction)  [unroll_impl.hpp]
//   plan (exact trip count by simulation, growth cap)
//   safety (no loop value may escape unrewritten)
//   commit (K+1 copies + rewire, one unroll per call)
//
// --- 9d2. loop unrolling -----------------------------------------------------
// Full unroll of simple counted loops with a CONSTANT trip count,
// computed EXACTLY (the induction advances with the program's
// wrapping add/sub, so even wrap-around trip counts are honored; the
// count is capped at `limit`). Shape required (all from the lib's
// natural-loop analysis):
//   * one back edge: the header's unique in-loop predecessor (the
//     latch; several back-edges tolerated, the LAST is the latch)
//   * one outside predecessor (the preheader)
//   * the ONLY exit edge is the header's comparison (no `break`s)
//   * induction phi i = phi(i0, i+c) with constant i0 and c != 0
//   * header test `i < bound` / `i <= bound` (either operand order),
//     bound constant
// The loop becomes K body copies + a tail copy of the header (whose
// test is provably false -> Br exit). Header phis turn into
// per-copy constants (the induction) and chained values
// (accumulators); uses outside the loop are rewritten to the tail's
// exit values; the exit block's phis are rekeyed from the header to
// the tail. constprop + DCE then fold pure arithmetic loops into
// constants at compile time.
//
// THEORY -- why the result computes the same function (the full
// argument, and the data-structure inventory, live in
// unroll_impl.hpp's header): the trip count K is found by RUNNING
// the loop's own wrapping arithmetic, the single-exit scan proves
// no other edge can re-enter the body, and SSA is preserved by
// NAMING -- every copy defines fresh vregs and every use is
// redirected (phi -> entry value, loop def -> the copy's private
// vreg), so no phi survives and no reconstruction pass is needed.

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../analysis/an.hpp"
#include "unroll_impl.hpp" // UnrScan/UnrLoop + the per-phase matchers

constexpr bool opt_unroll(Fn &fn, const An &A, OptStats &os, uint32_t limit) {
  UnrScan S;
  if (!unr_scan(fn, A, limit, S))
    return false;                // disabled, stale analysis, or nothing to name
  for (uint32_t h : S.headers) { // innermost loops first
    UnrLoop L;
    if (!unr_shape(fn, A, S, h, L))
      continue; // not an unrollable counted loop: keep it
    if (!unr_trip(L, limit) || !unr_growth_ok(fn, S, L))
      continue; // trip not computable in budget / growth cap
    if (!unr_outside_uses_ok(fn, S, L))
      continue;                      // a loop value would escape unrewritten
    return unr_commit(fn, S, L, os); // ONE unroll per call
  }
  return false;
}

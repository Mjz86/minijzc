#pragma once
// === cdfold.hpp ========================================
// countdown fold: replace tailrec'd exact-budget loops by closed
// forms. The support layer -- CdAff algebra, the resolver, the
// per-phase matchers, the two-buffer emitter -- lives in
// cdfold_impl.hpp (the code-quality continuation's file split); this
// file is the slim driver: match each loop header, fold what matches.
//
// THEORY (the full argument: cdfold_impl.hpp, section 9e5): the loop
// TCE emits is a countdown -- n' = n - 1, b' = b - 1, base exits at
// n == 0, a trap at b == 0 -- so N = (u64)n0 iterations bring n to
// 0 iff they fit the budget, and the guard `(u64)n0 > (u64)bInit`
// (emitted as the sign-bit lift: Xor INT64_MIN, then unsigned Lt)
// jumps to the SAME trap the budget check would have fired: n0 < 0
// wraps to a huge unsigned count, and n0 over budget, both trap
// exactly where the original recursion cap did. Additive
// accumulators x' = x + g + b*n fold to x_N = x0 + g*N + b*(N*n0 -
// N*(N-1)/2); the selector toggles per trip, sel_N = (1-2p)*sel0 +
// p with p = N&1 -- all mod-2^64 exact. Shapes the resolver cannot
// express (binary-search bodies) fail cd_match and keep their loop,
// so a fold is only ever an exact replacement. The loop is then
// severed (cd_sever_loop): empty blocks are inert, leaving plain
// straight-line code -- guard + closed form -- for every later pass.

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../analysis/an.hpp"
#include "../linear/linform.hpp"
#include "cdfold_impl.hpp"

constexpr bool opt_cdfold(Fn &fn, const An &A, OptStats &os) {
  if (A.n != fn.nblocks() || !fn.nblocks())
    return false;
  const uint32_t nb = fn.nblocks(); // the PASS-ENTRY count: matchers
  // scan [0, nb); the resolver and the rewrite see the CURRENT count
  // (earlier folds in this same call appended their closed-form blocks)
  if (!(fn.nvars + (uint32_t)fn.version_base.size()))
    return false;
  bool changed = false;
  for (uint32_t h = 0; h < A.n; ++h) {
    if (A.inner[h] != h)
      continue; // not a loop header
    CdShape S{};
    if (!cd_match(fn, A, nb, h, S))
      continue; // not the TCE countdown shape: keep the loop
    changed |= cd_apply(fn, S, nb, os);
  }
  return changed;
}

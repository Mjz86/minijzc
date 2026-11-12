#pragma once
// === cdfold.hpp ========================================
// countdown fold: replace tailrec'd exact-budget loops by closed
// forms. The support layer -- CdAff algebra, the resolver, the
// per-phase matchers, the two-buffer emitter -- lives in
// cdfold_impl.hpp (the code-quality continuation's file split); this
// file is the slim driver: match each loop header, fold what matches.

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

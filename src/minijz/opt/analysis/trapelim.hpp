#pragma once
// === trapelim.hpp ========================================
// trap elimination + range-proved comparison folding.

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "alias.hpp"
#include "an.hpp"
#include "intervals.hpp"

// --- 9d3c. trap elimination
// ---------------------------------------------------- A Div/Mod whose divisor
// provably excludes 0 (and excludes -1, or whose dividend provably excludes
// INT64_MIN) can never trap: mark it `nt` so every backend drops the guard,
// DCE/LICM may treat it as pure, and the closed-form/simulation loop passes may
// delete the loop computing it. A Load/Store whose handle provably names ONE
// constant-length allocation with a constant in-range index can never trap
// either. The same walk folds COMPARISONS the value-range intervals already
// decide (x.hi < y.lo etc.): comparisons never trap, so a decided outcome is
// a constant, which the tier-0 fold pass then threads into branches.
// `RA` is the caller's range analysis (shared with jthread since the
// checkpoint-14 battery: a second analysis per optimize call pushed the
// constexpr proofs' peak memory over the edge).
constexpr bool opt_trapelim(Fn &fn, const An &A, OptStats &os,
                            const Ranges &RA) {
  if (!RA.ok)
    return false;
  AliasInfo AI = alias_analyze(fn);
  const uint32_t nb = fn.nblocks();
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nv)
    return false;
  std::vector<uint32_t> defi(nv, NONE);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
      if (fn.code[i].dst != NONE && fn.code[i].dst < nv)
        defi[fn.code[i].dst] = i;
  // allocation lengths (constant ones only)
  std::vector<val_t> alen(AI.nallocs, val_t(-1));
  for (const Instr &I : fn.code)
    if (I.k == K::Alloc && I.a != NONE && I.a < nv && defi[I.a] != NONE &&
        fn.code[defi[I.a]].k == K::Const && I.dst != NONE && I.dst < nv &&
        AI.single[I.dst] != NONE && fn.code[defi[I.a]].c >= 0)
      alen[AI.single[I.dst]] = fn.code[defi[I.a]].c;
  auto nz = [](const Iv &i) { return i.bot || i.hi < 0 || i.lo > 0; };
  auto nm1 = [](const Iv &i) { return i.bot || i.hi < -1 || i.lo > -1; };
  auto nmin = [](const Iv &i) { return i.bot || i.lo > INT64_MIN; };
  // range-proved comparison folding: comparisons never trap, so an outcome
  // the intervals already decide is a constant. (the decision itself lives
  // in intervals.hpp as iv_decide -- jthread reuses it per-edge.)
  auto fold_cmp = [&](B b, const Iv &x, const Iv &y, val_t &out) -> bool {
    return iv_decide(b, x, y, out);
  };
  bool changed = false;
  std::vector<Iv> st(nv);
  for (uint32_t b = 0; b < nb; ++b) {
    for (uint32_t v = 0; v < nv; ++v)
      st[v] = RA.entry[(size_t)b * nv + v];
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k == K::Phi)
        continue; // entry rule already covered phis
      if (I.k == K::Bin &&
          (I.b == B::Lt || I.b == B::Le || I.b == B::Gt || I.b == B::Ge ||
           I.b == B::Eq || I.b == B::Ne) &&
          I.a != NONE && I.a < nv && I.b2 != NONE && I.b2 < nv) {
        // range-proved comparison folding (comparisons never trap)
        val_t out{};
        if (fold_cmp(I.b, st[I.a], st[I.b2], out)) {
          I.k = K::Const;
          I.c = out;
          I.a = NONE;
          I.b2 = NONE;
          os.rangecmps++;
          changed = true;
        }
      } else if (I.k == K::Bin && (I.b == B::Div || I.b == B::Mod) && !I.nt) {
        Iv dv = (I.b2 != NONE && I.b2 < nv) ? st[I.b2] : Iv{};
        Iv dd = (I.a != NONE && I.a < nv) ? st[I.a] : Iv{};
        if (nz(dv) && (nm1(dv) || nmin(dd))) {
          I.nt = true;
          os.traps_killed++;
          changed = true;
        }
      } else if ((I.k == K::Load || I.k == K::Store) && !I.nt) {
        // provably one constant-length allocation + provably in-range index
        // (a constant index, or the range analysis's interval for a runtime
        // index, must fit inside [0, len))
        if (I.a != NONE && I.a < nv && AI.single[I.a] != NONE &&
            alen[AI.single[I.a]] >= 0 && I.b2 != NONE && I.b2 < nv) {
          val_t L = alen[AI.single[I.a]];
          bool safe = false;
          if (defi[I.b2] != NONE && fn.code[defi[I.b2]].k == K::Const) {
            val_t idx = fn.code[defi[I.b2]].c;
            safe = idx >= 0 && idx < L;
          } else {
            const Iv &ix = st[I.b2]; // range at this program point
            safe = !ix.bot && ix.lo >= 0 && ix.hi < L;
          }
          if (safe) {
            I.nt = true;
            os.traps_killed++;
            changed = true;
          }
        }
      }
      iv_transfer(fn, i, nv, st);
    }
  }
  return changed;
}

// ============================================================================

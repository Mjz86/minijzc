#pragma once
// === gvn.hpp ========================================
// dominator-based global value numbering -- the wishlist's "GVN proper".
//
// The local cse already unifies pure Bin recomputation under a dominating
// identical Bin; gvn generalizes the SAME dominator-scoped walk to a single
// value-number table with three entry kinds:
//
//   * Const -- two `t = 7` defs (one dominating the other) are ONE value:
//     the later def renames onto the earlier and dies. Unrolling and
//     splice litter blocks with duplicate constants; this folds them.
//   * Phi congruence -- two phis of the SAME block whose incoming maps
//     (per-pred resolved operands) agree are the same value: the later
//     renames onto the earlier and dies. Duplicated merge patterns (hand
//     written and unrolled) produce these; copyprop cannot see them.
//   * Bin -- exactly cse's rules (pure, non-trapping div/mod only with nt,
//     commutative canonicalization, constant operands keyed by value).
//
// Soundness notes (the cse skeleton, one table):
//   ren[v] = w implies w's def DOMINATES v's def (dom-tree preorder with
//   scoped truncation on exit), which dominates every edge-use of v -- so
//   the completed map can rewrite every operand of every instruction, Phi
//   arguments included, in a second pass. A phi pair is only unified when
//   neither references the other's result (mutually-referential header
//   pairs are left to phi-degen/sccp: conservative, never wrong). Const
//   defs that define a PARAMETER vreg (fnspec's clones materialize those)
//   are never touched -- params are defined "virtually".
//
// MECHANISM: the walk, the scoped table, the operand resolution and the
//   commutative/const key normalization are the GENERIC kernel
//   (opt_ns::value_number) -- the same call cse makes, with phi and
//   const entries enabled. This wrapper states the site tables: which
//   instrs take part (the policy: pure Bins, complete phis, non-param
//   consts) and their operand/pred rows; the phi TAINT rule (a pair is
//   refused when either phi's raw args contain the other's result --
//   checked over ALL of the block's phi sites, hit-dropped ones included)
//   reads the site rows inside the kernel.
#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../../ir/sweep.hpp"
#include "../analysis/an.hpp"
#include "../opt_generic.hpp"
#include "vn_sites.hpp"

constexpr bool opt_gvn(Fn &fn, const An &A, OptStats &os) {
  if (A.n != fn.nblocks() || !fn.nblocks())
    return false;
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  // known constants (the shared seeding): operands key by VALUE, not by
  // vreg (cse's rule), so i*7 hits i*7 even when the 7s are different temps
  std::vector<char> kconst{};
  std::vector<val_t> kval{};
  seed_consts(fn, nv, kconst, kval);
  // ---- the site tables (the wrapper's whole job): the three entry
  // kinds, their policies (pure Bins with nt-gated div/mod -- the SHARED
  // vn_fill_bin_site; phis with BOTH incomings present -- a degenerate
  // phi is phi-degen's business; consts that do not define a PARAMETER
  // vreg), and the operand/pred rows. The walk, the scoped table,
  // resolution and the taint rule are the kernel's.
  const uint32_t n = (uint32_t)fn.code.size();
  std::vector<opt_ns::vn_site_t<V>> sinfo(n);
  std::vector<std::vector<uintlen_t>> sargs(n), spreds(n);
  std::vector<std::vector<uintlen_t>> sites_of_block(fn.nblocks());
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      sites_of_block[b].push_back(i);
      const Instr &I = fn.code[i];
      sinfo[i].dst = vn_u(I.dst);
      if (I.dst == NONE || I.dst >= nv)
        continue;
      if (I.k == K::Const) {
        if (I.dst < fn.nparams)
          continue; // param-defining placeholder (fnspec clones): keep
        sinfo[i].tag = opt_ns::vn_const_v;
        sinfo[i].c = I.c;
        continue;
      }
      if (I.k == K::Phi) {
        // phis carry <= 2 incomings: I.a from pred I.t, I.b2 from pred
        // I.f. A degenerate phi (a missing incoming, NONE) is
        // phi-degen's business, not ours.
        if (I.a == NONE || I.b2 == NONE)
          continue;
        sinfo[i].tag = opt_ns::vn_phi_v;
        sargs[i] = {vn_u(I.a), vn_u(I.b2)};
        spreds[i] = {vn_u(I.t), vn_u(I.f)};
        continue;
      }
      vn_fill_bin_site(I, nv, sinfo[i], sargs[i]);
    }
  const opt_ns::vn_result_t<V> R = opt_ns::value_number<V>(
      nv, A.dom_tree, sites_of_block, sinfo, sargs, spreds, kconst, kval);
  if (!R.hits)
    return false;
  // second pass with the COMPLETED map (the shared rewrite)
  vn_rewrite_completed_map(fn, nv, R);
  // sweep dropped instrs, preserving block ranges
  os.gvns += (uint32_t)R.hits;
  sweep_survivors(fn, R.keep);
  return true;
}

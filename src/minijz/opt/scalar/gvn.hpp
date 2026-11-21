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
//   const entries enabled. The pass is the shared driver
//   (vn_sites.hpp's vn_run) plus the site policy: which instrs take
//   part (pure Bins, complete phis, non-param consts) and their
//   operand/pred rows; the phi TAINT rule (a pair is refused when
//   either phi's raw args contain the other's result -- checked over
//   ALL of the block's phi sites, hit-dropped ones included) reads the
//   site rows inside the kernel.
#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../../ir/sweep.hpp"
#include "../analysis/an.hpp"
#include "../opt_generic.hpp"
#include "vn_sites.hpp"

constexpr bool opt_gvn(Fn &fn, const An &A, OptStats &os) {
  uint32_t hits = 0;
  // the three entry kinds, their policies (pure Bins with nt-gated
  // div/mod -- the SHARED vn_fill_bin_site; phis with BOTH incomings
  // present -- a degenerate phi is phi-degen's business; consts that
  // do not define a PARAMETER vreg), and the operand/pred rows
  const bool did =
      vn_run(fn, A, hits,
             [&fn](const Instr &I, uint32_t nv, opt_ns::vn_site_t<V> &si,
                   std::vector<uintlen_t> &sa, std::vector<uintlen_t> &sp) {
               if (I.dst == NONE || I.dst >= nv)
                 return;
               if (I.k == K::Const) {
                 if (I.dst < fn.nparams)
                   return; // param-defining placeholder (fnspec clones): keep
                 si.tag = opt_ns::vn_const_v;
                 si.c = I.c;
                 return;
               }
               if (I.k == K::Phi) {
                 // phis carry <= 2 incomings: I.a from pred I.t, I.b2 from pred
                 // I.f. A degenerate phi (a missing incoming, NONE) is
                 // phi-degen's business, not ours.
                 if (I.a == NONE || I.b2 == NONE)
                   return;
                 si.tag = opt_ns::vn_phi_v;
                 sa = {vn_u(I.a), vn_u(I.b2)};
                 sp = {vn_u(I.t), vn_u(I.f)};
                 return;
               }
               vn_fill_bin_site(I, nv, si, sa);
             });
  if (!did)
    return false;
  os.gvns += hits;
  return true;
}

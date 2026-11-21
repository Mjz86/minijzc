#pragma once
// === cse.hpp ========================================
// dominator-scoped common subexpression elimination.
//
// ALGORITHM: opt_cse has three stages. (1) Seed kconst/kval from every
//   K::Const, so two temps holding 7 key by VALUE, not by vreg. (2) A
//   preorder walk of the dominator tree with a SCOPED table -- the
//   generic value-numbering kernel (opt_ns::value_number): entering a
//   block scans its pure Bins once (each builds a key from resolved
//   operands, commutatives order operands by the mixed var/value
//   tag), a hit records ren[dst] = the table entry's def and drops
//   the recomputation, a miss pushes. (3) A second pass rewrites all
//   operands (I.a/I.b2/I.c2, I.args, Phi args included) with the
//   completed map; the sweep then rebuilds fn.code, rewriting each
//   fn.blocks range.
// DATA STRUCTURES:
//   * the kernel's scoped table + ren map (operand resolution to the
//     canonical representative, constant operands keyed by VALUE).
//   * the site tables this wrapper builds: which instrs are pure
//     Bins (the POLICY: Div/Mod only when I.nt proves them safe),
//     and their operand rows (the IR knowledge).
//   * drop: per-instr bytes telling the sweep which instrs to omit.
// THEORY: visibility is exactly "def dominates use" -- table entries
//   live along the dominator path only, so a hit means the dominating
//   def computes the same pure value on every path. Renaming waits for
//   a SECOND pass: a Phi argument is a use on an INCOMING EDGE, and
//   the replaced def's block may be scanned after the phi's block in
//   preorder -- rewriting during the walk would leave the phi reading
//   a dropped def; ren[v] = w implies w's def dominates every use of v.
// ENABLES: duplicate Bins vanish and their operands unify on the
//   surviving def: later passes see fewer instrs and one name per
//   expression, with fn.blocks ranges rewritten in place.

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../../ir/sweep.hpp"
#include "../analysis/an.hpp"
#include "../opt_generic.hpp"
#include "vn_sites.hpp"

// --- 9a3. dominator-scoped common subexpression elimination
// ------------------- A pure, non-trapping Bin recomputed under a *dominating*
// identical Bin is replaced by the dominating def. Visibility is exactly "def
// dominates use": we walk the dominator tree (from the lib) in preorder,
// keeping a scoped table — entries pushed on block entry are truncated on
// exit. The renaming map is applied in a SECOND pass over the whole function
// after the walk completes: a Phi's argument is a use on an INCOMING EDGE, so
// the block defining the replaced vreg can be scanned AFTER the phi's own
// block in preorder (a merge block does not sit below the predecessor in the
// dominator tree) — rewriting during the walk would leave the phi pointing at
// a dropped def (a ghost vreg reading as 0).
//
// The whole pass is the shared driver (vn_sites.hpp's vn_run) plus ONE
// policy line: every instr is offered as a pure Bin site (nt-gated
// div/mod; vn_fill_bin_site declines the rest).
constexpr bool opt_cse(Fn &fn, const An &A, OptStats &os) {
  uint32_t hits = 0;
  const bool did =
      vn_run(fn, A, hits,
             [](const Instr &I, uint32_t nv, opt_ns::vn_site_t<V> &si,
                std::vector<uintlen_t> &sa,
                std::vector<uintlen_t> &) { vn_fill_bin_site(I, nv, si, sa); });
  if (!did)
    return false;
  os.cses += hits;
  return true;
}

// --- 9b. unreachable block removal + phi arg pruning -------------------------

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
constexpr bool opt_cse(Fn &fn, const An &A, OptStats &os) {
  if (A.n != fn.nblocks() || !fn.nblocks())
    return false;
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  // known constants: two temps holding 7 are the SAME operand. CSE keys
  // compare constants by VALUE, not by vreg, so i*7 hits i*7.
  std::vector<char> kconst(nv, 0);
  std::vector<val_t> kval(nv, 0);
  for (const Instr &I : fn.code)
    if (I.k == K::Const && I.dst != NONE && I.dst < nv) {
      kconst[I.dst] = 1;
      kval[I.dst] = I.c;
    }
  // ---- the site tables (the wrapper's whole job): WHICH instrs take part
  // is policy (pure Bins only, Div/Mod gated on nt); the operand rows are
  // the IR knowledge; the walk, the scoped table and the ren map are the
  // kernel's (opt_ns::value_number).
  auto U = [](uint32_t v) { return v == NONE ? opt_ns::none_v : (uintlen_t)v; };
  const uint32_t n = (uint32_t)fn.code.size();
  std::vector<opt_ns::vn_site_t<V>> sinfo(n);
  std::vector<std::vector<uintlen_t>> sargs(n), spreds(n);
  std::vector<std::vector<uintlen_t>> sites_of_block(fn.nblocks());
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      sites_of_block[b].push_back(i);
      const Instr &I = fn.code[i];
      sinfo[i].dst = U(I.dst);
      if (I.k != K::Bin || I.dst == NONE || I.dst >= nv)
        continue;
      if (I.b == B::Div || I.b == B::Mod)
        if (!I.nt)
          continue; // may trap: keep (nt: proven safe, pure)
      sinfo[i].tag = opt_ns::vn_bin_v;
      sinfo[i].op = (uintlen_t)I.b;
      const bool comm = I.b == B::Add || I.b == B::Mul || I.b == B::Eq ||
                        I.b == B::Ne || I.b == B::And || I.b == B::Or ||
                        I.b == B::Xor || I.b == B::MulHi;
      sinfo[i].flags = comm ? opt_ns::vn_comm_v : 0;
      sargs[i] = {U(I.a), U(I.b2)};
    }
  const opt_ns::vn_result_t<V> R = opt_ns::value_number<V>(
      nv, A.dom_tree, sites_of_block, sinfo, sargs, spreds, kconst, kval);
  if (!R.hits)
    return false;
  auto resolve = [&](uint32_t v) -> uint32_t {
    while (v != NONE && v < nv && R.ren[v] != opt_ns::none_v)
      v = (uint32_t)R.ren[v];
    return v;
  };
  // second pass: rewrite EVERY operand (Phi args included) with the
  // COMPLETED map -- sound because ren[v] = w implies w's def dominates
  // v's def, which dominates every (edge-)use of v
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      Instr &I = fn.code[i];
      if (I.a != NONE)
        I.a = resolve(I.a);
      if (I.b2 != NONE)
        I.b2 = resolve(I.b2);
      if (I.c2 != NONE)
        I.c2 = resolve(I.c2);
      for (uint32_t &av : I.args)
        av = resolve(av);
    }
  // sweep CSE'd instrs, preserving block ranges
  os.cses += (uint32_t)R.hits;
  sweep_survivors(fn, R.keep);
  return true;
}

// --- 9b. unreachable block removal + phi arg pruning -------------------------

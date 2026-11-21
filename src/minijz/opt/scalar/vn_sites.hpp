#pragma once
// === vn_sites.hpp ========================================
// the shared site-table glue and DRIVER of cse and gvn (the ckpt-31
// dedup wave lifted the three helpers; ckpt 33 collapsed the passes'
// verbatim prologue/epilogue twins into vn_run). Both passes are thin
// policies over the ONE generic kernel, opt_ns::value_number
// (mjz_lib/opt/vn.hpp -- the dominator-scoped table walk with
// truncation, operand resolution, const keying by VALUE, commutative
// canonicalization and the phi congruence taint rule). The passes
// differ ONLY in which entry kinds they admit:
//   * cse  -- Bin sites (pure, nt-gated div/mod)
//   * gvn  -- Bin sites + Const entries + complete phis
// The shared pieces, once:
//   * vn_seed_operand_consts -- kconst/kval: two temps holding 7 are
//     the SAME operand (operands key by VALUE, not by vreg)
//   * vn_fill_bin_site -- the Bin classification: the dst gate, the
//     may-trap Div/Mod gate (nt: proven safe), the commutative
//     family, the operand rows
//   * vn_rewrite_completed_map -- the SECOND pass: rewrite every
//     operand of every instr (Phi args included) with the completed
//     ren map, chained through resolve. Sound because ren[v] = w
//     implies w's def DOMINATES v's def, which dominates every
//     (edge-)use of v -- rewriting during the walk would leave a phi
//     reading a dropped def (a ghost vreg reading as 0).
//   * vn_run -- the whole pass skeleton: gate, seeding, census, kernel
//     call, rewrite, sweep. The pass states its fill-site policy and
//     bumps its own stats counter; everything else is here.
#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../../ir/sweep.hpp"
#include "../analysis/an.hpp"
#include "../opt_generic.hpp"

// the NONE -> none_v conversion every site table needs
inline uintlen_t vn_u(uint32_t v) {
  return v == NONE ? opt_ns::none_v : (uintlen_t)v;
}

// classify a pure Bin site; false = declined (not a Bin / no dst /
// may-trap div-mod without the nt proof). fills tag/op/flags and the
// operand row (a, b2).
inline bool vn_fill_bin_site(const Instr &I, uint32_t nv,
                             opt_ns::vn_site_t<V> &si,
                             std::vector<uintlen_t> &sa) {
  if (I.k != K::Bin || I.dst == NONE || I.dst >= nv)
    return false;
  if (I.b == B::Div || I.b == B::Mod)
    if (!I.nt)
      return false; // may trap: keep (nt: proven safe, pure)
  si.tag = opt_ns::vn_bin_v;
  si.op = (uintlen_t)I.b;
  const bool comm = I.b == B::Add || I.b == B::Mul || I.b == B::Eq ||
                    I.b == B::Ne || I.b == B::And || I.b == B::Or ||
                    I.b == B::Xor || I.b == B::MulHi;
  si.flags = comm ? opt_ns::vn_comm_v : 0;
  sa = {vn_u(I.a), vn_u(I.b2)};
  return true;
}

// rewrite EVERY operand with the COMPLETED map (Phi args included) --
// the second pass both passes run after the kernel walk
inline void vn_rewrite_completed_map(Fn &fn, uint32_t nv,
                                     const opt_ns::vn_result_t<V> &R) {
  auto resolve = [&](uint32_t v) -> uint32_t {
    while (v != NONE && v < nv && R.ren[v] != opt_ns::none_v)
      v = (uint32_t)R.ren[v];
    return v;
  };
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
      rewrite_operands(fn.code[i], [&](uint32_t &v) { v = resolve(v); });
}

// ---- the shared cse/gvn driver --------------------------------------------
// Everything the two passes share, once: the freshness gate, the const
// seeding, the site-table census (the dst row plus the POLICY fill),
// the kernel call, the completed-map rewrite and the survivor sweep.
// The passes differ in exactly two things: WHICH instrs admit entries
// (the fill callback -- cse admits pure Bin sites only; gvn adds const
// and complete-phi entries) and the stats counter they bump. The
// prologue/epilogue twins (~30 lines x2) collapsed here in the
// ckpt-33 dedup wave.
template <class Fill>
constexpr bool vn_run(Fn &fn, const An &A, uint32_t &hits, Fill &&fill) {
  if (A.n != fn.nblocks() || !fn.nblocks())
    return false;
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  // known constants (the shared seeding): two temps holding 7 are the
  // SAME operand -- operands key by VALUE, not by vreg
  std::vector<char> kconst{};
  std::vector<val_t> kval{};
  seed_consts(fn, nv, kconst, kval);
  // ---- the site tables: WHICH instrs take part is policy (the fill
  // callback); the walk, the scoped table and the ren map are the
  // kernel's (opt_ns::value_number)
  const uint32_t n = (uint32_t)fn.code.size();
  std::vector<opt_ns::vn_site_t<V>> sinfo(n);
  std::vector<std::vector<uintlen_t>> sargs(n), spreds(n);
  std::vector<std::vector<uintlen_t>> sites_of_block(fn.nblocks());
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      sites_of_block[b].push_back(i);
      sinfo[i].dst = vn_u(fn.code[i].dst);
      fill(fn.code[i], nv, sinfo[i], sargs[i], spreds[i]);
    }
  const opt_ns::vn_result_t<V> R = opt_ns::value_number<V>(
      nv, A.dom_tree, sites_of_block, sinfo, sargs, spreds, kconst, kval);
  if (!R.hits)
    return false;
  // second pass with the COMPLETED map (the shared rewrite)
  vn_rewrite_completed_map(fn, nv, R);
  hits = (uint32_t)R.hits;
  // sweep dropped instrs, preserving block ranges
  sweep_survivors(fn, R.keep);
  return true;
}

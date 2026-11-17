#pragma once
// === vn_sites.hpp ========================================
// the shared site-table glue of cse and gvn (the ckpt-31 dedup wave:
// both wrappers hand-rolled these three pieces verbatim until they
// were lifted here). Both passes are thin policies over the ONE
// generic kernel, opt_ns::value_number (mjz_lib/opt/vn.hpp -- the
// dominator-scoped table walk with truncation, operand resolution,
// const keying by VALUE, commutative canonicalization and the phi
// congruence taint rule). The passes differ ONLY in which entry
// kinds they admit:
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
#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
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
}

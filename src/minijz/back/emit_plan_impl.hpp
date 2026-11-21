#pragma once
// === emit_plan_impl.hpp ========================================
// the emission-plan bodies: mark_referenced_blocks,
// append_source_range_banner, make_emit_plan (see emit_plan.hpp for the
// algorithm + theory). Included by emit_plan.hpp in the header-only build
// (inline, via MJZ_PRINTER_FN) and by tu/runners.cpp in the multi-TU build
// (plain external linkage) -- include it nowhere else.

#include "emit_plan.hpp"

MJZ_PRINTER_FN std::vector<char> mark_referenced_blocks(
    const Fn &fn, const EmitPlan &P) {
  const uint32_t nb = fn.nblocks();
  const uint32_t nv = (uint32_t)P.kc.size();
  std::vector<char> refd(nb, 0);
  for (uint32_t b : P.order)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (P.dead[i])
        continue;
      if (I.k == K::Br && I.t != P.next[b])
        refd[I.t] = 1;
      if (I.k == K::Cbr) {
        if (P.fuse[i] == NONE && I.a < nv && P.kc[I.a]) {
          uint32_t tgt = (P.kv[I.a] != 0) ? I.t : I.f; // constant condition
          if (tgt != P.next[b])
            refd[tgt] = 1;
        } else if (I.f == P.next[b]) {
          refd[I.t] = 1; // the false arm falls through, label the true arm
        } else if (I.t == P.next[b]) {
          refd[I.f] = 1; // the true arm falls through, label the false arm
        } else {
          refd[I.t] = 1;
          refd[I.f] = 1;
        }
      }
    }
  return refd;
}

MJZ_PRINTER_FN void append_source_range_banner(
    std::string &s, const Fn &fn, const SrcMap &SM, bool is_main,
    std::string_view fname, const char *open, const char *close) {
  uint32_t lo = NONE, hi = 0;
  for (const Instr &I : fn.code)
    if (I.src != NONE) {
      if (lo == NONE || I.src < lo)
        lo = I.src;
      if (I.src > hi)
        hi = I.src;
    }
  if (lo != NONE && SM.ok() && lo <= SM.src.size()) {
    char b2[96];
    if (is_main)
      std::snprintf(b2, sizeof b2, "%smain: source lines %u..%u%s", open,
                    (unsigned)SM.line(lo), (unsigned)SM.line(hi), close);
    else
      std::snprintf(b2, sizeof b2, "%sfn %s: source lines %u..%u%s", open,
                    std::string(fname).c_str(), (unsigned)SM.line(lo),
                    (unsigned)SM.line(hi), close);
    s += b2;
  }
}

MJZ_PRINTER_FN EmitPlan make_emit_plan(const Fn &fn) {
  EmitPlan P{};
  const uint32_t nb = fn.nblocks();
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  // the kernel rows: valid succs in SLOT order (the tiebreak vocabulary)
  std::vector<std::vector<ulen>> succ_rows(nb);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t s : {fn.succ0(b), fn.succ1(b)})
      if (s != NONE && s < nb)
        succ_rows[b].push_back(s);
  // reachability from entry + RPO (succ0 first => loop headers precede
  // their bodies) + the trace chains: all three in the library now
  const std::vector<char> reach =
      opt_ns::calculate_reachable_mask(succ_rows, 0);
  const std::vector<ulen> rpo =
      opt_ns::calculate_reverse_postorder(succ_rows, reach, 0);
  std::vector<ulen> score(nb, 0);
  for (uint32_t b = 0; b < nb; ++b)
    score[b] = ulen(fn.preds[b].size()); // fewer preds: the cheaper jump
  const opt_ns::trace_layout_t tr =
      opt_ns::calculate_trace_layout(succ_rows, reach, score, rpo);
  P.order.clear();
  P.order.reserve(tr.order.size());
  for (ulen b : tr.order)
    P.order.push_back(uint32_t(b));
  P.next.assign(nb, NONE);
  for (uint32_t b = 0; b < nb; ++b)
    P.next[b] = (tr.next[b] == opt_ns::none_v) ? NONE : uint32_t(tr.next[b]);
  // constants: a vreg is literal-inlinable ONLY when it has exactly ONE def
  // and that def is a Const. Post-destruction phi slots can hold several
  // defs, one of which post_destruct_peephole may have folded into a Const
  // (e.g. `c3 = 5` on the entry edge, `c3 = t` on the back edge): inlining
  // the one Const def at every use would freeze the slot's other-path
  // values to that constant and miscompile the loop test.
  seed_single_def_consts(fn, nv, P.kc, P.kv);
  // branch fusion: the shared gate (make_fuse_table below) -- astrun's
  // slot allocator calls the SAME function, so the mirror can never drift
  P.fuse = make_fuse_table(fn, nv).fuse;
  // textual uses (consts inlined, fused cbrs read the comparison operands)
  P.uses.assign(nv, 0);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k == K::Const)
        continue; // inlined at every use
      if (P.fuse[i] != NONE) {
        const Instr &D = fn.code[P.fuse[i]];
        if (D.a != NONE && D.a < nv && !P.kc[D.a])
          P.uses[D.a]++;
        if (D.b2 != NONE && D.b2 < nv && !P.kc[D.b2])
          P.uses[D.b2]++;
        continue;
      }
      for (uint32_t v : {I.a, I.b2, I.c2})
        if (v != NONE && v < nv && !P.kc[v])
          P.uses[v]++;
      for (uint32_t v : I.args)
        if (v != NONE && v < nv && !P.kc[v])
          P.uses[v]++;
    }
  // dead: pure instrs whose dst is never textually read
  P.dead.assign(fn.code.size(), 0);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k == K::Const) {
        // dead (inlined at every use) only when the slot is const-inlinable,
        // i.e. single-def; a multi-def slot's Const edge write is real code
        P.dead[i] = (I.dst != NONE && I.dst < nv && P.kc[I.dst]) ? 1 : 0;
        continue;
      }
      if (I.dst == NONE || I.dst >= nv)
        continue;
      if (P.uses[I.dst] != 0)
        continue;
      bool pure = I.k == K::Copy || I.k == K::Len ||
                  (I.k == K::Bin && I.b != B::Div && I.b != B::Mod);
      if (pure)
        P.dead[i] = 1;
    }
  for (uint32_t d : P.fuse)
    if (d != NONE)
      P.dead[d] = 1; // consumed by the branch text
  return P;
}

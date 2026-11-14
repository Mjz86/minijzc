#pragma once
// === regalloc.hpp ========================================
// the x86-64 register allocator (split from emit_x64_impl.hpp, checkpoint
// 21): RaPlan + ra_plan — block liveness (backward fixpoint), the SSA
// interference graph, chordal coloring via mjz_lib (greedy in
// maximum-cardinality-search order — OPTIMAL for chordal graphs, and
// SSA-shaped interference graphs are chordal), and the 5-register
// assignment (colors 0..4 -> rbx,r12..r15, the callee-saved pool, so
// anything live across a call needs no extra saving; the rest spill to
// the classic -N(%rbp) slots). The audit layer (regaudit.hpp) reads the
// SAME plan the codegen gets — one allocator, two consumers.
//
// MECHANICS (liveness -> interference -> coloring -> homes):
//   * BACKWARD LIVENESS. lin/lout per block, computed by a backward
//     fixpoint (up to 64 rounds): live-out(b) = union of live-in of
//     b's successors; inside a block, scan instructions BACKWARD
//     with def-kills-use (a def removes its vreg from live, a use
//     adds it). Dead instrs (P.dead) and inlined consts (P.kc) are
//     invisible to the reader -- the fused Cbr reads the compare's
//     OPERANDS (each_use), which is what keeps slots alive where
//     the branch fusion needs them.
//   * INTERFERENCE. Walk each block backward with `live` seeded
//     from live-out: at a def, add edges (def, every vreg live
//     after it), then kill the def. Parameters interfere with
//     everything live into entry AND PAIRWISE ALWAYS (the prologue
//     unconditionally moves every argument into its home -- fuzz
//     seed 804, documented at the edge() calls above).
//   * COLORING. mjz_lib's color_chordal_graph: greedy in maximum-
//     cardinality-search order, OPTIMAL on chordal graphs (colors =
//     clique number), and these interference graphs are SSA-shaped
//     enough to be chordal in practice.
//   * HOMES. color 0..4 = rbx,r12..r15 (callee-saved: anything
//     live across a call needs no save); color >= 5 spills to
//     -N(%rbp) with spill[] ordinals; usedmask drives the prologue's
//     counted push/pop of the used subset only.

#include "../base/pass_cfg.hpp"
#include "../graph_lib.hpp"
#include "../ir/instr.hpp"
#include "emit_plan.hpp"

#define MJZ_EMIT_FN // runtime-only (static locals): plain in both modes

// ---- 13.5b.0 register allocation (chordal graph coloring, via mjz_lib) -----
// Live ranges are computed on the destroyed code (backward dataflow to a
// fixpoint); at every definition the def interferes with everything live
// after it. That interference graph is handed to mjz::graph_ns::
// color_chordal_graph — greedy coloring in maximum-cardinality-search order,
// which is OPTIMAL for chordal graphs, and SSA-shaped interference graphs are
// chordal. The pool is the five callee-saved registers rbx/r12-r15, so any
// value live across a call is safe with no extra saving; callers only need
// the (counted) push/pop of the subset actually used. Anything the coloring
// cannot house (color >= 5) spills to the classic -N(%rbp) slot.
struct RaPlan {
  std::vector<int32_t> color{};  // -1: no location; 0..4: register; 5: spill
  std::vector<uint32_t> spill{}; // per vreg: spill slot ordinal
  uint32_t nspill{};
  uint32_t usedmask{};
};

MJZ_EMIT_FN RaPlan ra_plan(const Fn &fn, const EmitPlan &P) {
  const uint32_t nb = fn.nblocks();
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  RaPlan R{};
  R.color.assign(nv, -1);
  R.spill.assign(nv, 0);
  if (!nb || !nv)
    return R;
  // textual-use reader honoring inlined consts and fused branches
  auto each_use = [&](uint32_t i, auto &&f) {
    const Instr &I = fn.code[i];
    if (P.fuse[i] != NONE) { // the branch reads the comparison's operands
      const Instr &D = fn.code[P.fuse[i]];
      auto one = [&](uint32_t v) {
        if (v != NONE && v < nv && !P.kc[v])
          f(v);
      };
      one(D.a);
      one(D.b2);
      return;
    }
    auto one = [&](uint32_t v) {
      if (v != NONE && v < nv && !P.kc[v])
        f(v);
    };
    one(I.a);
    one(I.b2);
    one(I.c2);
    for (uint32_t v : I.args)
      one(v);
  };
  auto is_def = [&](uint32_t i) -> uint32_t { // live def of instr i (or NONE)
    const Instr &I = fn.code[i];
    if (P.dead[i])
      return NONE;
    if (I.dst == NONE || I.dst >= nv || P.kc[I.dst])
      return NONE;
    return I.dst;
  };
  // ---- block liveness (backward scan, fixpoint over successors) ----------
  std::vector<std::vector<char>> lin(nb, std::vector<char>(nv, 0)),
      lout(nb, std::vector<char>(nv, 0));
  for (int round = 0; round < 64; ++round) {
    bool changed = false;
    for (int32_t b = (int32_t)nb - 1; b >= 0; --b) {
      std::vector<char> &lo = lout[(uint32_t)b];
      std::fill(lo.begin(), lo.end(), 0);
      for (uint32_t s : {fn.succ0((uint32_t)b), fn.succ1((uint32_t)b)})
        if (s != NONE && s < nb)
          for (uint32_t v = 0; v < nv; ++v)
            lo[v] |= lin[s][v];
      std::vector<char> live = lo;
      for (uint32_t i = fn.blocks[(uint32_t)b].end;
           i-- > fn.blocks[(uint32_t)b].begin;) {
        if (P.dead[i])
          continue;
        uint32_t d = is_def(i);
        if (d != NONE)
          live[d] = 0;
        each_use(i, [&](uint32_t v) { live[v] = 1; });
      }
      for (uint32_t v = 0; v < nv; ++v)
        if (live[v] != lin[(uint32_t)b][v]) {
          lin[(uint32_t)b][v] = live[v];
          changed = true;
        }
    }
    if (!changed)
      break;
  }
  // ---- interference edges ------------------------------------------------
  std::vector<std::vector<ulen>> adj(nv);
  auto edge = [&](uint32_t a, uint32_t b) {
    if (a == b || a >= nv || b >= nv)
      return;
    for (uint32_t x : adj[a])
      if (x == b)
        return;
    adj[a].push_back(b);
    adj[b].push_back(a);
  };
  { // parameters are "defined at entry": they conflict with each other and
    // with anything live into the entry block
    std::vector<char> live = lin[0];
    for (uint32_t p = 0; p < fn.nparams && p < nv; ++p) {
      for (uint32_t u = 0; u < nv; ++u)
        if (live[u] && u != p)
          edge(p, u);
      live[p] = 0;
    }
    // ... and PAIRWISE, always: the prologue unconditionally moves every
    // incoming argument into its home, so even a param whose only uses sit
    // in unreachable code (not live-in, no edges above) must not share a
    // home with a live sibling — the entry move would clobber it (fuzz
    // seed 804: p0 and p1 both colored rbx, `mov %rsi,%rbx` destroyed p0)
    for (uint32_t a = 0; a < fn.nparams && a < nv; ++a)
      for (uint32_t b = a + 1; b < fn.nparams && b < nv; ++b)
        edge(a, b);
  }
  for (uint32_t b = 0; b < nb; ++b) {
    std::vector<char> live = lout[b];
    for (uint32_t i = fn.blocks[b].end; i-- > fn.blocks[b].begin;) {
      if (P.dead[i])
        continue;
      uint32_t d = is_def(i);
      if (d != NONE) {
        for (uint32_t u = 0; u < nv; ++u)
          if (live[u])
            edge(d, u);
        live[d] = 0;
      }
      each_use(i, [&](uint32_t v) { live[v] = 1; });
    }
  }
  // ---- the lib does the coloring ------------------------------------------
  auto forest = gns::make_basic_forest<V>(adj);
  auto colored = gns::color_chordal_graph<V>(forest);
  // ---- assignment: colors 0..4 -> rbx,r12..r15; the rest spill ------------
  std::vector<char> hasloc(nv, 0);
  for (uint32_t v = 0; v < fn.nparams && v < nv; ++v)
    hasloc[v] = 1;
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
      if (is_def(i) != NONE)
        hasloc[fn.code[i].dst] = 1;
  for (uint32_t b = 0; b < nb; ++b) // defensive: used-but-undefined
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
      each_use(i, [&](uint32_t v) { hasloc[v] = 1; });
  uint32_t spills = 0;
  for (uint32_t v = 0; v < nv; ++v) {
    if (!hasloc[v])
      continue;
    uint64_t c = (v < colored.first.size()) ? (uint64_t)colored.first[v] : 0;
    if (c < 5) {
      R.color[v] = (int32_t)c;
      R.usedmask |= (uint32_t)1 << c;
    } else {
      R.color[v] = 5;
      R.spill[v] = spills++;
    }
  }
  R.nspill = spills;
  return R;
}

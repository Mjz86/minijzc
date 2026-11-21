#pragma once
// === regalloc.hpp ========================================
// the x86-64 register allocator (split from emit_x64_impl.hpp, checkpoint
// 21): RaPlan + ra_plan — block liveness, the SSA interference graph,
// chordal coloring via mjz_lib, and the 5-register assignment (colors
// 0..4 -> rbx,r12..r15, the callee-saved pool, so anything live across
// a call needs no extra saving; the rest spill to the classic -N(%rbp)
// slots). The audit layer (regaudit.hpp) reads the SAME plan the
// codegen gets — one allocator, two consumers.
//
// MECHANICS (liveness -> interference -> coloring -> homes):
//   * BACKWARD LIVENESS. lin/lout per block, the fixpoint in the
//     library now (opt/liveness.hpp's calculate_block_liveness --
//     the round-robin sweep this file hand-rolled until the kernel
//     lift): live-out(b) = union of live-in of b's successors; inside
//     a block, instructions scanned BACKWARD with def-kills-use.
//     Dead instrs (P.dead) and inlined consts (P.kc) are invisible to
//     the reader -- the fused Cbr reads the compare's OPERANDS
//     (each_use), which is what keeps slots alive where the branch
//     fusion needs them.
//   * INTERFERENCE. calculate_interference_rows (the same header):
//     walk each block backward with `live` seeded from live-out; at a
//     def, add edges (def, every vreg live after it), then kill the
//     def. Parameters are the entry defs: they interfere with
//     everything live into entry AND PAIRWISE ALWAYS (the prologue
//     unconditionally moves every argument into its home -- fuzz
//     seed 804, documented in the kernel).
//   * COLORING. mjz_lib's color_chordal_graph: greedy in maximum-
//     cardinality-search order, OPTIMAL on chordal graphs (colors =
//     clique number), and these interference graphs are SSA-shaped
//     enough to be chordal in practice.
//   * HOMES. color 0..4 = rbx,r12..r15 (callee-saved: anything
//     live across a call needs no save); color >= 5 spills to
//     -N(%rbp) with spill[] ordinals; usedmask drives the prologue's
//     counted push/pop of the used subset only.
//
// THE ADAPTER'S TABLES (what the kernel lift left here): the
// textual-use reader (each_use -- the fuse/kc visibility policy) and
// the def reader (is_def) materialize args_of_site/def_of_site/
// skip_site rows; the succ rows filter NONE. Everything else is the
// IR knowledge the kernel must not know (which vregs exist, what a
// home is, the 5-register pool).

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
  const uint32_t ni = uint32_t(fn.code.size());
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
    for_each_operand(I, one); // the shared enumeration (instr.hpp)
  };
  auto is_def = [&](uint32_t i) -> uint32_t { // live def of instr i (or NONE)
    const Instr &I = fn.code[i];
    if (P.dead[i])
      return NONE;
    if (I.dst == NONE || I.dst >= nv || P.kc[I.dst])
      return NONE;
    return I.dst;
  };
  // ---- the kernel tables (the adapter's whole job) ------------------------
  std::vector<std::vector<ulen>> succ_rows(nb);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t s : {fn.succ0(b), fn.succ1(b)})
      if (s != NONE && s < nb)
        succ_rows[b].push_back(s);
  std::vector<std::pair<ulen, ulen>> block_sites(nb);
  for (uint32_t b = 0; b < nb; ++b)
    block_sites[b] = {fn.blocks[b].begin, fn.blocks[b].end};
  std::vector<std::vector<ulen>> args_of_site(ni);
  std::vector<ulen> def_of_site(ni, opt_ns::none_v);
  std::vector<char> skip_site(ni, 0);
  for (uint32_t i = 0; i < ni; ++i) {
    skip_site[i] = P.dead[i] ? 1 : 0;
    if (P.dead[i])
      continue; // invisible to every reader
    each_use(i, [&](uint32_t v) { args_of_site[i].push_back(v); });
    const uint32_t d = is_def(i);
    def_of_site[i] = (d == NONE) ? opt_ns::none_v : ulen(d);
  }
  // ---- liveness + interference: the library kernels ----------------------
  const auto liv = opt_ns::calculate_block_liveness<V>(
      succ_rows, block_sites, args_of_site, def_of_site, skip_site, nv, 64);
  std::vector<ulen> entry_defs{};
  for (uint32_t p = 0; p < fn.nparams && p < nv; ++p)
    entry_defs.push_back(p);
  const auto adj = opt_ns::calculate_interference_rows<V>(
      block_sites, args_of_site, def_of_site, skip_site, liv, nv, 0,
      entry_defs);
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

#pragma once
// === copyprop.hpp ========================================
// copy propagation.
//
// ALGORITHM: opt_copyprop makes two sweeps. (1) Recording: every
//   eligible K::Copy `d = s` (a != dst) becomes a pair; the generic
//   rename builder (opt_ns::build_copy_rename) walks them in code
//   order, resolving sources transitively through the map being
//   built (chains die in one hop) and refusing the CROSSED closing
//   copy of a cyclic rename (resolve(src) == dst -- a self-loop
//   entry that would never resolve; the phi-degenerate hazard, fuzz
//   seed 7406). (2) Applying: self-copies (a == dst) and propagated
//   copies (ren[dst] set) are dropped; every other operand --
//   I.a, I.b2, I.c2 (a Store's stored value), I.args (Call
//   arguments) -- is rewritten through the capped resolve
//   (opt_ns::resolve_capped); the sweep then rebuilds fn.code per
//   block range.
// DATA STRUCTURES:
//   * the kernel's ren map (nv wide, none where untouched); a single
//     write per dst keeps it acyclic, the crossed-pair guard keeps it
//     self-loop-free, the resolve cap is belt and braces so a future
//     multi-def IR cannot resurrect an infinite walk.
//   * drop: per-instr bytes selecting the copies the sweep omits.
// THEORY: SSA gives each vreg exactly one def, and a copy's source
//   dominates the copy (it must run first), hence the source dominates
//   every replaced use -- always sound. The exception: CROSSED
//   phi-degenerate copies around a loop (`t473 = t496` + `t496 = t521`
//   + `t521 = t473`) -- each source is defined only on the incoming
//   edge, so recording the closing copy would make resolve(a) == dst,
//   a SELF-LOOP that hung every later resolve (fuzz seed 7406, -O1
//   with dce disabled); crossed copies stay unpropagated (still sound).
// ENABLES: this is the pass that cleans up phi-to-copy degenerations
//   (unreach and dce both emit such copies) and lets CSE see canonical
//   names; the dropped copies leave fewer instrs for later sweeps.

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../../ir/sweep.hpp"
#include "../analysis/an.hpp"
#include "../opt_generic.hpp"

// --- 9a2. copy propagation
// ---------------------------------------------------- `d = s` (Copy): every
// use of d becomes s and the copy dies. SSA gives each vreg exactly one def,
// and the copy's source dominates the copy, hence the source dominates every
// replaced use — always sound. This is the pass that cleans up phi-to-copy
// degenerations and lets CSE see canonical names.
//
// The dominance argument does NOT hold for CROSSED phi-degenerate copies:
// around a loop, degeneration can materialize `t473 = t496` and
// `t496 = t521` + `t521 = t473` (each source is only defined on the incoming
// edge, not dominating). Recording ren[dst] = resolve(a) for the closing copy
// then yields resolve(a) == dst — a SELF-LOOP that sends every later resolve
// into an infinite `while (ren[v] != NONE)` walk (fuzz seed 7406, -O1 with
// dce disabled, hung here). Such crossed copies stay unpropagated instead:
// uses keep their original names (conservative, still sound). Both rules live
// in the generic rename builder now.
constexpr bool opt_copyprop(Fn &fn, OptStats &os) {
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  // the recording sweep: eligible copies as (dst, src) pairs in code
  // order (the whole IR knowledge the kernel needs)
  std::vector<std::pair<uintlen_t, uintlen_t>> pairs{};
  bool had = false;
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k == K::Copy && I.dst != NONE && I.a != NONE && I.a != I.dst) {
        pairs.push_back({(uintlen_t)I.dst, (uintlen_t)I.a});
        had = true;
      }
    }
  if (!had)
    return false;
  // the rename map: transitive chains die in one hop; crossed closing
  // copies stay unrecorded (the self-loop guard)
  const std::vector<uintlen_t> ren = opt_ns::build_copy_rename(nv, pairs);
  auto resolve = [&](uint32_t v) -> uint32_t {
    return (uint32_t)opt_ns::resolve_capped(ren, v == NONE ? opt_ns::none_v
                                                           : (uintlen_t)v);
  };
  bool changed = false;
  std::vector<char> keep(fn.code.size(), 1);
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k == K::Copy && I.dst != NONE &&
          (I.a == I.dst || ren[I.dst] != opt_ns::none_v)) { // self/propagated
        keep[i] = 0;
        if (I.a != I.dst)
          os.copyprops++;
        changed = true;
        continue;
      }
      if (I.a != NONE) {
        uint32_t r = resolve(I.a);
        changed |= (r != I.a);
        I.a = r;
      }
      if (I.b2 != NONE) {
        uint32_t r = resolve(I.b2);
        changed |= (r != I.b2);
        I.b2 = r;
      }
      if (I.c2 != NONE) { // Store: the stored value
        uint32_t r = resolve(I.c2);
        changed |= (r != I.c2);
        I.c2 = r;
      }
      for (uint32_t &av : I.args) { // Call arguments
        uint32_t r = resolve(av);
        changed |= (r != av);
        av = r;
      }
    }
  if (changed) // sweep dropped copies, preserving block ranges
    sweep_survivors(fn, keep);
  return changed;
}

// --- 9a4. canonicalization
// ----------------------------------------------------

#pragma once
// === copyprop.hpp ========================================
// copy propagation.

#include "minijz/base/pass_cfg.hpp"
#include "minijz/ir/instr.hpp"
#include "minijz/opt/analysis/an.hpp"

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
// uses keep their original names (conservative, still sound).
constexpr bool opt_copyprop(Fn &fn, OptStats &os) {
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  std::vector<uint32_t> ren(nv, NONE);
  auto resolve = [&](uint32_t v) -> uint32_t {
    uint32_t steps = 0;
    while (v != NONE && v < nv && ren[v] != NONE) {
      v = ren[v];
      // belt-and-braces: ren stays acyclic by construction (single write per
      // dst + the self-loop guard below); the cap keeps any future
      // multi-def IR from resurrecting the infinite walk
      if (++steps > nv)
        break;
    }
    return v;
  };
  bool had = false;
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k == K::Copy && I.dst != NONE && I.a != NONE && I.a != I.dst) {
        uint32_t r = resolve(I.a); // sources resolve transitively: chains die
        if (r != I.dst)            // r == dst: crossed pair — do not record
          ren[I.dst] = r;          // (a self-loop entry would never resolve)
        had = true;
      }
    }
  if (!had)
    return false;
  bool changed = false;
  std::vector<char> drop(fn.code.size(), 0);
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k == K::Copy && I.dst != NONE &&
          (I.a == I.dst || ren[I.dst] != NONE)) { // self copy or propagated
        drop[i] = 1;
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
  if (changed) { // sweep dropped copies, preserving block ranges
    std::vector<Instr> nc{};
    nc.reserve(fn.code.size());
    for (uint32_t b = 0; b < fn.nblocks(); ++b) {
      uint32_t ob = fn.blocks[b].begin, oe = fn.blocks[b].end;
      fn.blocks[b].begin = (uint32_t)nc.size();
      for (uint32_t i = ob; i < oe; ++i)
        if (!drop[i])
          nc.push_back(fn.code[i]);
      fn.blocks[b].end = (uint32_t)nc.size();
    }
    fn.code = std::move(nc);
  }
  return changed;
}

// --- 9a4. canonicalization
// ----------------------------------------------------

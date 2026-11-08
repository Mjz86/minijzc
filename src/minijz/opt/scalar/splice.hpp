#pragma once
// === splice.hpp ========================================
// block splicing (single-pred/single-succ shortcutting).

#include "minijz/base/pass_cfg.hpp"
#include "minijz/ir/instr.hpp"
#include "minijz/opt/analysis/an.hpp"

// --- 9b2. block splicing
// ------------------------------------------------------ If a single-succ block
// b ends in `br s`, and s has exactly one pred (b) and no phis, s's code is
// appended to b (b's Br dropped). This collapses the jump chains that branch
// folding leaves behind. Phi pred ids in s's successors are rekeyed s -> b so
// phi args stay pred-identified.
constexpr bool opt_splice(Fn &fn, OptStats &os) {
  uint32_t nb = fn.nblocks();
  std::vector<uint32_t> absorbed(nb, NONE); // block -> absorber
  std::vector<uint32_t> merges(nb, NONE);   // absorber -> absorbed block
  bool any = false;
  for (uint32_t b = 0; b < nb; ++b) {
    if (absorbed[b] != NONE || merges[b] != NONE)
      continue; // already in a pair this sweep (matchings only: no chains)
    uint32_t s = fn.succ0(b);
    if (fn.succ1(b) != NONE || s == NONE)
      continue; // dead or 2-succ
    if (s == b || s == 0)
      continue; // self-loop / entry: keep
    if (absorbed[s] != NONE || merges[s] != NONE)
      continue;
    if (fn.preds[s].size() != 1 || fn.preds[s][0] != b)
      continue;
    bool s_has_phi = false;
    for (uint32_t i = fn.blocks[s].begin; i < fn.blocks[s].end; ++i)
      if (fn.code[i].k == K::Phi) {
        s_has_phi = true;
        break;
      }
    if (s_has_phi)
      continue;
    // absorb s into b
    absorbed[s] = b;
    merges[b] = s;
    any = true;
    os.splices++;
    // rekey phis in s's successors (pred s -> pred b) and refresh their preds
    for (uint32_t t : {fn.succ0(s), fn.succ1(s)}) {
      if (t == NONE)
        continue;
      for (uint32_t i = fn.blocks[t].begin; i < fn.blocks[t].end; ++i) {
        Instr &I = fn.code[i];
        if (I.k != K::Phi)
          break;
        if (I.t == s)
          I.t = b;
        if (I.f == s)
          I.f = b;
      }
      for (uint32_t &p : fn.preds[t])
        if (p == s)
          p = b;
    }
  }
  if (!any)
    return false;
  // rebuild: absorbed blocks vanish; an absorber emits its own non-terminator
  // instrs, then its absorbed block's instrs, then (that block may itself
  // have absorbed someone in this sweep) the whole chain, ending at the final
  // terminator. Ranges AND succs are read from SNAPSHOTS: the rebuild
  // overwrites fn.blocks/fn.succ while chain ends still need the absorbed
  // blocks' ORIGINAL successors (an absorbed block isolated earlier in this
  // same loop must not poison its absorber with NONE succs).
  std::vector<Block> old_blocks = fn.blocks;
  std::vector<uint32_t> old_succ = fn.succ;
  std::vector<Instr> nc{};
  nc.reserve(fn.code.size());
  for (uint32_t b = 0; b < nb; ++b) {
    if (absorbed[b] != NONE) {
      fn.blocks[b] = {(uint32_t)nc.size(), (uint32_t)nc.size()};
      fn.succ[2 * b] = NONE;
      fn.succ[2 * b + 1] = NONE;
      continue;
    }
    uint32_t begin = (uint32_t)nc.size();
    uint32_t x = b;
    for (;;) {
      uint32_t xb = old_blocks[x].begin, xe = old_blocks[x].end;
      uint32_t m = merges[x];
      if (m == NONE) { // chain end: emit everything incl. the terminator
        for (uint32_t i = xb; i < xe; ++i)
          nc.push_back(fn.code[i]);
        fn.succ[2 * b] = old_succ[2 * x];
        fn.succ[2 * b + 1] = old_succ[2 * x + 1];
        break;
      }
      // x absorbed m: emit x's instrs but its trailing Br, then follow
      if (xe > xb)
        for (uint32_t i = xb; i + 1 < xe; ++i)
          nc.push_back(fn.code[i]);
      x = m;
    }
    fn.blocks[b] = {begin, (uint32_t)nc.size()};
  }
  fn.code = std::move(nc);
  fn.recompute_preds();
  return true;
}

// --- 9c. SSA dead-code elimination -------------------------------------------

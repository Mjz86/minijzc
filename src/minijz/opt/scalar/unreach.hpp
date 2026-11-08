#pragma once
// === unreach.hpp ========================================
// unreachable-block removal + phi arg pruning.

#include "minijz/base/pass_cfg.hpp"
#include "minijz/ir/instr.hpp"
#include "minijz/opt/analysis/an.hpp"

// --- 9b. unreachable block removal + phi arg pruning -------------------------
// Phi instrs carry their pred block ids in t/f (set during SSA construction),
// so pruning is by pred identity — no guesswork.
constexpr bool opt_remove_unreachable(Fn &fn, OptStats &os) {
  std::vector<char> reach(fn.nblocks(), 0);
  std::vector<uint32_t> wl{0};
  reach[0] = 1;
  while (!wl.empty()) {
    uint32_t b = wl.back();
    wl.pop_back();
    for (uint32_t s : {fn.succ0(b), fn.succ1(b)})
      if (s != NONE && !reach[s]) {
        reach[s] = 1;
        wl.push_back(s);
      }
  }
  bool any = false;
  for (uint32_t b = 0; b < fn.nblocks(); ++b) {
    if (reach[b])
      continue;
    if (b != 0 && fn.blocks[b].begin == fn.blocks[b].end &&
        fn.succ0(b) == NONE && fn.succ1(b) == NONE)
      continue; // isolated by an earlier round: old news, not a change
    any = true; // genuinely new dead code (or a newly-folded edge)
    break;
  }
  if (any) {
    // compact code of reachable blocks; isolate unreachable ones
    std::vector<Instr> nc{};
    uint32_t gone = 0;
    for (uint32_t b = 0; b < fn.nblocks(); ++b) {
      uint32_t ob = fn.blocks[b].begin, oe = fn.blocks[b].end;
      fn.blocks[b].begin = (uint32_t)nc.size();
      if (reach[b]) {
        for (uint32_t i = ob; i < oe; ++i)
          nc.push_back(fn.code[i]);
      } else {
        if (oe > ob || fn.succ[2 * b] != NONE || fn.succ[2 * b + 1] != NONE)
          ++gone; // only count blocks that actually died THIS round
        fn.succ[2 * b] = NONE; // isolate: no stale edges into analyses
        fn.succ[2 * b + 1] = NONE;
      }
      fn.blocks[b].end = (uint32_t)nc.size();
    }
    fn.code = std::move(nc);
    os.blocks_gone += gone;
  }
  // succ may have changed (Cbr folds) even without dead blocks: always
  // refresh preds, then prune phi args by pred identity
  fn.recompute_preds();
  bool degenerated = false;
  for (uint32_t b = 0; b < fn.nblocks(); ++b) {
    if (fn.preds[b].empty())
      continue;
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k != K::Phi)
        break;
      auto is_live_pred = [&](uint32_t p) {
        for (uint32_t q : fn.preds[b])
          if (q == p)
            return true;
        return false;
      };
      bool l0 = (I.a != NONE) && is_live_pred(I.t);
      bool l1 = (I.b2 != NONE) && is_live_pred(I.f);
      uint32_t keep = l0 ? I.a : (l1 ? I.b2 : NONE);
      if (l0 && l1)
        continue; // both preds live: phi unchanged
      if (I.k != K::Phi)
        continue;         // already degenerated on an earlier visit
      if (keep == NONE) { // no live pred (cannot happen in a live
        I.k = K::Const;
        I.c = 0; // block, but be safe) -> undef, DCE food
        os.folds++;
      } else { // one pred left: phi degenerates to copy
        I.k = K::Copy;
        I.a = keep;
        I.b2 = NONE;
        os.copies++;
      }
      os.phi_degens++;
      degenerated = true;
      I.t = I.f = NONE;
    }
  }
  return any || degenerated;
}

// --- 9b2. block splicing
// ------------------------------------------------------

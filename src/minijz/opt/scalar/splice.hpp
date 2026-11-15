#pragma once
// === splice.hpp ========================================
// block splicing (single-pred/single-succ shortcutting).
//
// ALGORITHM: opt_splice pairs, then rebuilds. Pairing (one sweep over
//   blocks): b is skipped if already in a pair this sweep (matchings
//   only: no chains); s = fn.succ0(b) must be b's only successor, not
//   b itself, not the entry 0, not already paired; it must have
//   fn.preds[s] == {b} exactly and hold no Phi. Then s is absorbed
//   (absorbed[s] = b, merges[b] = s, os.splices++) and phis in s's
//   successors rekey pred ids s -> b (I.t/I.f, fn.preds[t]). Rebuild:
//   absorbed blocks get empty ranges + NONE succs; an absorber emits
//   its instrs minus the trailing Br, follows merges to the chain end
//   whose terminator and succs (from the old_blocks/old_succ
//   snapshots) become b's; fn.recompute_preds() closes.
// DATA STRUCTURES:
//   * absorbed (block -> absorber) + merges (absorber -> absorbed):
//     the pair bookkeeping, one entry per splice.
//   * old_blocks/old_succ: snapshots of ranges and succs -- the rebuild
//     overwrites fn.blocks/fn.succ while chain ends still need the
//     absorbed blocks' ORIGINAL successors.
//   * nc: the rebuilt instr vector, ranges updated per block.
// THEORY: the guard makes the merge a control-flow no-op: b ends in
//   `br s` and s is entered only from b, so s's instrs already run
//   right after b's on every path -- appending them to b and dropping
//   the Br runs the same instrs in the same order. s == b (self-loop)
//   and s == 0 (entry) are kept; s must be phi-free, and its
//   successors' phis are rekeyed s -> b so every phi arg stays
//   pred-identified for the pred-based passes that follow.
// ENABLES: this collapses the jump chains that branch folding leaves
//   behind; merged pairs become longer straight-line blocks so later
//   scans see straight-line runs, with preds recomputed to match.

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../analysis/an.hpp"
#include "../opt_generic.hpp"

// --- 9b2. block splicing
// ------------------------------------------------------ If a single-succ block
// b ends in `br s`, and s has exactly one pred (b) and no phis, s's code is
// appended to b (b's Br dropped). This collapses the jump chains that branch
// folding leaves behind. Phi pred ids in s's successors are rekeyed s -> b so
// phi args stay pred-identified.
constexpr bool opt_splice(Fn &fn, OptStats &os) {
  const uint32_t nb = fn.nblocks();
  // ---- the pairing plan (pure CFG math): matchings only -- b absorbs its
  // single successor s when s is not b, not the entry, not already paired,
  // entered ONLY from b, and phi-free. The kernel keeps its own rekeyed
  // copy of the preds (a later pair's "entered only from b'" check reads
  // the rekeyed rows, exactly like the old inline sweep); the final
  // fn.preds are recomputed from succ below anyway.
  auto U = [](uint32_t v) { return v == NONE ? opt_ns::none_v : (uintlen_t)v; };
  std::vector<std::vector<uintlen_t>> srows(nb), prows(nb);
  std::vector<char> has_phi(nb, 0);
  for (uint32_t b = 0; b < nb; ++b) {
    if (fn.succ0(b) != NONE)
      srows[b].push_back(fn.succ0(b));
    if (fn.succ1(b) != NONE)
      srows[b].push_back(fn.succ1(b));
    for (uint32_t p : fn.preds[b])
      prows[b].push_back(p);
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
      if (fn.code[i].k == K::Phi) {
        has_phi[b] = 1;
        break;
      }
  }
  const opt_ns::splice_plan_t plan =
      opt_ns::calculate_splice_plan(srows, std::move(prows), 0, has_phi);
  bool any = false;
  for (uint32_t b = 0; b < nb; ++b)
    any |= (plan.merges_of[b] != opt_ns::none_v);
  if (!any)
    return false;
  // ---- the rekeys the plan implies: for every absorber pair, the phis of
  // the absorbed block's successors rekey pred s -> pred b (pred identity,
  // never position), in the sweep's own order
  for (uint32_t b = 0; b < nb; ++b) {
    const uintlen_t mm = plan.merges_of[b];
    if (mm == opt_ns::none_v)
      continue;
    const uint32_t s2 = (uint32_t)mm;
    for (uint32_t t : {fn.succ0(s2), fn.succ1(s2)}) {
      if (t == NONE)
        continue;
      for (uint32_t i = fn.blocks[t].begin; i < fn.blocks[t].end; ++i) {
        Instr &I = fn.code[i];
        if (I.k != K::Phi)
          break;
        if (I.t == s2)
          I.t = b;
        if (I.f == s2)
          I.f = b;
      }
    }
    os.splices++;
  }
  // ---- rebuild: absorbed blocks vanish; an absorber emits its own
  // non-terminator instrs, then its absorbed block's instrs, then (that
  // block may itself have absorbed someone in this sweep) the whole chain,
  // ending at the final terminator. Ranges AND succs are read from
  // SNAPSHOTS: the rebuild overwrites fn.blocks/fn.succ while chain ends
  // still need the absorbed blocks' ORIGINAL successors (an absorbed block
  // isolated earlier in this same loop must not poison its absorber with
  // NONE succs).
  std::vector<Block> old_blocks = fn.blocks;
  std::vector<uint32_t> old_succ = fn.succ;
  std::vector<Instr> nc{};
  nc.reserve(fn.code.size());
  for (uint32_t b = 0; b < nb; ++b) {
    if (plan.absorbed_of[b] != opt_ns::none_v) {
      fn.blocks[b] = {(uint32_t)nc.size(), (uint32_t)nc.size()};
      fn.succ[2 * b] = NONE;
      fn.succ[2 * b + 1] = NONE;
      continue;
    }
    uint32_t begin = (uint32_t)nc.size();
    uint32_t x = b;
    for (;;) {
      uint32_t xb = old_blocks[x].begin, xe = old_blocks[x].end;
      const uintlen_t mm = plan.merges_of[x];
      if (mm == opt_ns::none_v) { // chain end: emit all incl. terminator
        for (uint32_t i = xb; i < xe; ++i)
          nc.push_back(fn.code[i]);
        fn.succ[2 * b] = old_succ[2 * x];
        fn.succ[2 * b + 1] = old_succ[2 * x + 1];
        break;
      }
      uint32_t m = (uint32_t)mm;
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

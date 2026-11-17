#pragma once
// === dse.hpp ========================================
// dead-store elimination: the in-window killing-store rules for
// provably-single, provably-in-bounds slots (the LLVM-gap backlog's
// "DSE killing-store chains" item -- ldelim's sibling, running right
// after it: every load ldelim forwarded became a Copy, which is
// window-transparent here, so the windows are as wide as they can
// get).
//
// ALGORITHM (opt_dse -- the client's half of the kernel contract):
//   * the participation gate. A Store or Load takes part only with a
//     single-allocation handle (alias_analyze's AI.single), a KNOWN
//     CONST index (kc/kv), and a known const allocation length L
//     (the Alloc's operand const) with 0 <= index < L: the slot key
//     is then EXACT (a constant by value) and the access cannot
//     trap -- both are load-bearing for the window discipline.
//   * the window policy (what breaks it): anything that may trap
//     (Div/Mod, an access that failed the gate -- its index could be
//     out of bounds, and a MOVED trap reports a different line: the
//     trap text carries kind AND source line), anything observable
//     (Print, Input, Call -- even a 0-arg one can print or hit the
//     depth trap inside -- Ret, Exit, Bkpt, Alloc: "invalid array
//     size" traps), and anything that may write an inexact slot
//     (an obscure-handle store, a runtime-index store) breaks the
//     window. Const/Copy/non-Div-Mod Bin/Phi/Len are transparent:
//     no event, the window holds.
//   * the kernel (opt_ns::dse_ns::calculate_dead_stores) runs the
//     two rules per exact slot: SAME-VALUE (a redundant re-write of
//     the value the slot provably still holds -- the survivor keeps
//     the entry) and OVERWRITE (a fresh store kills the remembered
//     store nobody read since). One rule per pair, never both: the
//     same-value check runs first.
//   * the replay drops the dead stores (dst == NONE: no SSA impact)
//     by rebuilding the code vector, block ranges reallocated
//     cleanly.
//
// DATA STRUCTURES: AI.single + alloc_len[] (the const lengths, -1
// unknown), kc/kv (SSA: every vreg one def, a Const def keys its
// vreg by value), evs[b] (the event rows) + store_at (the store
// events' instruction indices, in emission order -- the kernel's
// dead ordinals pair through it), dead_idx (the drop set).
//
// THEORY: an exact-slot store between window-transparent
// instructions is dead when a later exact store to the SAME slot
// overwrites it unread (nothing between could have trapped, printed,
// or observed the slot -- the window contract), and an exact-slot
// store of the value the slot provably still holds is redundant
// (exactness: no other store could have written the slot; the
// window: no break could have hidden one). Deleting it leaves the
// observable stream -- prints, inputs, traps AT THEIR LINES, and
// every later load's value -- identical.
//
// ENABLES: pure weight off the arena path (mem2reg declines arrays
// whose OTHER accesses are runtime-indexed; this still kills their
// pairwise-redundant const stores), and smaller store pressure for
// the backends.
//
// PIPELINE POSITION. Tier 1, directly after ldelim (the forwarded
// loads became Copies -- transparent here); gated by --disable=dse.

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../analysis/alias.hpp"
#include "../analysis/an.hpp"
#include "../opt_generic.hpp"

// --- 9d5. DEAD-STORE ELIMINATION (the in-window kill rules) ---------------
constexpr bool opt_dse(Fn &fn, const An &A, OptStats &os) {
  if (A.n != fn.nblocks() || !fn.nblocks())
    return false;
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nv)
    return false;
  const AliasInfo AI = alias_analyze(fn);
  // known constants (SSA: one def per vreg -- a Const def keys it)
  std::vector<char> kc(nv, 0);
  std::vector<val_t> kv(nv, 0);
  for (const Instr &I : fn.code)
    if (I.k == K::Const && I.dst != NONE && I.dst < nv) {
      kc[I.dst] = 1;
      kv[I.dst] = I.c;
    }
  // the const allocation lengths (unknown = -1): an Alloc whose
  // length operand is a known const
  std::vector<val_t> alloc_len(AI.nallocs, -1);
  for (const Instr &I : fn.code)
    if (I.k == K::Alloc && I.dst != NONE && I.dst < nv &&
        AI.single[I.dst] != NONE && I.a != NONE && I.a < nv && kc[I.a] &&
        kv[I.a] >= 0)
      alloc_len[AI.single[I.dst]] = kv[I.a];
  // ---- the event rows (the window policy) + the ordinal pairing ---------
  const uint32_t nb = fn.nblocks();
  std::vector<std::vector<opt_ns::dse_event_t>> evs(nb);
  std::vector<uint32_t> ev_at{}; // event ordinal -> instr index (every
                                 // event; dead sites pair through this)
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      // the participation gate: exact, in-bounds, non-trapping access
      const bool is_acc = I.k == K::Store || I.k == K::Load;
      bool part = false;
      if (is_acc && I.a != NONE && I.a < nv && AI.single[I.a] != NONE &&
          I.b2 != NONE && I.b2 < nv && kc[I.b2]) {
        const val_t L = alloc_len[AI.single[I.a]];
        part = L >= 0 && kv[I.b2] >= 0 && kv[I.b2] < L;
      }
      if (part) {
        const opt_ns::dse_event_t ev{
            .kind = (I.k == K::Store) ? opt_ns::dse_store_v : opt_ns::dse_load_v,
            .group = AI.single[I.a],
            .key = (uintlen_t)(int64_t)kv[I.b2],
            .val = (I.k == K::Store && I.c2 != NONE && I.c2 < nv)
                       ? (uintlen_t)I.c2
                       : opt_ns::none_v};
        evs[b].push_back(ev);
        ev_at.push_back(i);
        continue;
      }
      // the window policy: what stays transparent, what breaks it
      const bool transparent =
          I.k == K::Const || I.k == K::Copy || I.k == K::Phi || I.k == K::Len ||
          (I.k == K::Bin && I.b != B::Div && I.b != B::Mod);
      if (!transparent) {
        evs[b].push_back({.kind = opt_ns::dse_break_v});
        ev_at.push_back(i);
      }
    }
  // ---- the kernel -------------------------------------------------------
  const std::vector<opt_ns::dse_dead_t> dead =
      opt_ns::calculate_dead_stores<V>(evs);
  if (dead.empty())
    return false;
  // ---- the replay: drop the dead stores (dst == NONE, no SSA impact) ---
  std::vector<char> drop(fn.code.size(), 0);
  for (const opt_ns::dse_dead_t &d : dead) {
    if (d.site < ev_at.size()) {
      drop[ev_at[d.site]] = 1;
      if (d.rule == opt_ns::dse_overwrite_v)
        os.dse_overwrites++;
      else
        os.dse_sames++;
    }
  }
  std::vector<Instr> nc{};
  nc.reserve(fn.code.size());
  for (uint32_t b = 0; b < nb; ++b) {
    const uint32_t ob = fn.blocks[b].begin, oe = fn.blocks[b].end;
    fn.blocks[b].begin = (uint32_t)nc.size();
    for (uint32_t i = ob; i < oe; ++i)
      if (!drop[i])
        nc.push_back(fn.code[i]);
    fn.blocks[b].end = (uint32_t)nc.size();
  }
  fn.code = std::move(nc);
  return true;
}

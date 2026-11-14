#pragma once
// === mem2reg.hpp ========================================
// mem2reg: array memory -> SSA registers + dead-store/dead-alloc
// elimination for never-read arrays.
//
// DATA STRUCTURES (the promotion machinery's state):
//   AI.single      alias analysis: vreg -> allocation id, NONE when
//                  the handle is OBSCURE (may name >1 array) -- only
//                  single-handle arrays are promotable
//   Cls            per-allocation classification {len, lift, dead}:
//                  lift = constant length <= 256, every Load/Store
//                  single-handle + constant in-range index, loads
//                  exist, and the 2048-slot budget admits it; dead =
//                  never read with no may-trap store (Len -> L later)
//   slot_base[id]  element j of allocation id is SLOT slot_base+j --
//                  slots are the pseudo-variables SSA is built over
//   sdefs[s]       def blocks per slot (the Alloc block for the
//                  zero-init + every Store's block) -> IDF phi
//                  placement, exactly an.hpp's construction
//   init_vreg/init_slot_of  the Const-0 init vreg per slot (created
//                  by the rebuild, ids >= nv) and its reverse map
//   phis_at[b]     slot phis per block (placeholders first at block
//                  starts, .c = slot id until renaming assigns dst)
//
// THE SSA CORE IS THE GENERIC KERNEL (ir/ssa_generic.hpp): placement
// (calculate_phi_placements over sdefs), renaming (rename_ssa over a
// Load/Store/init event stream) and the rebuild's layout math
// (calculate_reflow_plan: dead stores dropped, slot phis prepended,
// inits anchored after their Allocs) all live there, index-space;
// this file keeps only the IR knowledge -- which instrs are slot
// defs/uses, the Alloc/Load/Store/Len rewrites, the classification
// gates. The walk became a table replay: fresh vregs are minted in
// visit_order x program order, the exact sequence the interleaved
// walk produced (byte-identical parity).
//
// PIPELINE POSITION. Runs early (the scalar/loop passes then see
// plain registers, not arena traffic); gated by the toggles
// (lift_ok = the mem2reg toggle, delalloc_ok) and the program-wide
// stkarr_ok gate from prog_handle_gates (deleting an allocation
// shifts every later arena handle -- observable through handle
// VALUES and Len-of-obscure -- so under that gate dead allocs are
// pure weight). Bkpt-observed arrays stay in the arena (the
// debugger must be able to dump their contents). The full slot
// theory (init/store/load as pseudo-variable defs/uses) is the
// 9d4 banner below; the stale-loop-array hazard (the init must be
// pushed at the ALLOCATION SITE, not at entry) is documented at
// the renaming walk.

#include "../base/pass_cfg.hpp"
#include "../ir/instr.hpp"
#include "../ir/ssa_generic.hpp"
#include "analysis/alias.hpp"
#include "analysis/an.hpp"

// ============================================================================
// 9d4. mem2reg: array memory -> SSA registers ("memory to register lifting")
// ============================================================================
// An `array(L)` with constant L whose every access is through a handle that
// provably names ONLY that allocation, with a constant in-range index, does
// not need memory at all: slot j of the array becomes a pseudo-variable,
//     init   : L x `Const 0` right after the Alloc (arena arrays are
//              zero-filled; the Alloc itself is KEPT so the arena bump,
//              handle values, lengths and OOM traps stay bit-identical)
//     store  : a versioned def (a Copy) of the slot's pseudo-variable
//     load   : a use of the currently reaching version
// with phis placed at the iterated dominance frontier of each slot's def
// blocks and renaming by dominator-tree walk -- full SSA, so every later
// pass (constprop, CSE, LICM, closed forms...) sees plain registers.
// An array that is never READ (no Load at all) is dead regardless of index
// shape: all its Stores are deleted ("if the array is not read we just
// delete writes, cuz why not") and Len(h) folds to the constant L.
constexpr bool opt_mem2reg(Fn &fn, const An &A, OptStats &os, bool lift_ok,
                           bool delalloc_ok) {
  const uint32_t nb = fn.nblocks();
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nb || !nv)
    return false;
  AliasInfo AI = alias_analyze(fn);
  if (!AI.nallocs)
    return false;
  // ---- dead allocations --------------------------------------------------
  // With every Load/Store/Len of an array gone (lifted here, dead-store
  // deleted here, or never present at all) nothing reads the allocation
  // anymore. Deleting it shifts every LATER handle, which is observable
  // only through handle VALUES (print/compare/pass/stored) or a Len through
  // an obscure handle -- exactly what the program-wide stkarr_ok gate
  // excludes -- so under that gate an allocation whose handle chain is
  // referenced by nothing but its own Copy/Phi plumbing is pure dead weight
  // and dies here (DCE then cleans the plumbing). This is what removes the
  // `int64_t mjz_sN[len]; memset(...)` pair for arrays that folding has
  // already turned into constants.
  auto sweep_dead_allocs = [&]() -> bool {
    if (!delalloc_ok)
      return false;
    std::vector<char> used(AI.nallocs, 0);
    for (uint32_t b = 0; b < fn.nblocks(); ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        uint32_t dst_id =
            (I.dst != NONE && I.dst < nv) ? AI.single[I.dst] : NONE;
        auto use = [&](uint32_t v) {
          if (v == NONE || v >= nv)
            return;
          uint32_t id = AI.single[v];
          if (id != NONE && id != dst_id)
            used[id] = 1;
        };
        use(I.a);
        use(I.b2);
        use(I.c2);
        for (uint32_t v : I.args)
          use(v);
      }
    bool any = false;
    for (uint32_t id = 0; id < AI.nallocs; ++id)
      any |= used[id] == 0;
    if (!any)
      return false;
    // the rebuild as a keep-only reflow plan (instruction removal is
    // pure index math in the kernel; the scan predicates stay here)
    const uint32_t nb2 = fn.nblocks();
    const uint32_t n0 = (uint32_t)fn.code.size();
    std::vector<ulen> ob(nb2), oe(nb2), keep(n0, 1), head(nb2, 0), after(n0, 0);
    for (uint32_t b = 0; b < nb2; ++b) {
      ob[b] = fn.blocks[b].begin;
      oe[b] = fn.blocks[b].end;
    }
    for (uint32_t b = 0; b < nb2; ++b)
      for (uint32_t i = ob[b]; i < oe[b]; ++i) {
        const Instr &I = fn.code[i];
        if (I.k == K::Alloc && I.dst != NONE && I.dst < nv) {
          uint32_t id = AI.single[I.dst];
          if (id != NONE && !used[id]) {
            keep[i] = 0;
            os.deadallocs++;
          }
        }
      }
    const ssa_ns::reflow_plan_t<V> plan =
        ssa_ns::calculate_reflow_plan<V>(ob, oe, keep, head, after);
    std::vector<Instr> nc{};
    nc.reserve(plan.total);
    for (uint32_t b = 0; b < nb2; ++b)
      for (uint32_t i = ob[b]; i < oe[b]; ++i)
        if (keep[i])
          nc.push_back(fn.code[i]);
    for (uint32_t b = 0; b < nb2; ++b)
      fn.blocks[b] = {(uint32_t)plan.new_begin[b], (uint32_t)plan.new_end[b]};
    fn.code = std::move(nc);
    return true;
  };
  if (!lift_ok)
    return sweep_dead_allocs();
  std::vector<uint32_t> ndefs(nv, 0);
  for (const Instr &I : fn.code)
    if (I.dst != NONE && I.dst < nv)
      ++ndefs[I.dst];
  std::vector<char> kc(nv, 0);
  std::vector<val_t> kv(nv, 0);
  for (const Instr &I : fn.code)
    if (I.k == K::Const && I.dst != NONE && I.dst < nv && ndefs[I.dst] == 1) {
      kc[I.dst] = 1;
      kv[I.dst] = I.c;
    }
  constexpr val_t MJZ_MEM2REG_MAX_ELEMS = 256;
  constexpr uint32_t MJZ_MEM2REG_MAX_SLOTS = 2048;
  // ---- classify allocations -----------------------------------------------
  // liftable: constant length, every access single-handle + const index
  //           in range (loads exist)          -> slot machinery
  // dead:     constant length, NO loads, and every store provably unable to
  //           trap (constant in-range index, or `nt` from the range
  //           analysis) -> drop the stores; Len -> L; keep the Alloc (an
  //           earlier arena slot must not shift)
  struct Cls {
    uint32_t id{};
    val_t len{};
    bool lift{};
    bool dead{};
  };
  std::vector<Cls> cls(AI.nallocs, {0, -1, false, false});
  {
    std::vector<char> has_load(AI.nallocs, 0), has_store(AI.nallocs, 0),
        bad(AI.nallocs, 0), bad_dead(AI.nallocs, 0);
    std::vector<uint32_t> alloc_instr(AI.nallocs, NONE);
    for (uint32_t b = 0; b < nb; ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        uint32_t id = (I.a != NONE && I.a < nv) ? AI.single[I.a] : NONE;
        if (I.k == K::Alloc && I.dst != NONE && I.dst < nv &&
            AI.single[I.dst] != NONE) {
          if (id == NONE)
            id = AI.single[I.dst];
          if (I.a != NONE && I.a < nv && kc[I.a] && kv[I.a] >= 0) {
            cls[id].len = kv[I.a];
            alloc_instr[id] = i;
          } else {
            cls[id].len = -1; // runtime length: not classifiable
          }
          continue;
        }
        if (I.k == K::Bkpt) { // observed handle: the array must STAY in the
          for (uint32_t v : I.args) { // arena so the debugger can dump its
            uint32_t oid =
                (v != NONE && v < nv) ? AI.single[v] : NONE; // contents
            if (oid != NONE) {
              bad[oid] = 1;      // no lifting to vregs
              bad_dead[oid] = 1; // and no dead-store sweep either
            }
          }
          continue;
        }
        if (id == NONE)
          continue;
        if (I.k == K::Load || I.k == K::Store) {
          bool idx_ok = I.b2 != NONE && I.b2 < nv && kc[I.b2] &&
                        kv[I.b2] >= 0 && kv[I.b2] < cls[id].len;
          if (!idx_ok)
            bad[id] = 1; // slot machinery needs compile-time elems
          if (I.k == K::Load) {
            has_load[id] = 1;
          } else {
            has_store[id] = 1;
            if (!idx_ok && !I.nt)
              bad_dead[id] = 1; // a may-trap store must stay (trap parity)
          }
        }
      }
    uint32_t budget = MJZ_MEM2REG_MAX_SLOTS;
    for (uint32_t id = 0; id < AI.nallocs; ++id) {
      if (cls[id].len < 0 || alloc_instr[id] == NONE)
        continue;
      if (has_load[id] && !bad[id] && cls[id].len <= MJZ_MEM2REG_MAX_ELEMS &&
          budget >= (uint32_t)cls[id].len) {
        cls[id].lift = true;
        budget -= (uint32_t)cls[id].len;
      } else if (!has_load[id] && !bad_dead[id]) {
        cls[id].dead = true; // never read: stores are pure dead weight
      }
    }
  }
  bool any_lift = false, any_dead = false;
  for (const Cls &c : cls) {
    any_lift |= c.lift;
    any_dead |= c.dead;
  }
  if (!any_lift && !any_dead)
    return sweep_dead_allocs();
  // ---- defensive pre-check (before ANY mutation): every access of a ------
  // lifted array must be dominated by the allocation's block, and ordered
  // after the allocation when both share a block. SSA's "def dominates use"
  // makes this a theorem for the handle's uses; verify it anyway so the
  // renaming walk below can never encounter an empty slot stack.
  {
    std::vector<uint32_t> alloc_idx(AI.nallocs, NONE),
        alloc_blk(AI.nallocs, NONE);
    for (uint32_t b = 0; b < nb; ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k == K::Alloc && I.dst != NONE && I.dst < nv &&
            AI.single[I.dst] != NONE) {
          alloc_idx[AI.single[I.dst]] = i;
          alloc_blk[AI.single[I.dst]] = b;
        }
      }
    for (uint32_t b = 0; b < nb; ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k != K::Load && I.k != K::Store)
          continue;
        uint32_t h = (I.a != NONE && I.a < nv) ? AI.single[I.a] : NONE;
        if (h == NONE || !cls[h].lift)
          continue;
        uint32_t ab = alloc_blk[h];
        if (ab == NONE || !A.dominates(ab, b) || (ab == b && i < alloc_idx[h]))
          cls[h].lift = false;
      }
    any_lift = false;
    for (const Cls &c : cls)
      any_lift |= c.lift;
    if (!any_lift && !any_dead)
      return sweep_dead_allocs();
  }
  // ---- lifted arrays: slot bookkeeping --------------------------------------
  uint32_t total_slots = 0;
  for (const Cls &c : cls)
    if (c.lift)
      total_slots += (uint32_t)c.len;
  // slot id = slot_base[id] + element index
  std::vector<uint32_t> slot_base(AI.nallocs, NONE);
  {
    uint32_t next = 0;
    for (uint32_t id = 0; id < AI.nallocs; ++id)
      if (cls[id].lift) {
        slot_base[id] = next;
        next += (uint32_t)cls[id].len;
      }
  }
  // def blocks per slot: the Alloc block (init) + each Store's block
  std::vector<std::vector<ulen>> sdefs(total_slots);
  {
    std::vector<uint32_t> alloc_block(AI.nallocs, NONE);
    for (uint32_t b = 0; b < nb; ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k == K::Alloc && I.dst < nv && AI.single[I.dst] != NONE)
          alloc_block[AI.single[I.dst]] = b;
      }
    for (uint32_t id = 0; id < AI.nallocs; ++id)
      if (cls[id].lift)
        for (val_t j = 0; j < cls[id].len; ++j)
          sdefs[slot_base[id] + (uint32_t)j].push_back(alloc_block[id]);
    for (uint32_t b = 0; b < nb; ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k == K::Store && I.a < nv && AI.single[I.a] != NONE &&
            cls[AI.single[I.a]].lift)
          sdefs[slot_base[AI.single[I.a]] + (uint32_t)kv[I.b2]].push_back(b);
      }
  }
  // ---- phi placement at each slot's IDF (the generic kernel; the
  // pred rows are POSITIONAL with fn.preds' order -- the phi entries'
  // edge identity -- and the succ rows keep SLOT order for seeding)
  std::vector<std::vector<ulen>> prs(nb), scs(nb);
  for (uint32_t b = 0; b < nb; ++b) {
    for (uint32_t p : fn.preds[b])
      prs[b].push_back(p);
    if (fn.succ0(b) != NONE)
      scs[b].push_back(fn.succ0(b));
    if (fn.succ1(b) != NONE)
      scs[b].push_back(fn.succ1(b));
  }
  const auto pred_rows = gns::make_basic_forest<V>(prs);
  const auto succ_rows = gns::make_basic_forest<V>(scs);
  const gns::treversal_result_t<V> placement =
      ssa_ns::calculate_phi_placements<V>(A.df, pred_rows, sdefs);
  auto phis_at = placement.range();
  std::vector<uint32_t> phi_ord_base(nb + 1, 0);
  for (uint32_t b = 0; b < nb; ++b)
    phi_ord_base[b + 1] = phi_ord_base[b] + (uint32_t)phis_at[b].size();
  const uint32_t total_phis = phi_ord_base[nb];
  // ---- rebuild: slot phis at block starts, init consts after Allocs,
  //      drop dead-array stores -- ONE reflow plan (heads = the slot
  //      phis, keeps = the dead-store mask, afters = the init counts)
  std::vector<uint32_t> init_vreg(total_slots, NONE);
  {
    const uint32_t n0 = (uint32_t)fn.code.size();
    std::vector<ulen> ob(nb), oe(nb), keep(n0, 1), head(nb, 0), after(n0, 0);
    for (uint32_t b = 0; b < nb; ++b) {
      ob[b] = fn.blocks[b].begin;
      oe[b] = fn.blocks[b].end;
      head[b] = (uint32_t)phis_at[b].size();
    }
    for (uint32_t b = 0; b < nb; ++b)
      for (uint32_t i = ob[b]; i < oe[b]; ++i) {
        const Instr &I = fn.code[i];
        if (I.k == K::Store && I.a < nv && AI.single[I.a] != NONE &&
            cls[AI.single[I.a]].dead) {
          keep[i] = 0; // never read: the store is pure dead weight
          os.mem2reg_dses++;
        } else if (I.k == K::Alloc && I.dst < nv && AI.single[I.dst] != NONE &&
                   cls[AI.single[I.dst]].lift)
          after[i] = (uint32_t)cls[AI.single[I.dst]].len;
      }
    const ssa_ns::reflow_plan_t<V> plan =
        ssa_ns::calculate_reflow_plan<V>(ob, oe, keep, head, after);
    std::vector<Instr> nc{};
    nc.reserve(plan.total + 16);
    for (uint32_t b = 0; b < nb; ++b) {
      for (ulen s : phis_at[b]) // slot phis go FIRST (before var phis)
        nc.push_back({.k = K::Phi, .c = (int64_t)s});
      for (uint32_t i = ob[b]; i < oe[b]; ++i) {
        if (!keep[i])
          continue;
        nc.push_back(fn.code[i]);
        if (after[i]) { // the promotion's zero-init, right after the Alloc
          uint32_t base = slot_base[AI.single[fn.code[i].dst]];
          for (val_t j = 0; j < (val_t)after[i]; ++j) {
            uint32_t v = new_anon_vreg(fn, 0);
            init_vreg[base + (uint32_t)j] = v;
            nc.push_back({.k = K::Const, .dst = v, .c = 0});
          }
          os.mem2reg_lifts++;
        }
      }
    }
    for (uint32_t b = 0; b < nb; ++b)
      fn.blocks[b] = {(uint32_t)plan.new_begin[b], (uint32_t)plan.new_end[b]};
    fn.code = std::move(nc);
  }
  // ---- renaming: the generic kernel over a Load/Store/init event
  // stream, then the table replay ------------------------------------------
  // (SSA guarantees every access of a lifted array is dominated by the
  // allocation's block -- the handle's def dominates its uses -- so the
  // init version is always on the stack when an access is rewritten.)
  // init_vreg -> slot lookup. NOTE: the init vregs were created by the
  // rebuild (new_anon_vreg), so their ids are >= nv; the map is sized to
  // cover them. The init is a def event AT THE ALLOCATION SITE in the
  // stream, NOT at function entry -- an array allocated inside a loop is
  // re-zeroed every iteration, and reads before the allocation never
  // happen (the dominance pre-check guarantees it).
  // (Entry-pushing instead lets a loop-carried slot phi SHADOW the init and
  // leak the previous iteration's values -- the stale-loop-array bug.)
  std::vector<uint32_t> init_slot_of(nv + total_slots, NONE);
  for (uint32_t s = 0; s < total_slots; ++s)
    if (init_vreg[s] != NONE && init_vreg[s] < nv + total_slots)
      init_slot_of[init_vreg[s]] = s;
  // the event stream (program order): a zero-init is a DEF whose vreg
  // the rebuild already minted (pre-materialized); a lifted Load is a
  // USE of its slot; a lifted Store is a DEF (its fresh vreg is minted
  // at the replay). Site ids are dense in this enumeration; the gather
  // and the apply re-derive them by the same rule.
  std::vector<std::vector<ssa_ns::ssa_event_t>> evs(nb);
  std::vector<uint32_t> site_begin(nb, 0), site_init_vreg{};
  site_init_vreg.reserve(64);
  {
    uint32_t site = 0;
    for (uint32_t b = 0; b < nb; ++b) {
      site_begin[b] = site;
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k == K::Phi)
          continue; // slot-phi placeholders: kernel-synthesized defs
        if (I.k == K::Const && I.dst != NONE && I.dst < init_slot_of.size() &&
            init_slot_of[I.dst] != NONE) {
          evs[b].push_back({.kind = ssa_ns::ssa_def_v,
                            .var = init_slot_of[I.dst],
                            .site = site});
          site_init_vreg.push_back(init_vreg[init_slot_of[I.dst]]);
          ++site;
        } else if (I.k == K::Load) {
          uint32_t h = (I.a != NONE && I.a < nv) ? AI.single[I.a] : NONE;
          if (h != NONE && cls[h].lift) {
            evs[b].push_back({.kind = ssa_ns::ssa_use_v,
                              .var = slot_base[h] + (uint32_t)kv[I.b2],
                              .site = site});
            site_init_vreg.push_back(NONE);
            ++site;
          }
        } else if (I.k == K::Store) {
          uint32_t h = (I.a != NONE && I.a < nv) ? AI.single[I.a] : NONE;
          if (h != NONE && cls[h].lift) {
            evs[b].push_back({.kind = ssa_ns::ssa_def_v,
                              .var = slot_base[h] + (uint32_t)kv[I.b2],
                              .site = site});
            site_init_vreg.push_back(NONE);
            ++site;
          }
        }
      }
    }
  }
  const uint32_t n_sites = (uint32_t)site_init_vreg.size();
  const ssa_ns::ssa_renaming_t<V> R = ssa_ns::rename_ssa<V>(
      total_slots, n_sites, A.dom_tree, succ_rows, pred_rows, placement, evs);
  // defensive: a lifted Load with no reaching def (cannot happen -- the
  // dominance pre-check above guarantees non-empty stacks)
  {
    std::vector<char> visited(nb, 0);
    for (ulen b : R.visit_order)
      visited[(uint32_t)b] = 1;
    for (uint32_t b = 0; b < nb; ++b) {
      if (!visited[b])
        continue;
      for (const ssa_ns::ssa_event_t &ev : evs[b])
        if (ev.kind == ssa_ns::ssa_use_v &&
            R.reaching_def[ev.site] == ssa_ns::ssa_none_v)
          return false; // same state as before: the rebuild stands,
                        // nothing else mutated
    }
  }
  // replay: mint the fresh vregs in the walk's order (slot-phi dsts at
  // block heads, then the Store defs in program order; inits are
  // pre-materialized) -- the exact new_anon_vreg sequence of the old
  // interleaved walk, so vreg ids stay byte-identical.
  {
    std::vector<uint32_t> site_vreg(n_sites + total_phis, NONE);
    for (uint32_t s = 0; s < n_sites; ++s)
      if (site_init_vreg[s] != NONE)
        site_vreg[s] = site_init_vreg[s];
    for (ulen bb : R.visit_order) {
      const uint32_t b = (uint32_t)bb;
      for (uint32_t pi = 0; pi < (uint32_t)phis_at[b].size(); ++pi) {
        uint32_t v = new_anon_vreg(fn, 0);
        site_vreg[n_sites + phi_ord_base[b] + pi] = v;
      }
      for (const ssa_ns::ssa_event_t &ev : evs[b])
        if (ev.kind == ssa_ns::ssa_def_v && site_vreg[ev.site] == NONE) {
          uint32_t v = new_anon_vreg(fn, 0);
          site_vreg[ev.site] = v;
        }
    }
    // apply: the rewrites, only in walked (reachable) blocks -- the
    // unreachable ones keep their raw arena traffic for unreach to
    // delete, exactly as the old walk left them
    for (ulen bb : R.visit_order) {
      const uint32_t b = (uint32_t)bb;
      uint32_t pi = 0;
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        Instr &I = fn.code[i];
        if (I.k != K::Phi)
          break;
        if (pi < (uint32_t)phis_at[b].size()) { // slot phis precede var phis
          const uint32_t phi = phi_ord_base[b] + pi;
          const uint32_t e0 = R.phi_entry_begin[phi];
          I.dst = site_vreg[R.phi_def_site[phi]];
          for (uint32_t e = e0; e < R.phi_entry_begin[phi + 1]; ++e) {
            if (R.phi_entry_pred[e] == ssa_ns::ssa_none_v)
              continue; // never seeded (unreachable pred / dup slot)
            const uint32_t arg = R.phi_entry_def[e] == ssa_ns::ssa_none_v
                                     ? NONE
                                     : site_vreg[R.phi_entry_def[e]];
            if (e == e0) {
              I.a = arg;
              I.t = (uint32_t)R.phi_entry_pred[e];
            } else {
              I.b2 = arg;
              I.f = (uint32_t)R.phi_entry_pred[e];
            }
          }
        }
        ++pi;
      }
      uint32_t site = site_begin[b];
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        Instr &I = fn.code[i];
        if (I.k == K::Phi)
          continue;
        if (I.k == K::Const && I.dst != NONE && I.dst < init_slot_of.size() &&
            init_slot_of[I.dst] != NONE) {
          ++site; // the promotion's zero-init: already materialized
        } else if (I.k == K::Load) {
          uint32_t h = (I.a != NONE && I.a < nv) ? AI.single[I.a] : NONE;
          if (h != NONE && cls[h].lift) {
            const uint32_t rd = (uint32_t)R.reaching_def[site];
            I.k = K::Copy;
            I.a = rd == ssa_ns::ssa_none_v ? NONE : site_vreg[rd];
            I.b2 = NONE;
            ++site;
          }
        } else if (I.k == K::Store) {
          uint32_t h = (I.a != NONE && I.a < nv) ? AI.single[I.a] : NONE;
          if (h != NONE && cls[h].lift) {
            I.k = K::Copy;
            I.dst = site_vreg[site];
            I.a = I.c2; // the stored value
            I.b2 = NONE;
            I.c2 = NONE;
            ++site;
          }
        } else if (I.k == K::Len) {
          uint32_t h = (I.a != NONE && I.a < nv) ? AI.single[I.a] : NONE;
          if (h != NONE && (cls[h].lift || cls[h].dead)) {
            I.k = K::Const;
            I.c = cls[h].len;
            I.a = NONE;
          }
        }
      }
    }
  }
  (void)sweep_dead_allocs(); // post-rewrite: lifted handles are now unused
  return true;
}

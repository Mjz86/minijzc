#pragma once
// === mem2reg_impl.hpp ========================================
// the MEM2REG SUPPORT layer, split out of mem2reg.hpp (the
// code-quality continuation, round seven -- the cdfold/lcf/unroll/
// fold/unswitch_bif/taildup treatment): the pass context (the alias
// tables, the classification, the slot machinery and the renaming
// state the former monolith held in locals) and one small function
// per phase. The slim driver -- the phase sequence -- stays in
// mem2reg.hpp. The SSA core itself is NOT here: placement, renaming
// and the rebuild's layout math are the generic index-space kernel
// (ir/ssa_generic.hpp, mjz::ssa_ns); this layer is the IR knowledge
// -- which instrs are slot defs/uses, the Alloc/Load/Store/Len
// rewrites, the classification gates -- gathered around it.
//
// ALGORITHM (the phase list; every order below is the original's):
//   scan    the pass-entry guards: blocks and a vreg universe exist,
//           the alias analysis finds at least one allocation.
//           AI.single is the single-handle map (vreg -> allocation
//           id, NONE when the handle is OBSCURE -- may name >1
//           array); only single-handle arrays are promotable.
//   sweep   THE DEAD-ALLOC REMOVAL: an allocation whose handle chain
//           no one but its own Copy/Phi plumbing references is pure
//           weight under the program-wide stkarr_ok gate (deleting
//           it shifts every later arena handle -- observable through
//           handle VALUES and Len-of-obscure, exactly what that gate
//           excludes). A keep-only reflow plan; runs on three exits
//           (lift disabled, nothing classifiable, post-rewrite).
//   keys    the sole-def Const table: kc[v]/kv[v] hold v's value iff
//           v is a Const with exactly one def -- the compile-time
//           index source. Everything downstream reads indices
//           through this gate.
//   classify per allocation: lift (constant length <= 256, every
//           access single-handle + constant in-range index, loads
//           exist, the 2048-slot budget admits it), dead (no loads
//           and every store provably unable to trap), or neither.
//           Bkpt-observed arrays are neither (the debugger must be
//           able to dump arena contents).
//   domchk  the defensive dominance pre-check: every access of a
//           lifted array is dominated by the allocation's block, and
//           ordered after the allocation when both share a block.
//           SSA's def-dominates-use makes this a theorem; it is
//           verified anyway so the renaming walk can never meet an
//           empty slot stack (a demoted candidate falls back to
//           sweep).
//   slots   the pseudo-variables: slot slot_base[id]+j for element j
//           of lifted allocation id; sdefs[s] the def blocks per
//           slot (the Alloc block for the zero-init + every Store's
//           block) -- exactly an.hpp's phi-placement construction.
//   place   the kernel's phi placement over sdefs (the IR's policy
//           wrapper: <2 def blocks skip, >=2 pred-slot rule), the
//           positional pred rows fn.preds' order carries the phi
//           entries' edge identity, phi_ord_base the per-block
//           ordinal offsets.
//   rebuild ONE reflow plan: slot phis prepended at block starts
//           (.c = slot id until renaming assigns dst), dead-array
//           stores dropped, L zero-init Consts anchored after each
//           lifted Alloc (their fresh vregs minted HERE, in emission
//           order -- the rebuild's own new_anon_vreg sequence).
//   gather  the event stream the kernel renames over: a zero-init is
//           a DEF whose vreg the rebuild already minted
//           (pre-materialized), a lifted Load a USE, a lifted Store
//           a DEF (its fresh vreg is minted at the replay). Site
//           ids dense in this enumeration; site_begin/per-block
//           streams re-derive them by the same rule.
//   rename  the kernel's dominator walk, then the defensive visited
//           check: a lifted Load with no reaching def cannot happen
//           (domchk guarantees non-empty stacks) -- if it ever did,
//           the phase reports failure and the driver returns false
//           with the rebuild standing (the documented asymmetry:
//           the rebuild is already applied, nothing else mutates).
//   replay  mint the remaining fresh vregs (slot-phi dsts at block
//           heads, then Store defs) in visit_order x program order
//           -- the EXACT new_anon_vreg sequence of the old
//           interleaved walk, the allocation-order contract -- then
//           apply: phi args from the kernel's (pred, incoming)
//           entries, Load -> Copy of the reaching def, Store ->
//           Copy with the stored value, Len -> the constant L.
//
// DATA STRUCTURES (the pass context, M2RCtx):
//   AI              the alias analysis (single-handle map, nallocs)
//   kc/kv           vreg -> sole-def Const flag / value (the keys)
//   cls             per-allocation classification (len, lift, dead)
//   total_slots     the pseudo-variable universe
//   slot_base       allocation id -> its first slot id
//   sdefs           slot -> def blocks (phi placement's input)
//   placement       the kernel's block-major slot-phi rows
//   pred/succ_rows  the CFG CSR pair (positional preds, slot succs)
//   phi_ord_base    block -> phi ordinal offset (the replay's index)
//   init_vreg       slot -> the Const-0 init vreg (rebuild-minted)
//   init_slot_of    the reverse map (vreg -> slot; ids >= nv)
//   evs             per-block event streams (the kernel's input)
//   site_begin      block -> its first site id
//   site_init_vreg  site -> pre-materialized init vreg / NONE
//   R               the kernel's renaming tables (visit_order,
//                   reaching_def, the phi records)
//
// THEORY (the 9d4 slot construction, and why it is safe):
//   * AN ARRAY BECOMES L PSEUDO-VARIABLES. init: L x Const 0 right
//   after the Alloc (arena arrays are zero-filled; the Alloc itself
//   is KEPT so the arena bump, handle values, lengths and OOM traps
//   stay bit-identical). store: a versioned def (a Copy). load: a
//   use of the currently reaching version. Phis at each slot's IDF,
//   renaming by dominator walk -- full SSA, so every later pass
//   (constprop, CSE, LICM, closed forms...) sees plain registers.
//   * THE INIT IS A DEF AT THE ALLOCATION SITE, NOT AT ENTRY. An
//   array allocated inside a loop is re-zeroed every iteration;
//   reads before the allocation never happen (domchk guarantees
//   it). Entry-pushing instead lets a loop-carried slot phi SHADOW
//   the init and leak the previous iteration's values -- the
//   stale-loop-array bug, pinned by the audit.
//   * DEAD ARRAYS PAY NO TRAP. A never-read array's stores die only
//   when every one of them provably cannot trap (constant
//   in-range index, or `nt' from the range analysis); a may-trap
//   store stays for trap parity. Len(h) folds to the constant L
//   either way (the length never depended on the stores).
//   * BUDGET. At most 2048 slots per function: the reach bitset of
//   the transitive reduction is words-per-node, and the sweep is
//   held to a size class, not a promise.
//
// Byte-identical contract (what a refactor must NOT perturb):
//   * every scan bound is the pass-entry nb/nv: the rebuild's own
//   minted vregs (ids >= nv) are invisible to the use scans, and
//   the final sweep re-reads fn.nblocks()/fn.code.size() fresh (the
//   original's expressions -- no blocks are added by this pass, but
//   the expressions are the original's).
//   * the ALLOCATION ORDER is observable (the register-audit table
//   keys on vreg ids): the rebuild mints the init vregs per block,
//   per kept instr, element j ascending; the replay mints per
//   visit_order block the slot-phi dsts then the Store defs in
//   program order. Reordering any mint changes the audit table.
//   * the classify if-chain ORDER is behavior: an Alloc first sets
//   id from I.a then falls back to AI.single[I.dst]; a Bkpt marks
//   BOTH bad flags; the Load/Store case reads cls[id].len BEFORE
//   the lift branch could have... (no: classification is a single
//   pass, len is set by the Alloc visit -- the access rows read
//   the len the Alloc row left, so an Alloc AFTER its access in
//   block order still classifies the same, the scans are order-
//   independent tables).
//   * the sweep's use scan EXCLUDES the instr's own dst (an Alloc
//   reading its own handle is plumbing, not a use); the dst_id is
//   computed per instr BEFORE the operand walk (the original's
//   expression).
//   * the rebuild's Alloc test omits the `I.dst != NONE' guard the
//   other tests carry (I.dst < nv alone): NONE is ~0u, the original
//   relied on that, the impl keeps the same expression.
//   * rename's defensive failure returns false WITHOUT the final
//   sweep: the rebuild stands, nothing else mutated -- the
//   original's documented asymmetry.
#include "../base/pass_cfg.hpp"
#include "../ir/instr.hpp"
#include "../ir/ssa_generic.hpp"
#include "analysis/alias.hpp"
#include "analysis/an.hpp"

// the promotion's size class: at most 256 elements per lifted array,
// 2048 slots per function (the reach bitset and the sweep's budget)
inline constexpr val_t MJZ_MEM2REG_MAX_ELEMS = 256;
inline constexpr uint32_t MJZ_MEM2REG_MAX_SLOTS = 2048;

// the per-allocation classification (liftable / dead / neither)
struct m2r_cls_t {
  uint32_t id{};
  val_t len{};
  bool lift{};
  bool dead{};
};

// ---- the pass context (the former monolith's locals) ----------------------
struct M2RCtx {
  Fn &fn;      // the function under rewrite
  const An &A; // the caller's fresh analysis (df, dom_tree, dominates)
  bool lift_ok{}, delalloc_ok{};     // the pass toggles
  uint32_t nb{}, nv{};               // pass-entry block / vreg universe
  AliasInfo AI{};                    // the single-handle alias analysis
  std::vector<char> kc{};            // vreg -> sole-def Const (the index key)
  std::vector<val_t> kv{};           // ... and its value
  std::vector<m2r_cls_t> cls{};      // per-allocation classification
  bool any_lift{}, any_dead{};       // the classification summary
  uint32_t total_slots{};            // the pseudo-variable universe
  std::vector<uint32_t> slot_base{}; // alloc id -> first slot
  std::vector<std::vector<ulen>> sdefs{}; // slot -> def blocks
  gns::treversal_result_t<V> placement{}; // block-major slot phis
  gns::treversal_result_t<V> pred_rows{}; // the CFG CSR pair
  gns::treversal_result_t<V> succ_rows{}; // (positional/slot order)
  std::vector<uint32_t> phi_ord_base{};   // block -> phi ordinal
  uint32_t total_phis{};
  std::vector<uint32_t> init_vreg{};    // slot -> the Const-0 vreg
  std::vector<uint32_t> init_slot_of{}; // vreg -> slot (reverse map)
  std::vector<std::vector<ssa_ns::ssa_event_t>> evs{}; // the streams
  std::vector<uint32_t> site_begin{};                  // block -> first site id
  std::vector<uint32_t> site_init_vreg{}; // site -> pre-minted vreg
  uint32_t n_sites{};
  ssa_ns::ssa_renaming_t<V> R{}; // the kernel's renaming tables
};

// ---- scan: the pass-entry guards ------------------------------------------
constexpr bool m2r_scan(Fn &fn, M2RCtx &X) {
  X.nb = fn.nblocks();
  X.nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!X.nb || !X.nv)
    return false;
  X.AI = alias_analyze(fn);
  return X.AI.nallocs != 0;
}

// ---- sweep: the dead-allocation removal -----------------------------------
// With every Load/Store/Len of an array gone (lifted here, dead-store
// deleted here, or never present at all) nothing reads the allocation
// anymore. Deleting it shifts every LATER handle, which is observable
// only through handle VALUES (print/compare/pass/stored) or a Len
// through an obscure handle -- exactly what the program-wide stkarr_ok
// gate excludes -- so under that gate an allocation whose handle chain
// is referenced by nothing but its own Copy/Phi plumbing is pure dead
// weight and dies here (DCE then cleans the plumbing). This is what
// removes the `int64_t mjz_sN[len]; memset(...)` pair for arrays that
// folding has already turned into constants.
constexpr bool m2r_sweep_dead_allocs(Fn &fn, M2RCtx &X, OptStats &os) {
  const AliasInfo &AI = X.AI;
  const uint32_t nv = X.nv;
  if (!X.delalloc_ok)
    return false;
  std::vector<char> used(AI.nallocs, 0);
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      uint32_t dst_id = (I.dst != NONE && I.dst < nv) ? AI.single[I.dst] : NONE;
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
}

// ---- keys: the sole-def Const table ---------------------------------------
constexpr void m2r_key_consts(Fn &fn, M2RCtx &X) {
  const uint32_t nv = X.nv;
  seed_single_def_consts(fn, nv, X.kc, X.kv);
}

// ---- classify: liftable / dead / neither ----------------------------------
// liftable: constant length, every access single-handle + const index
//           in range (loads exist)          -> slot machinery
// dead:     constant length, NO loads, and every store provably unable to
//           trap (constant in-range index, or `nt' from the range
//           analysis) -> drop the stores; Len -> L; keep the Alloc (an
//           earlier arena slot must not shift)
constexpr void m2r_classify(Fn &fn, M2RCtx &X) {
  const AliasInfo &AI = X.AI;
  const uint32_t nb = X.nb, nv = X.nv;
  X.cls.assign(AI.nallocs, m2r_cls_t{0, -1, false, false});
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
          if (I.a != NONE && I.a < nv && X.kc[I.a] && X.kv[I.a] >= 0) {
            X.cls[id].len = X.kv[I.a];
            alloc_instr[id] = i;
          } else {
            X.cls[id].len = -1; // runtime length: not classifiable
          }
          continue;
        }
        if (I.k == K::Bkpt) {         // observed handle: the array must STAY in
          for (uint32_t v : I.args) { // the arena so the debugger can dump
            uint32_t oid =            // its contents
                (v != NONE && v < nv) ? AI.single[v] : NONE;
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
          bool idx_ok = I.b2 != NONE && I.b2 < nv && X.kc[I.b2] &&
                        X.kv[I.b2] >= 0 && X.kv[I.b2] < X.cls[id].len;
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
      if (X.cls[id].len < 0 || alloc_instr[id] == NONE)
        continue;
      if (has_load[id] && !bad[id] && X.cls[id].len <= MJZ_MEM2REG_MAX_ELEMS &&
          budget >= (uint32_t)X.cls[id].len) {
        X.cls[id].lift = true;
        budget -= (uint32_t)X.cls[id].len;
      } else if (!has_load[id] && !bad_dead[id]) {
        X.cls[id].dead = true; // never read: stores are pure dead weight
      }
    }
  }
  X.any_lift = false;
  X.any_dead = false;
  for (const m2r_cls_t &c : X.cls) {
    X.any_lift |= c.lift;
    X.any_dead |= c.dead;
  }
}

// ---- domchk: the defensive dominance pre-check -----------------------------
// Every access of a lifted array must be dominated by the allocation's
// block, and ordered after the allocation when both share a block. SSA's
// "def dominates use" makes this a theorem for the handle's uses; verify
// it anyway so the renaming walk below can never encounter an empty slot
// stack. A demoted candidate is dropped here (any_lift recomputed).
constexpr void m2r_check_dominance(Fn &fn, M2RCtx &X) {
  const AliasInfo &AI = X.AI;
  const uint32_t nb = X.nb, nv = X.nv;
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
      if (h == NONE || !X.cls[h].lift)
        continue;
      uint32_t ab = alloc_blk[h];
      if (ab == NONE || !X.A.dominates(ab, b) || (ab == b && i < alloc_idx[h]))
        X.cls[h].lift = false;
    }
  X.any_lift = false;
  for (const m2r_cls_t &c : X.cls)
    X.any_lift |= c.lift;
}

// ---- slots: the pseudo-variable tables ------------------------------------
constexpr void m2r_slot_tables(Fn &fn, M2RCtx &X) {
  const AliasInfo &AI = X.AI;
  const uint32_t nb = X.nb, nv = X.nv;
  X.total_slots = 0;
  for (const m2r_cls_t &c : X.cls)
    if (c.lift)
      X.total_slots += (uint32_t)c.len;
  // slot id = slot_base[id] + element index
  X.slot_base.assign(AI.nallocs, NONE);
  {
    uint32_t next = 0;
    for (uint32_t id = 0; id < AI.nallocs; ++id)
      if (X.cls[id].lift) {
        X.slot_base[id] = next;
        next += (uint32_t)X.cls[id].len;
      }
  }
  // def blocks per slot: the Alloc block (init) + each Store's block
  X.sdefs.assign(X.total_slots, {});
  {
    std::vector<uint32_t> alloc_block(AI.nallocs, NONE);
    for (uint32_t b = 0; b < nb; ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k == K::Alloc && I.dst < nv && AI.single[I.dst] != NONE)
          alloc_block[AI.single[I.dst]] = b;
      }
    for (uint32_t id = 0; id < AI.nallocs; ++id)
      if (X.cls[id].lift)
        for (val_t j = 0; j < X.cls[id].len; ++j)
          X.sdefs[X.slot_base[id] + (uint32_t)j].push_back(alloc_block[id]);
    for (uint32_t b = 0; b < nb; ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k == K::Store && I.a < nv && AI.single[I.a] != NONE &&
            X.cls[AI.single[I.a]].lift)
          X.sdefs[X.slot_base[AI.single[I.a]] + (uint32_t)X.kv[I.b2]].push_back(
              b);
      }
  }
}

// ---- place: the kernel's phi placement -------------------------------------
// The pred rows are POSITIONAL with fn.preds' order -- the phi entries'
// edge identity -- and the succ rows keep SLOT order for seeding.
constexpr void m2r_place(Fn &fn, M2RCtx &X) {
  const uint32_t nb = X.nb;
  std::vector<std::vector<ulen>> prs(nb), scs(nb);
  for (uint32_t b = 0; b < nb; ++b) {
    for (uint32_t p : fn.preds[b])
      prs[b].push_back(p);
    if (fn.succ0(b) != NONE)
      scs[b].push_back(fn.succ0(b));
    if (fn.succ1(b) != NONE)
      scs[b].push_back(fn.succ1(b));
  }
  X.pred_rows = gns::make_basic_forest<V>(prs);
  X.succ_rows = gns::make_basic_forest<V>(scs);
  X.placement =
      ssa_ns::calculate_phi_placements<V>(X.A.df, X.pred_rows, X.sdefs);
  auto phis_at = X.placement.range();
  X.phi_ord_base.assign(nb + 1, 0);
  for (uint32_t b = 0; b < nb; ++b)
    X.phi_ord_base[b + 1] = X.phi_ord_base[b] + (uint32_t)phis_at[b].size();
  X.total_phis = X.phi_ord_base[nb];
}

// ---- rebuild: slot phis at block starts, init consts after Allocs, ---------
// ---- dead-array stores dropped -- ONE reflow plan ---------------------------
constexpr void m2r_rebuild(Fn &fn, M2RCtx &X, OptStats &os) {
  const AliasInfo &AI = X.AI;
  const uint32_t nb = X.nb, nv = X.nv;
  X.init_vreg.assign(X.total_slots, NONE);
  auto phis_at = X.placement.range();
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
          X.cls[AI.single[I.a]].dead) {
        keep[i] = 0; // never read: the store is pure dead weight
        os.mem2reg_dses++;
      } else if (I.k == K::Alloc && I.dst < nv && AI.single[I.dst] != NONE &&
                 X.cls[AI.single[I.dst]].lift)
        after[i] = (uint32_t)X.cls[AI.single[I.dst]].len;
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
        uint32_t base = X.slot_base[AI.single[fn.code[i].dst]];
        for (val_t j = 0; j < (val_t)after[i]; ++j) {
          uint32_t v = new_anon_vreg(fn, 0);
          X.init_vreg[base + (uint32_t)j] = v;
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

// ---- gather: the event stream the kernel renames over ----------------------
// init_vreg -> slot lookup. NOTE: the init vregs were created by the
// rebuild (new_anon_vreg), so their ids are >= nv; the map is sized to
// cover them. The init is a def event AT THE ALLOCATION SITE in the
// stream, NOT at function entry -- an array allocated inside a loop is
// re-zeroed every iteration, and reads before the allocation never
// happen (the dominance pre-check guarantees it).
// (Entry-pushing instead lets a loop-carried slot phi SHADOW the init and
// leak the previous iteration's values -- the stale-loop-array bug.)
constexpr void m2r_gather_events(Fn &fn, M2RCtx &X) {
  const AliasInfo &AI = X.AI;
  const uint32_t nb = X.nb, nv = X.nv;
  X.init_slot_of.assign(nv + X.total_slots, NONE);
  for (uint32_t s = 0; s < X.total_slots; ++s)
    if (X.init_vreg[s] != NONE && X.init_vreg[s] < nv + X.total_slots)
      X.init_slot_of[X.init_vreg[s]] = s;
  // the event stream (program order): a zero-init is a DEF whose vreg
  // the rebuild already minted (pre-materialized); a lifted Load is a
  // USE of its slot; a lifted Store is a DEF (its fresh vreg is minted
  // at the replay). Site ids are dense in this enumeration; the gather
  // and the apply re-derive them by the same rule.
  X.evs.assign(nb, {});
  X.site_begin.assign(nb, 0);
  X.site_init_vreg.clear();
  X.site_init_vreg.reserve(64);
  {
    uint32_t site = 0;
    for (uint32_t b = 0; b < nb; ++b) {
      X.site_begin[b] = site;
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k == K::Phi)
          continue; // slot-phi placeholders: kernel-synthesized defs
        if (I.k == K::Const && I.dst != NONE && I.dst < X.init_slot_of.size() &&
            X.init_slot_of[I.dst] != NONE) {
          X.evs[b].push_back({.kind = ssa_ns::ssa_def_v,
                              .var = X.init_slot_of[I.dst],
                              .site = site});
          X.site_init_vreg.push_back(X.init_vreg[X.init_slot_of[I.dst]]);
          ++site;
        } else if (I.k == K::Load) {
          uint32_t h = (I.a != NONE && I.a < nv) ? AI.single[I.a] : NONE;
          if (h != NONE && X.cls[h].lift) {
            X.evs[b].push_back({.kind = ssa_ns::ssa_use_v,
                                .var = X.slot_base[h] + (uint32_t)X.kv[I.b2],
                                .site = site});
            X.site_init_vreg.push_back(NONE);
            ++site;
          }
        } else if (I.k == K::Store) {
          uint32_t h = (I.a != NONE && I.a < nv) ? AI.single[I.a] : NONE;
          if (h != NONE && X.cls[h].lift) {
            X.evs[b].push_back({.kind = ssa_ns::ssa_def_v,
                                .var = X.slot_base[h] + (uint32_t)X.kv[I.b2],
                                .site = site});
            X.site_init_vreg.push_back(NONE);
            ++site;
          }
        }
      }
    }
  }
  X.n_sites = (uint32_t)X.site_init_vreg.size();
}

// ---- rename: the kernel's walk + the defensive visited check ---------------
constexpr bool m2r_rename(M2RCtx &X) {
  const uint32_t nb = X.nb;
  X.R = ssa_ns::rename_ssa<V>(X.total_slots, X.n_sites, X.A.dom_tree,
                              X.succ_rows, X.pred_rows, X.placement, X.evs);
  // defensive: a lifted Load with no reaching def (cannot happen -- the
  // dominance pre-check above guarantees non-empty stacks)
  {
    std::vector<char> visited(nb, 0);
    for (ulen b : X.R.visit_order)
      visited[(uint32_t)b] = 1;
    for (uint32_t b = 0; b < nb; ++b) {
      if (!visited[b])
        continue;
      for (const ssa_ns::ssa_event_t &ev : X.evs[b])
        if (ev.kind == ssa_ns::ssa_use_v &&
            X.R.reaching_def[ev.site] == ssa_ns::ssa_none_v)
          return false; // same state as before: the rebuild stands,
                        // nothing else mutated
    }
  }
  return true;
}

// ---- replay: mint the fresh vregs in the walk's order, then apply ----------
constexpr void m2r_replay(Fn &fn, M2RCtx &X) {
  const AliasInfo &AI = X.AI;
  const uint32_t nv = X.nv;
  auto phis_at = X.placement.range();
  const uint32_t n_sites = X.n_sites;
  // mint the fresh vregs in the walk's order (slot-phi dsts at block
  // heads, then the Store defs in program order; inits are
  // pre-materialized) -- the exact new_anon_vreg sequence of the old
  // interleaved walk, so vreg ids stay byte-identical.
  {
    std::vector<uint32_t> site_vreg(n_sites + X.total_phis, NONE);
    for (uint32_t s = 0; s < n_sites; ++s)
      if (X.site_init_vreg[s] != NONE)
        site_vreg[s] = X.site_init_vreg[s];
    for (ulen bb : X.R.visit_order) {
      const uint32_t b = (uint32_t)bb;
      for (uint32_t pi = 0; pi < (uint32_t)phis_at[b].size(); ++pi) {
        uint32_t v = new_anon_vreg(fn, 0);
        site_vreg[n_sites + X.phi_ord_base[b] + pi] = v;
      }
      for (const ssa_ns::ssa_event_t &ev : X.evs[b])
        if (ev.kind == ssa_ns::ssa_def_v && site_vreg[ev.site] == NONE) {
          uint32_t v = new_anon_vreg(fn, 0);
          site_vreg[ev.site] = v;
        }
    }
    // apply: the rewrites, only in walked (reachable) blocks -- the
    // unreachable ones keep their raw arena traffic for unreach to
    // delete, exactly as the old walk left them
    for (ulen bb : X.R.visit_order) {
      const uint32_t b = (uint32_t)bb;
      uint32_t pi = 0;
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        Instr &I = fn.code[i];
        if (I.k != K::Phi)
          break;
        if (pi < (uint32_t)phis_at[b].size()) { // slot phis precede var phis
          const uint32_t phi = X.phi_ord_base[b] + pi;
          const uint32_t e0 = X.R.phi_entry_begin[phi];
          I.dst = site_vreg[X.R.phi_def_site[phi]];
          for (uint32_t e = e0; e < X.R.phi_entry_begin[phi + 1]; ++e) {
            if (X.R.phi_entry_pred[e] == ssa_ns::ssa_none_v)
              continue; // never seeded (unreachable pred / dup slot)
            const uint32_t arg = X.R.phi_entry_def[e] == ssa_ns::ssa_none_v
                                     ? NONE
                                     : site_vreg[X.R.phi_entry_def[e]];
            if (e == e0) {
              I.a = arg;
              I.t = (uint32_t)X.R.phi_entry_pred[e];
            } else {
              I.b2 = arg;
              I.f = (uint32_t)X.R.phi_entry_pred[e];
            }
          }
        }
        ++pi;
      }
      uint32_t site = X.site_begin[b];
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        Instr &I = fn.code[i];
        if (I.k == K::Phi)
          continue;
        if (I.k == K::Const && I.dst != NONE && I.dst < X.init_slot_of.size() &&
            X.init_slot_of[I.dst] != NONE) {
          ++site; // the promotion's zero-init: already materialized
        } else if (I.k == K::Load) {
          uint32_t h = (I.a != NONE && I.a < nv) ? AI.single[I.a] : NONE;
          if (h != NONE && X.cls[h].lift) {
            const auto rd = X.R.reaching_def[site];
            I.k = K::Copy;
            I.a = rd == ssa_ns::ssa_none_v ? NONE : site_vreg[uint32_t(rd)];
            I.b2 = NONE;
            ++site;
          }
        } else if (I.k == K::Store) {
          uint32_t h = (I.a != NONE && I.a < nv) ? AI.single[I.a] : NONE;
          if (h != NONE && X.cls[h].lift) {
            I.k = K::Copy;
            I.dst = site_vreg[site];
            I.a = I.c2; // the stored value
            I.b2 = NONE;
            I.c2 = NONE;
            ++site;
          }
        } else if (I.k == K::Len) {
          uint32_t h = (I.a != NONE && I.a < nv) ? AI.single[I.a] : NONE;
          if (h != NONE && (X.cls[h].lift || X.cls[h].dead)) {
            I.k = K::Const;
            I.c = X.cls[h].len;
            I.a = NONE;
          }
        }
      }
    }
  }
}

#pragma once
// === mem2reg.hpp ========================================
// mem2reg: array memory -> SSA registers + dead-store/dead-alloc
// elimination for never-read arrays.

#include "minijz/base/pass_cfg.hpp"
#include "minijz/ir/instr.hpp"
#include "minijz/opt/analysis/an.hpp"
#include "minijz/opt/analysis/alias.hpp"

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
  if (!nb || !nv) return false;
  AliasInfo AI = alias_analyze(fn);
  if (!AI.nallocs) return false;
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
    if (!delalloc_ok) return false;
    std::vector<char> used(AI.nallocs, 0);
    for (uint32_t b = 0; b < fn.nblocks(); ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        uint32_t dst_id =
            (I.dst != NONE && I.dst < nv) ? AI.single[I.dst] : NONE;
        auto use = [&](uint32_t v) {
          if (v == NONE || v >= nv) return;
          uint32_t id = AI.single[v];
          if (id != NONE && id != dst_id) used[id] = 1;
        };
        use(I.a);
        use(I.b2);
        use(I.c2);
        for (uint32_t v : I.args) use(v);
      }
    bool any = false;
    for (uint32_t id = 0; id < AI.nallocs; ++id) any |= used[id] == 0;
    if (!any) return false;
    std::vector<Instr> nc{};
    nc.reserve(fn.code.size());
    for (uint32_t b = 0; b < fn.nblocks(); ++b) {
      uint32_t old_begin = fn.blocks[b].begin;
      fn.blocks[b].begin = (uint32_t)nc.size();
      for (uint32_t i = old_begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k == K::Alloc && I.dst != NONE && I.dst < nv) {
          uint32_t id = AI.single[I.dst];
          if (id != NONE && !used[id]) {
            os.deadallocs++;
            continue;
          }
        }
        nc.push_back(I);
      }
      fn.blocks[b].end = (uint32_t)nc.size();
    }
    fn.code = std::move(nc);
    return true;
  };
  if (!lift_ok) return sweep_dead_allocs();
  std::vector<uint32_t> ndefs(nv, 0);
  for (const Instr &I : fn.code)
    if (I.dst != NONE && I.dst < nv) ++ndefs[I.dst];
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
          if (id == NONE) id = AI.single[I.dst];
          if (I.a != NONE && I.a < nv && kc[I.a] && kv[I.a] >= 0) {
            cls[id].len = kv[I.a];
            alloc_instr[id] = i;
          } else {
            cls[id].len = -1; // runtime length: not classifiable
          }
          continue;
        }
        if (id == NONE) continue;
        if (I.k == K::Load || I.k == K::Store) {
          bool idx_ok = I.b2 != NONE && I.b2 < nv && kc[I.b2] &&
                        kv[I.b2] >= 0 && kv[I.b2] < cls[id].len;
          if (!idx_ok) bad[id] = 1; // slot machinery needs compile-time elems
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
      if (cls[id].len < 0 || alloc_instr[id] == NONE) continue;
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
  if (!any_lift && !any_dead) return sweep_dead_allocs();
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
        if (I.k != K::Load && I.k != K::Store) continue;
        uint32_t h = (I.a != NONE && I.a < nv) ? AI.single[I.a] : NONE;
        if (h == NONE || !cls[h].lift) continue;
        uint32_t ab = alloc_blk[h];
        if (ab == NONE || !A.dominates(ab, b) ||
            (ab == b && i < alloc_idx[h]))
          cls[h].lift = false;
      }
    any_lift = false;
    for (const Cls &c : cls) any_lift |= c.lift;
    if (!any_lift && !any_dead) return sweep_dead_allocs();
  }
  // ---- lifted arrays: slot bookkeeping --------------------------------------
  uint32_t total_slots = 0;
  for (const Cls &c : cls)
    if (c.lift) total_slots += (uint32_t)c.len;
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
  // ---- phi placement at each slot's IDF -------------------------------------
  std::vector<std::vector<uint32_t>> phis_at(nb);
  for (uint32_t s = 0; s < total_slots; ++s) {
    if (sdefs[s].size() < 2) continue;
    std::vector<ulen> idf =
        gns::calculate_iterated_dominance_frontiers_given_dominance_frontiers<
            V>(A.df, sdefs[s]);
    for (ulen blk : idf) {
      uint32_t b = (uint32_t)blk;
      if (b >= nb || fn.preds[b].size() < 2) continue;
      bool dup = false;
      for (uint32_t x : phis_at[b]) dup |= (x == s);
      if (!dup) phis_at[b].push_back(s);
    }
  }
  // ---- rebuild: slot phis at block starts, init consts after Allocs,
  //      drop dead-array stores -----------------------------------------------
  std::vector<uint32_t> init_vreg(total_slots, NONE);
  std::vector<char> drop(fn.code.size(), 0);
  {
    std::vector<Instr> nc{};
    nc.reserve(fn.code.size() + total_slots + 16);
    for (uint32_t b = 0; b < nb; ++b) {
      uint32_t old_begin = fn.blocks[b].begin;
      fn.blocks[b].begin = (uint32_t)nc.size();
      for (uint32_t s : phis_at[b]) // slot phis go FIRST (before var phis)
        nc.push_back({.k = K::Phi, .c = (int64_t)s});
      for (uint32_t i = old_begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k == K::Store && I.a < nv && AI.single[I.a] != NONE &&
            cls[AI.single[I.a]].dead) {
          drop[i] = 1;
          os.mem2reg_dses++;
          continue;
        }
        nc.push_back(I);
        if (I.k == K::Alloc && I.dst < nv && AI.single[I.dst] != NONE &&
            cls[AI.single[I.dst]].lift) {
          uint32_t base = slot_base[AI.single[I.dst]];
          for (val_t j = 0; j < cls[AI.single[I.dst]].len; ++j) {
            uint32_t v = new_anon_vreg(fn, 0);
            init_vreg[base + (uint32_t)j] = v;
            nc.push_back({.k = K::Const, .dst = v, .c = 0});
          }
          os.mem2reg_lifts++;
        }
      }
      fn.blocks[b].end = (uint32_t)nc.size();
    }
    fn.code = std::move(nc);
  }
  // ---- renaming: dominator-tree walk with per-slot version stacks ----------
  // (SSA guarantees every access of a lifted array is dominated by the
  // allocation's block -- the handle's def dominates its uses -- so the
  // init version is always on the stack when an access is rewritten.)
  // init_vreg -> slot lookup. NOTE: the init vregs were created by the
  // rebuild (new_anon_vreg), so their ids are >= nv; the map is sized to
  // cover them. The init is pushed onto the version stack WHEN THE WALK
  // REACHES IT (at the allocation site), NOT at function entry -- an array
  // allocated inside a loop is re-zeroed every iteration, and reads before
  // the allocation never happen (the dominance pre-check guarantees it).
  // (Entry-pushing instead lets a loop-carried slot phi SHADOW the init and
  // leak the previous iteration's values -- the stale-loop-array bug.)
  std::vector<uint32_t> init_slot_of(nv + total_slots, NONE);
  for (uint32_t s = 0; s < total_slots; ++s)
    if (init_vreg[s] != NONE && init_vreg[s] < nv + total_slots)
      init_slot_of[init_vreg[s]] = s;
  {
    std::vector<std::vector<uint32_t>> stacks(total_slots);
    struct Frame {
      uint32_t block, child_idx;
      std::vector<uint32_t> pushed; // slots this block pushed (pop on exit)
    };
    std::vector<Frame> wstack;
    // visit: rewrite this block's accesses; seed slot-phi args in successors
    auto visit = [&](uint32_t b, std::vector<uint32_t> &pushed) {
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        Instr &I = fn.code[i];
        if (I.k == K::Phi) {
          if (I.dst == NONE) { // slot-phi placeholder: assign its result
            uint32_t s = (uint32_t)I.c;
            I.dst = new_anon_vreg(fn, 0);
            stacks[s].push_back(I.dst);
            pushed.push_back(s);
          }
          continue; // variable phis: args are plain vreg references
        }
        if (I.k == K::Const && I.dst != NONE &&
            I.dst < init_slot_of.size() && init_slot_of[I.dst] != NONE) {
          // the promotion's zero-init
          uint32_t s2 = init_slot_of[I.dst];
          stacks[s2].push_back(I.dst);
          pushed.push_back(s2);
          continue;
        }
        uint32_t h = (I.a != NONE && I.a < nv) ? AI.single[I.a] : NONE;
        if (I.k == K::Load && h != NONE && cls[h].lift) {
          uint32_t s = slot_base[h] + (uint32_t)kv[I.b2];
          if (stacks[s].empty()) return false; // cannot happen (dominance)
          I.k = K::Copy;
          I.a = stacks[s].back();
          I.b2 = NONE;
        } else if (I.k == K::Store && h != NONE && cls[h].lift) {
          uint32_t s = slot_base[h] + (uint32_t)kv[I.b2];
          uint32_t v = new_anon_vreg(fn, 0);
          I.k = K::Copy;
          I.dst = v;
          I.a = I.c2; // the stored value
          I.b2 = NONE;
          I.c2 = NONE;
          stacks[s].push_back(v);
          pushed.push_back(s);
        } else if (I.k == K::Len && h != NONE &&
                   (cls[h].lift || cls[h].dead)) {
          I.k = K::Const;
          I.c = cls[h].len;
          I.a = NONE;
        }
      }
      // seed slot-phi args in successors: the value at the END of this
      // block reaches the successor along this edge. Slot phis are exactly
      // the first phis_at[s].size() phi instrs of s (insertion order).
      for (uint32_t s2 : {fn.succ0(b), fn.succ1(b)}) {
        if (s2 == NONE || s2 >= nb) continue;
        uint32_t k = 0;
        for (uint32_t i = fn.blocks[s2].begin; i < fn.blocks[s2].end; ++i) {
          Instr &I = fn.code[i];
          if (I.k != K::Phi) break;
          if (k < phis_at[s2].size()) {
            uint32_t s = phis_at[s2][k];
            uint32_t slot = NONE; // b's position among s2's preds
            for (uint32_t j = 0; j < fn.preds[s2].size(); ++j)
              if (fn.preds[s2][j] == b) slot = j;
            if (slot == NONE) return false;
            uint32_t arg = stacks[s].empty() ? NONE : stacks[s].back();
            if (slot == 0) { I.a = arg; I.t = b; }
            else           { I.b2 = arg; I.f = b; }
          }
          ++k;
        }
      }
      return true;
    };
    std::vector<uint32_t> pu{};
    if (!visit(0, pu)) return false; // defensive: refuse (nothing mutated
                                     // by visit at that point on failure)
    wstack.push_back({0, 0, std::move(pu)});
    while (!wstack.empty()) {
      Frame &fr = wstack.back();
      auto ch = A.dom_tree.range()[fr.block];
      if (fr.child_idx < (uint32_t)ch.size()) {
        uint32_t next = (uint32_t)ch[fr.child_idx++];
        std::vector<uint32_t> pu2{};
        visit(next, pu2);
        wstack.push_back({next, 0, std::move(pu2)});
      } else {
        for (auto it = fr.pushed.rbegin(); it != fr.pushed.rend(); ++it)
          stacks[*it].pop_back();
        wstack.pop_back();
      }
    }
  }
  (void)sweep_dead_allocs(); // post-rewrite: lifted handles are now unused
  return true;
}

#pragma once
// === destruct.hpp ========================================
// SSA destruction: split critical edges, materialize phis as copies,
// fuse copy chains.

#include "minijz/base/pass_cfg.hpp"
#include "minijz/opt/analysis/an.hpp"

// ============================================================================
// 9g. SSA DESTRUCTION — phis -> edge copies
//     1. split critical edges (pred with 2 succs -> block with phis): the
//        edge copies need a block of their own on that edge
//     2. for every pred edge into a phi block, lower the phis to a PARALLEL
//        copy set, then sequentialize it: greedily emit copies whose dst is
//        not a source of any pending copy; a leftover set is a swap cycle,
//        broken by saving one source into a fresh temp (lost-copy fix)
//     3. strip the phis; the result is plain (non-SSA) 3-address code where
//        every vreg is just a slot
// ============================================================================
struct DestrStats {
  uint32_t edges_split{};  // critical edges split
  uint32_t copies{};       // sequentialized edge copies emitted
  uint32_t zeros{};        // undef phi args materialized as Const 0
  uint32_t self_copies{};  // d<-d no-ops skipped
  uint32_t cycles_broken{};// swap cycles broken with a temp
  uint32_t fused{};        // post-destruction copies folded into their source
};

// sequentialize one parallel copy set; appends real instrs to `out`
constexpr void sequentialize(
    Fn &fn, std::vector<std::pair<uint32_t, uint32_t>> &pcs,
    std::vector<Instr> &out, DestrStats &ds) {
  // self-copies are no-ops (the slot already holds the right value on that
  // edge) and would otherwise look like unbreakable 1-cycles
  for (size_t i = 0; i < pcs.size();) {
    if (pcs[i].first == pcs[i].second && pcs[i].second != NONE) {
      pcs.erase(pcs.begin() + (long)i);
      ds.self_copies++;
    } else {
      ++i;
    }
  }
  while (!pcs.empty()) {
    bool progress = false;
    for (size_t i = 0; i < pcs.size(); ++i) {
      uint32_t d = pcs[i].first;
      bool d_needed = false; // is d still a source of a pending copy?
      for (const auto &p : pcs)
        if (p.second == d) { d_needed = true; break; }
      if (d_needed) continue; // emitting now would clobber a needed value
      if (pcs[i].second == NONE) { // undef arg: materialize 0
        out.push_back({.k = K::Const, .dst = d, .c = 0});
        ds.zeros++;
      } else {
        out.push_back({.k = K::Copy, .dst = d, .a = pcs[i].second});
        ds.copies++;
      }
      pcs.erase(pcs.begin() + (long)i);
      progress = true;
      break;
    }
    if (!progress && !pcs.empty()) {
      // swap cycle: save the source into a fresh temp, redirect this copy
      uint32_t d = pcs[0].first, s = pcs[0].second;
      uint32_t t = new_anon_vreg(fn, fn.base_of(d));
      out.push_back({.k = K::Copy, .dst = t, .a = s});
      pcs[0].second = t;
      ds.copies++;
      ds.cycles_broken++;
    }
  }
}

constexpr void split_critical_edges(Fn &fn, DestrStats &ds) {
  uint32_t n0 = fn.nblocks();
  for (uint32_t p = 0; p < n0; ++p) {
    if (fn.succ1(p) == NONE || fn.succ0(p) == fn.succ1(p)) continue;
    for (uint32_t si = 0; si < 2; ++si) {
      uint32_t s = si ? fn.succ1(p) : fn.succ0(p);
      if (s == NONE) continue;
      bool s_has_phi = false;
      for (uint32_t i = fn.blocks[s].begin; i < fn.blocks[s].end; ++i)
        if (fn.code[i].k == K::Phi) { s_has_phi = true; break; }
      if (!s_has_phi) continue; // no copies to place: no split needed
      // fresh block m = [br s] on the edge p -> s
      uint32_t m = fn.nblocks();
      fn.blocks.push_back({});
      fn.succ.push_back(s);
      fn.succ.push_back(NONE);
      fn.blocks[m].begin = (uint32_t)fn.code.size();
      fn.code.push_back({.k = K::Br, .t = s});
      fn.blocks[m].end = (uint32_t)fn.code.size();
      // retarget p's edge s -> m (succ table + terminator instr)
      if (si == 0) fn.succ[2 * p] = m;
      else fn.succ[2 * p + 1] = m;
      for (uint32_t i = fn.blocks[p].begin; i < fn.blocks[p].end; ++i) {
        Instr &I = fn.code[i];
        if (I.k == K::Cbr) {
          if (si == 0 && I.t == s) I.t = m;
          if (si == 1 && I.f == s) I.f = m;
        } else if (I.k == K::Br && I.t == s) {
          I.t = m;
        }
      }
      // rekey phis in s: the arg formerly keyed by pred p now arrives via m
      for (uint32_t i = fn.blocks[s].begin; i < fn.blocks[s].end; ++i) {
        Instr &I = fn.code[i];
        if (I.k != K::Phi) break;
        if (I.t == p) I.t = m;
        if (I.f == p) I.f = m;
      }
      ds.edges_split++;
    }
  }
  fn.recompute_preds();
}

constexpr void destroy_ssa(Fn &fn, DestrStats &ds) {
  split_critical_edges(fn, ds);
  // lower phis to edge copies + strip them, rebuilding the code vector.
  // after the split, every pred of a phi block is single-succ, so its edge
  // copies can sit directly before its terminator.
  // NOTE: ranges are read from a snapshot — the rebuild overwrites
  // fn.blocks[p] while later preds still read phi ranges by OLD coordinates.
  std::vector<Block> old_blocks = fn.blocks;
  std::vector<Instr> nc{};
  nc.reserve(fn.code.size() + 16);
  for (uint32_t p = 0; p < fn.nblocks(); ++p) {
    uint32_t ob = old_blocks[p].begin, oe = old_blocks[p].end;
    uint32_t nbegin = (uint32_t)nc.size();
    if (oe > ob) {
      for (uint32_t i = ob; i + 1 < oe; ++i) // all but the terminator
        if (fn.code[i].k != K::Phi) nc.push_back(fn.code[i]);
      const Instr term = fn.code[oe - 1];
      uint32_t s = fn.succ0(p);
      if (fn.succ1(p) == NONE && s != NONE) {
        std::vector<std::pair<uint32_t, uint32_t>> pcs{};
        for (uint32_t i = old_blocks[s].begin; i < old_blocks[s].end; ++i) {
          const Instr &I = fn.code[i];
          if (I.k != K::Phi) break;
          uint32_t arg = NONE;
          if (I.t == p && I.a != NONE) arg = I.a;
          else if (I.f == p && I.b2 != NONE) arg = I.b2;
          pcs.push_back({I.dst, arg}); // arg NONE -> undef -> zero
        }
        if (!pcs.empty()) sequentialize(fn, pcs, nc, ds);
      }
      nc.push_back(term);
    }
    fn.blocks[p] = {nbegin, (uint32_t)nc.size()};
  }
  fn.code = std::move(nc);
  fn.recompute_preds();
}

// --- 9g2. post-destruction copy fusion ----------------------------------------
// Destruction leaves `t = <expr>; d = t;` pairs (the phi-edge copy of a
// freshly computed temp). When t has exactly ONE reader (that copy), the
// defining instruction's destination is renamed to d and the copy dies.
// The defining op stays exactly where it was, so traps, arena reads and
// stdin consumption do not move; d's only other writers are the *other*
// phi-edge copies, which execute later on their own paths, and d's readers
// are all dominated by the join block — so the early write is never observed
// except on the path the copy served. Copy chains collapse the same way.
// This is the IR-level half of "make the backend input non-redundant"; the
// emission plan below does the rest (immediates, branch fusion).
constexpr void post_destruct_peephole(Fn &fn, DestrStats &ds) {
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  for (int round = 0; round < 256; ++round) {
    if (fn.code.empty()) return;
    std::vector<uint32_t> uses(nv, 0);
    std::vector<uint32_t> def_idx(nv, NONE);
    for (uint32_t i = 0; i < fn.code.size(); ++i) {
      const Instr &I = fn.code[i];
      if (I.a != NONE && I.a < nv) uses[I.a]++;
      if (I.b2 != NONE && I.b2 < nv) uses[I.b2]++;
      if (I.c2 != NONE && I.c2 < nv) uses[I.c2]++;
      for (uint32_t v : I.args)
        if (v < nv) uses[v]++;
      if (I.dst != NONE && I.dst < nv) def_idx[I.dst] = i;
    }
    bool changed = false;
    std::vector<char> drop(fn.code.size(), 0);
    for (uint32_t b = 0; b < fn.nblocks() && !changed; ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end && !changed;
           ++i) {
        Instr &I = fn.code[i];
        if (I.k != K::Copy || I.dst == NONE || I.a == NONE || I.a == I.dst)
          continue;
        if (I.a >= nv || uses[I.a] != 1) continue; // src read only here
        uint32_t d = def_idx[I.a];
        if (d == NONE || d == i || drop[d]) continue;
        if (d > i)
          continue; // def BELOW the copy. Post-destruction vregs are plain
        // slots, so a later write to the same slot can sit below this copy
        // -- the classic case is a cycle-broken parallel-copy block: the
        // temp's copy `T = x` comes BEFORE the aliased copy `x = y`. Both
        // fusion forms below assume def-precedes-copy; with d > i the
        // between-writes / between-reads scans iterate an empty range and
        // pass vacuously, and the rewiring would drop a write the
        // parallel-copy semantics still need (the swap collapses to
        // identity and a rotation slot keeps its entry value forever).
        // Reject outright.
        Instr &D = fn.code[d];
        if (D.dst != I.a) continue;
        bool same_block =
            (d >= fn.blocks[b].begin && d < fn.blocks[b].end);
        if (D.k == K::Copy && D.a != NONE && D.a != I.dst && same_block) {
          // copy chain: d2 <- s <- t : this copy reads t directly, s dies.
          // t is single-def when it is a temp; when it is a phi slot its
          // other edge copies live in other blocks, so within THIS block a
          // write to t between D and I is the parallel-copy clobber that
          // cycle breaking exists to prevent (t<-a; a<-b; b<-t) — reject it.
          bool between_writes_src = false;
          for (uint32_t j = d + 1; j < i; ++j)
            if (fn.code[j].dst == D.a) {
              between_writes_src = true;
              break;
            }
          if (between_writes_src) continue;
          I.a = D.a;
          drop[d] = 1;
        } else if (same_block && D.k != K::Br && D.k != K::Cbr &&
                   D.k != K::Print && D.k != K::Exit && D.k != K::Store) {
          // rename the def's destination to the copy's dst and drop the copy.
          // SOUND ONLY within one block with no reader of dst in between:
          // the write moves earlier (from the copy up to the def), and phi
          // slots are read anywhere the header dominates — e.g. a loop body
          // between `t = i+1` and the back-edge copy `i = t` still reads the
          // OLD i. Same-block + no intermediate read of dst rules that out.
          bool between_reads_dst = false;
          for (uint32_t j = d + 1; j < i; ++j) {
            const Instr &M = fn.code[j];
            bool reads_dst =
                M.a == I.dst || M.b2 == I.dst || M.c2 == I.dst;
            for (uint32_t av : M.args) reads_dst |= (av == I.dst);
            if (reads_dst) {
              between_reads_dst = true;
              break;
            }
          }
          if (between_reads_dst) continue;
          D.dst = I.dst;
          drop[i] = 1;
        } else
          continue;
        ds.fused++;
        changed = true;
      }
    if (!changed) return;
    // sweep the one dropped instr, preserving block ranges
    std::vector<Instr> nc{};
    nc.reserve(fn.code.size());
    for (uint32_t b = 0; b < fn.nblocks(); ++b) {
      uint32_t ob = fn.blocks[b].begin, oe = fn.blocks[b].end;
      fn.blocks[b].begin = (uint32_t)nc.size();
      for (uint32_t i = ob; i < oe; ++i)
        if (!drop[i]) nc.push_back(fn.code[i]);
      fn.blocks[b].end = (uint32_t)nc.size();
    }
    fn.code = std::move(nc);
  }
}

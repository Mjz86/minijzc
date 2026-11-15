#pragma once
// === destruct.hpp ========================================
// SSA destruction: split critical edges, materialize phis as copies,
// fuse copy chains.
//
// THEORY (phi lowering as parallel assignment):
//   A phi is a SIMULTANEOUS assignment of all its args at once --
//   when control traverses edge p -> s, every phi in s receives its
//   p-keyed arg in the same instant. Lowering to sequential copies
//   must preserve that parallelism; the two classic hazards are:
//   * THE SWAP CYCLE (lost-copy problem): `x,y = y,x` -- naive
//     sequential copies clobber the source before its reader runs.
//     sequentialize() therefore emits only copies whose dst is not
//     a pending source; whatever survives that greedy filter is
//     exactly a permutation cycle, broken by saving ONE source
//     into a fresh anon temp (cycles_broken) -- the swap then
//     reads the temp, never a clobbered slot.
//   * UNDEF ARGS: minimal SSA placed phis whose args are NONE on
//     edges where the variable was not live-in (an.hpp's
//     placement theory); destruction materializes them as
//     Const 0 -- matching the language's zero-initialized storage
//     (the oracle reads 0 there).
//   * CRITICAL EDGES: an edge from a 2-succ pred into a phi block
//     needs a block of its own -- the copies must execute ONLY on
//     that edge, but the pred's other successor must not see them.
//     split_critical_edges inserts [br s] on such edges; after it
//     every phi-block pred is single-succ, so each pred's copy set
//     can sit directly before its own terminator.
//   The rebuild reads block ranges from a SNAPSHOT (old_blocks):
//     it rewrites fn.blocks[p] while later preds still read phi
//     ranges by the old coordinates.
//   9g2's post_destruct_peephole then fuses `t = e; d = t` pairs
//   (single-reader renames) -- its soundness conditions (same
//   block, no intermediate reader of dst, def above the copy) are
//   documented inline below and are the load-bearing part.

#include "../base/pass_cfg.hpp"
#include "instr.hpp" // Fn (was masked by the old an.hpp include)
#include "../opt/opt_generic.hpp"
#include "stats.hpp"
#include "sweep.hpp"

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
// sequentialize one parallel copy set via the generic kernel
// (opt_ns::sequentialize_pcopy): the greedy emit-when-dst-not-needed
// filter, the permutation-cycle break by a fresh temp (the callback
// below mints the anon vreg exactly at the cycle-break points), undef
// args materialized as Const 0. The kernel works over (dst, src) id
// pairs and returns the op stream; the Instr emission stays here.
constexpr void sequentialize(Fn &fn,
                             std::vector<std::pair<uint32_t, uint32_t>> &pcs_ir,
                             std::vector<Instr> &out, DestrStats &ds) {
  std::vector<std::pair<uintlen_t, uintlen_t>> pcs{};
  pcs.reserve(pcs_ir.size());
  for (const auto &p : pcs_ir)
    pcs.push_back({(uintlen_t)p.first,
                   p.second == NONE ? opt_ns::none_v : (uintlen_t)p.second});
  opt_ns::pcopy_stats_t st{};
  const auto ops = opt_ns::sequentialize_pcopy(
      std::move(pcs),
      [&](uintlen_t d) {
        return (uintlen_t)new_anon_vreg(fn, fn.base_of((uint32_t)d));
      },
      st);
  for (const auto &op : ops) {
    if (op.zero) // undef arg: materialize 0
      out.push_back({.k = K::Const, .dst = (uint32_t)op.dst, .c = 0});
    else
      out.push_back(
          {.k = K::Copy, .dst = (uint32_t)op.dst, .a = (uint32_t)op.src});
  }
  ds.self_copies += (uint32_t)st.self_copies;
  ds.zeros += (uint32_t)st.zeros;
  ds.copies += (uint32_t)st.copies;
  ds.cycles_broken += (uint32_t)st.cycles_broken;
}

constexpr void split_critical_edges(Fn &fn, DestrStats &ds) {
  const uint32_t n0 = fn.nblocks();
  // the plan (pure CFG math): which (pred, slot) edges need a block of
  // their own -- a pred fanning out to >= 2 DISTINCT targets (this IR's
  // succ discipline: slot 1 valid implies slot 0 valid, so "2 valid
  // slots, not all one target" is exactly the old succ1/succ0 gate)
  // with a phi-holding target. Enumerated p-ascending, slot-ascending;
  // the client's rebuild mints identical fresh block ids in that order.
  std::vector<char> has_phi(n0, 0);
  for (uint32_t s = 0; s < n0; ++s)
    for (uint32_t i = fn.blocks[s].begin; i < fn.blocks[s].end; ++i)
      if (fn.code[i].k == K::Phi) {
        has_phi[s] = 1;
        break;
      }
  std::vector<std::vector<uintlen_t>> rows(n0);
  for (uint32_t p = 0; p < n0; ++p) {
    if (fn.succ0(p) != NONE)
      rows[p].push_back(fn.succ0(p));
    if (fn.succ1(p) != NONE)
      rows[p].push_back(fn.succ1(p));
  }
  const auto plan = opt_ns::calculate_edge_splits(rows, has_phi);
  for (const auto &sp : plan) {
    const uint32_t p = (uint32_t)sp.pred;
    const uint32_t si = (uint32_t)sp.slot;
    const uint32_t s = (uint32_t)sp.target;
    // fresh block m = [br s] on the edge p -> s
    uint32_t m = fn.nblocks();
    fn.blocks.push_back({});
    fn.succ.push_back(s);
    fn.succ.push_back(NONE);
    fn.blocks[m].begin = (uint32_t)fn.code.size();
    fn.code.push_back({.k = K::Br, .t = s});
    fn.blocks[m].end = (uint32_t)fn.code.size();
    // retarget p's edge s -> m (succ table + terminator instr)
    if (si == 0)
      fn.succ[2 * p] = m;
    else
      fn.succ[2 * p + 1] = m;
    for (uint32_t i = fn.blocks[p].begin; i < fn.blocks[p].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k == K::Cbr) {
        if (si == 0 && I.t == s)
          I.t = m;
        if (si == 1 && I.f == s)
          I.f = m;
      } else if (I.k == K::Br && I.t == s) {
        I.t = m;
      }
    }
    // rekey phis in s: the arg formerly keyed by pred p now arrives via m
    for (uint32_t i = fn.blocks[s].begin; i < fn.blocks[s].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k != K::Phi)
        break;
      if (I.t == p)
        I.t = m;
      if (I.f == p)
        I.f = m;
    }
    ds.edges_split++;
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
        if (fn.code[i].k != K::Phi)
          nc.push_back(fn.code[i]);
      const Instr term = fn.code[oe - 1];
      uint32_t s = fn.succ0(p);
      if (fn.succ1(p) == NONE && s != NONE) {
        std::vector<std::pair<uint32_t, uint32_t>> pcs{};
        for (uint32_t i = old_blocks[s].begin; i < old_blocks[s].end; ++i) {
          const Instr &I = fn.code[i];
          if (I.k != K::Phi)
            break;
          uint32_t arg = NONE;
          if (I.t == p && I.a != NONE)
            arg = I.a;
          else if (I.f == p && I.b2 != NONE)
            arg = I.b2;
          pcs.push_back({I.dst, arg}); // arg NONE -> undef -> zero
        }
        if (!pcs.empty())
          sequentialize(fn, pcs, nc, ds);
      }
      nc.push_back(term);
    }
    fn.blocks[p] = {nbegin, (uint32_t)nc.size()};
  }
  fn.code = std::move(nc);
  fn.recompute_preds();
}

// --- 9g2. post-destruction copy fusion
// ---------------------------------------- Destruction leaves `t = <expr>; d =
// t;` pairs (the phi-edge copy of a freshly computed temp). When t has exactly
// ONE reader (that copy), the defining instruction's destination is renamed to
// d and the copy dies. The defining op stays exactly where it was, so traps,
// arena reads and stdin consumption do not move; d's only other writers are the
// *other* phi-edge copies, which execute later on their own paths, and d's
// readers are all dominated by the join block — so the early write is never
// observed except on the path the copy served. Copy chains collapse the same
// way. This is the IR-level half of "make the backend input non-redundant"; the
// emission plan below does the rest (immediates, branch fusion).
constexpr void post_destruct_peephole(Fn &fn, DestrStats &ds) {
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  for (int round = 0; round < 256; ++round) {
    if (fn.code.empty())
      return;
    std::vector<uint32_t> uses(nv, 0);
    std::vector<uint32_t> def_idx(nv, NONE);
    for (uint32_t i = 0; i < fn.code.size(); ++i) {
      const Instr &I = fn.code[i];
      if (I.a != NONE && I.a < nv)
        uses[I.a]++;
      if (I.b2 != NONE && I.b2 < nv)
        uses[I.b2]++;
      if (I.c2 != NONE && I.c2 < nv)
        uses[I.c2]++;
      for (uint32_t v : I.args)
        if (v < nv)
          uses[v]++;
      if (I.dst != NONE && I.dst < nv)
        def_idx[I.dst] = i;
    }
    bool changed = false;
    std::vector<char> keep(fn.code.size(), 1);
    for (uint32_t b = 0; b < fn.nblocks() && !changed; ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end && !changed;
           ++i) {
        Instr &I = fn.code[i];
        if (I.k != K::Copy || I.dst == NONE || I.a == NONE || I.a == I.dst)
          continue;
        if (I.a >= nv || uses[I.a] != 1)
          continue; // src read only here
        uint32_t d = def_idx[I.a];
        if (d == NONE || d == i || !keep[d])
          continue;
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
        if (D.dst != I.a)
          continue;
        bool same_block = (d >= fn.blocks[b].begin && d < fn.blocks[b].end);
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
          if (between_writes_src)
            continue;
          I.a = D.a;
          keep[d] = 0;
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
            bool reads_dst = M.a == I.dst || M.b2 == I.dst || M.c2 == I.dst;
            for (uint32_t av : M.args)
              reads_dst |= (av == I.dst);
            if (reads_dst) {
              between_reads_dst = true;
              break;
            }
          }
          if (between_reads_dst)
            continue;
          D.dst = I.dst;
          keep[i] = 0;
        } else
          continue;
        ds.fused++;
        changed = true;
      }
    if (!changed)
      return;
    // sweep the one dropped instr, preserving block ranges
    sweep_survivors(fn, keep);
  }
}

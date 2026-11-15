#pragma once
// === sccp.hpp ========================================
// sparse conditional constant propagation (SCCP).
//
// THEORY (the lattice, the fixpoint, and why it is SPARSE):
//   * LATTICE per vreg: TOP ("not yet known") -> CONST ("provably
//     this exact value on EVERY executable path") -> OVERDEF
//     ("could be anything"). Descent is monotone (TOP->CN->OD
//     only); over a finite-height lattice, iterating monotone
//     transfer functions reaches a unique least fixpoint
//     (Tarski) -- the best sound approximation of "constant on
//     every executable path". The fixpoint is unique regardless
//     of visit order, so the engine's tables (lat/cval/bexec/
//     eexec) are a pure function of the inputs -- which is why
//     the rewrite below is byte-identical even though the
//     worklist internals are now kernel code.
//   * SPARSE propagation. Instead of dense per-block dataflow,
//     two worklists carry DELTAS only: the var worklist holds
//     vregs whose lattice just lowered (their USES -- the use
//     lists built in the one scan up top -- are revisited), the
//     edge worklist holds out-edges that became executable
//     (their target blocks are re-visited). Each instruction is
//     re-evaluated only when an input actually changed:
//     O(edges + use sites) propagations.
//   * CONDITIONAL = executable edges. bexec/eexec track which
//     blocks/edges can execute at all; a Cbr whose cond is CONST
//     makes ONLY its taken out-edge executable, and a Phi merges
//     its args over EXECUTABLE in-edges only (the duplicate-pred
//     case: ANY of a repeated pred's slots reaching the phi block
//     may carry the arg -- the N-ary rule; this IR's phis are the
//     2-entry records, an N-ary-phi IR consumes the same engine
//     with longer records). Code in never-executable blocks stays
//     TOP and is never claimed -- the dead arm is pruned as a Br
//     here and its block is left to the unreach pass.
//   * SOUNDNESS DETAILS. Trapping Div/Mod go OVERDEF (a runtime
//     trap is observable); params/Load/Input/Call/Len/Alloc are
//     OVERDEF by definition; K::Depth in main is CONST 1. The
//     step guard refuses to rewrite on suspected non-termination
//     (no rewrite, not a wrong rewrite). The rewrite phase
//     (CONST defs -> Const instrs, pruned Cbrs -> Br) touches
//     only executable blocks.
//
// THE SPLIT (this pass vs the kernel): the ENGINE -- the two
// worklists, the executability tables, the phi merge over
// executable in-edges, the monotone-descent check, the revisit
// discipline, the step guard -- is the generic sccp_ns
// (opt_generic.hpp) with N-ary phi records; the SEMANTICS -- the
// transfer for Const/Copy/Depth/Bin and the branch decision --
// stay here as the eval callback (a closure over scalars, the
// std::sort-comparator precedent), and the rewrite is a pure
// read of the engine's tables.

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../opt_generic.hpp"

// --- 9a0. sparse conditional constant propagation
// ----------------------------- Lattice per vreg: TOP (not yet known) -> CONST
// (provably that exact value on EVERY executable path) -> OVERDEF (anything). A
// block becomes executable once any in-edge is; a Cbr with a CONST condition
// makes ONLY the taken out-edge executable (branch pruning), so a Phi merges
// its args over EXECUTABLE in-edges only. That is what proves constants the
// local fold pass cannot see:
//   * conditional constants: `if (c) x=1; else x=1;`-style joins, and values
//     that are constant on every path that actually reaches the use because
//     the other branch is provably dead
//   * loop-carried constants: a phi whose executable back-edge argument
//     equals its entry argument (i = phi(0, j) with j provably 0)
// At the fixpoint every CONST vreg gets its def rewritten to a Const instr
// and every pruned Cbr becomes a Br (the dead arm goes unreachable for the
// unreach pass). Trapping Div/Mod never become constants: a runtime trap is
// observable, so they go OVERDEF and keep their runtime behavior. Params,
// Load/Input/Call/Len/Alloc results are OVERDEF by definition; K::Depth in
// main is provably 1 (the frame count of main).
constexpr bool opt_sccp(Fn &fn, OptStats &os, bool is_main = false) {
  const uint32_t nb = fn.nblocks();
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nb || !nv)
    return false;
  auto U = [](uint32_t v) { return v == NONE ? opt_ns::none_v : (uintlen_t)v; };
  // ---- the site tables: uses, kinds, and the N-ary phi records (this
  // IR: two (pred, arg) entries per phi, slot order t then f)
  opt_ns::sccp_engine_t<V> E{};
  E.succs.assign(nb, {});
  for (uint32_t b = 0; b < nb; ++b) {
    E.succs[b].push_back(U(fn.succ0(b)));
    E.succs[b].push_back(U(fn.succ1(b)));
  }
  std::vector<std::vector<uintlen_t>> uses(nv);
  std::vector<uint32_t> defi(nv, NONE); // def-less vregs feed the init below
  E.sites.assign(fn.code.size(), {});
  E.sites_of_block.assign(nb, {});
  {
    uintlen_t ord = 0;
    for (uint32_t b = 0; b < nb; ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        if (i >= fn.code.size())
          return false; // malformed: refuse
        E.sites_of_block[b].push_back(i);
        const Instr &I = fn.code[i];
        auto bump = [&](uint32_t v) {
          if (v != NONE && v < nv)
            uses[v].push_back(i);
        };
        bump(I.a);
        bump(I.b2);
        bump(I.c2);
        for (uint32_t v : I.args)
          bump(v);
        auto &si = E.sites[i];
        si.dst = U(I.dst);
        if (I.dst != NONE && I.dst < nv)
          defi[I.dst] = i;
        if (I.k == K::Phi) {
          si.kind = opt_ns::sccp_phi_v;
          si.phi = ord;
          E.phi_block.push_back(b);
          E.phi_entry_dst.push_back(si.dst);
          E.phi_entry_pred.push_back(U(I.t));
          E.phi_entry_arg.push_back(U(I.a));
          E.phi_entry_pred.push_back(U(I.f));
          E.phi_entry_arg.push_back(U(I.b2));
          E.phi_entry_begin.push_back(2 * ord);
          ++ord;
        } else if (I.k == K::Br || I.k == K::Cbr) {
          si.kind = opt_ns::sccp_branch_v;
        } else {
          si.kind = opt_ns::sccp_value_v;
        }
      }
    E.phi_entry_begin.push_back(2 * ord);
  }
  E.uses = std::move(uses);
  E.entry_block = 0;
  // initial states: params and def-less vregs are runtime values (OVERDEF)
  E.lat.assign(nv, opt_ns::sccp_top_v);
  E.cval.assign(nv, 0);
  for (uint32_t v = 0; v < nv; ++v)
    if (v < fn.nparams || defi[v] == NONE)
      E.lat[v] = opt_ns::sccp_od_v;
  const uint64_t guard = 1024 + 64ull * (fn.code.size() + 2 * nv + 8 * nb);
  // ---- run the engine; the eval closure is the SEMANTICS half
  const bool ran = E.run(
      [&](uintlen_t s, uintlen_t b) {
        const Instr &I = fn.code[s];
        if (I.dst != NONE && I.dst < nv) {
          uint8_t nl = opt_ns::sccp_top_v;
          int64_t nc = 0;
          switch (I.k) {
          case K::Const:
            nl = opt_ns::sccp_cn_v;
            nc = I.c;
            break;
          case K::Copy:
            nl = (I.a != NONE && I.a < nv) ? E.lat[I.a]
                                           : uint8_t(opt_ns::sccp_od_v);
            nc = (I.a != NONE && I.a < nv) ? E.cval[I.a] : 0;
            break;
          case K::Depth:
            if (is_main) {
              nl = opt_ns::sccp_cn_v;
              nc = 1;
            } else
              nl = opt_ns::sccp_od_v;
            break;
          case K::Bin: {
            const uint8_t la = (I.a != NONE && I.a < nv)
                                   ? E.lat[I.a]
                                   : uint8_t(opt_ns::sccp_od_v);
            const uint8_t lb = (I.b2 != NONE && I.b2 < nv)
                                   ? E.lat[I.b2]
                                   : uint8_t(opt_ns::sccp_od_v);
            if (la == opt_ns::sccp_od_v || lb == opt_ns::sccp_od_v) {
              nl = opt_ns::sccp_od_v;
            } else if (la == opt_ns::sccp_top_v || lb == opt_ns::sccp_top_v) {
              nl = opt_ns::sccp_top_v; // optimistic: wait for the other
            } else {
              Rv r = w_bin(I.b, E.cval[I.a], E.cval[I.b2]);
              if (r.trap)
                nl = opt_ns::sccp_od_v; // runtime trap is observable
              else {
                nl = opt_ns::sccp_cn_v;
                nc = r.v;
              }
            }
            break;
          }
          default:
            nl = opt_ns::sccp_od_v;
            break; // Alloc/Load/Call/Len/Input: runtime value
          }
          E.lower(I.dst, nl, nc);
        }
        // control flow: which out-edges become executable
        if (I.k == K::Br) {
          E.push_edge(b, 0);
        } else if (I.k == K::Cbr) {
          const uint8_t lc = (I.a != NONE && I.a < nv)
                                 ? E.lat[I.a]
                                 : uint8_t(opt_ns::sccp_od_v);
          if (lc == opt_ns::sccp_cn_v)
            E.push_edge(b, E.cval[I.a] != 0 ? 0 : 1);
          else if (lc == opt_ns::sccp_od_v) {
            E.push_edge(b, 0);
            E.push_edge(b, 1);
          }
          // TOP: the Cbr is in uses[cond]; revisited when the cond lowers
        }
      },
      guard);
  if (!ran)
    return false; // non-termination safety: no rewrite
  // ---- rewrite: CONST defs -> Const instrs; pruned Cbrs -> Br ---------------
  // (a pure read of the engine's tables)
  bool changed = false;
  for (uint32_t b = 0; b < nb; ++b) {
    if (!E.bexec[b])
      continue; // dead code: the unreach pass owns it
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k == K::Cbr && I.a != NONE && I.a < nv &&
          E.lat[I.a] == opt_ns::sccp_cn_v) {
        I.k = K::Br;
        I.t = (E.cval[I.a] != 0) ? I.t : I.f;
        I.f = NONE;
        I.a = NONE;
        fn.succ[2 * b] = I.t;
        fn.succ[2 * b + 1] = NONE;
        os.sccp_brs++;
        changed = true;
        continue;
      }
      if (I.dst != NONE && I.dst < nv && E.lat[I.dst] == opt_ns::sccp_cn_v &&
          I.k != K::Const) {
        I.k = K::Const;
        I.c = E.cval[I.dst];
        I.a = I.b2 = I.c2 = NONE;
        I.t = I.f = NONE;
        I.b = B{};
        I.nt = false;
        I.args.clear();
        os.sccp_consts++;
        changed = true;
      }
    }
  }
  return changed;
}

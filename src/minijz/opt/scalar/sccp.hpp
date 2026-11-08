#pragma once
// === sccp.hpp ========================================
// sparse conditional constant propagation (SCCP).

#include "minijz/base/pass_cfg.hpp"
#include "minijz/ir/instr.hpp"

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
  enum : uint8_t { TOP = 0, CN = 1, OD = 2 };
  std::vector<uint8_t> lat(nv, TOP);
  std::vector<val_t> cval(nv, 0);
  std::vector<char> bexec(nb, 0);
  std::vector<char> eexec(2 * nb, 0);
  // def index, block-of, use lists (SSA: one def per vreg)
  std::vector<uint32_t> defi(nv, NONE);
  std::vector<uint32_t> blkof(fn.code.size(), NONE);
  std::vector<std::vector<uint32_t>> uses(nv);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      if (i >= fn.code.size())
        return false; // malformed: refuse
      blkof[i] = b;
      const Instr &I = fn.code[i];
      if (I.dst != NONE && I.dst < nv)
        defi[I.dst] = i;
      auto bump = [&](uint32_t v) {
        if (v != NONE && v < nv)
          uses[v].push_back(i);
      };
      bump(I.a);
      bump(I.b2);
      bump(I.c2);
      for (uint32_t v : I.args)
        bump(v);
    }
  // params and def-less vregs are runtime values: OVERDEF from the start
  for (uint32_t v = 0; v < nv; ++v)
    if (v < fn.nparams || defi[v] == NONE)
      lat[v] = OD;
  // meet two lattice values (result written to r/rc)
  auto meet = [&](uint8_t a, val_t av, uint8_t b, val_t bv, uint8_t &r,
                  val_t &rc) {
    if (a == TOP) {
      r = b;
      rc = bv;
      return;
    }
    if (b == TOP) {
      r = a;
      rc = av;
      return;
    }
    if (a == OD || b == OD) {
      r = OD;
      return;
    }
    if (av == bv) {
      r = CN;
      rc = av;
      return;
    }
    r = OD;
  };
  // is the in-edge `slot' of pred p (into block b) executable?
  auto arg_edge_exec = [&](uint32_t p, uint32_t b) -> bool {
    if (p == NONE || p >= nb)
      return false;
    bool s0 = fn.succ0(p) == b, s1 = fn.succ1(p) == b;
    if (!s0 && !s1)
      return false; // stale pred id: not an in-edge
    if (s0 && s1)   // duplicated pred: either edge may carry it
      return eexec[2 * p] || eexec[2 * p + 1];
    return eexec[2 * p + (s1 ? 1u : 0u)];
  };
  // lattice of one Phi over its EXECUTABLE in-edges
  auto eval_phi = [&](const Instr &I, uint32_t b, uint8_t &r, val_t &rc) {
    r = TOP;
    rc = 0;
    auto merge = [&](uint32_t pred, uint32_t arg) {
      if (!arg_edge_exec(pred, b))
        return;
      uint8_t al = (arg != NONE && arg < nv) ? lat[arg] : OD;
      val_t av = (arg != NONE && arg < nv) ? cval[arg] : 0;
      uint8_t mr{};
      val_t mc{};
      meet(r, rc, al, av, mr, mc);
      r = mr;
      rc = mc;
    };
    merge(I.t, I.a);  // slot 0: arg a arrives from pred t
    merge(I.f, I.b2); // slot 1: arg b2 arrives from pred f
  };
  // worklists: vregs whose lattice lowered, out-edges to make executable
  std::vector<uint32_t> ssa_wl{}, edge_wl{};
  uint64_t steps = 0;
  const uint64_t guard = 1024 + 64ull * (fn.code.size() + 2 * nv + 8 * nb);
  auto visit_instr = [&](uint32_t i) {
    const Instr &I = fn.code[i];
    // never evaluate code in a non-executable block: its values are dead
    // (dominance keeps every executable use out of them)
    uint32_t b = blkof[i];
    if (b == NONE || !bexec[b])
      return;
    if (I.dst != NONE && I.dst < nv) {
      uint8_t nl = TOP;
      val_t nc = 0;
      switch (I.k) {
      case K::Const:
        nl = CN;
        nc = I.c;
        break;
      case K::Copy:
        nl = (I.a != NONE && I.a < nv) ? lat[I.a] : OD;
        nc = (I.a != NONE && I.a < nv) ? cval[I.a] : 0;
        break;
      case K::Depth:
        if (is_main) {
          nl = CN;
          nc = 1;
        } else
          nl = OD;
        break;
      case K::Phi:
        eval_phi(I, blkof[i], nl, nc);
        break;
      case K::Bin: {
        uint8_t la = (I.a != NONE && I.a < nv) ? lat[I.a] : OD;
        uint8_t lb = (I.b2 != NONE && I.b2 < nv) ? lat[I.b2] : OD;
        if (la == OD || lb == OD) {
          nl = OD;
        } else if (la == TOP || lb == TOP) {
          nl = TOP; // optimistic: wait for the other operand
        } else {
          Rv r = w_bin(I.b, cval[I.a], cval[I.b2]);
          if (r.trap)
            nl = OD; // runtime trap is observable: keep the op
          else {
            nl = CN;
            nc = r.v;
          }
        }
        break;
      }
      default:
        nl = OD;
        break; // Alloc/Load/Call/Len/Input: runtime value
      }
      if (nl != lat[I.dst]) { // TOP->CN->OD only (monotone by construction)
        lat[I.dst] = nl;
        cval[I.dst] = nc;
        if (nl != TOP)
          ssa_wl.push_back(I.dst);
      }
    }
    // control flow: which out-edges become executable
    if (I.k == K::Br) {
      edge_wl.push_back(2 * b);
    } else if (I.k == K::Cbr) {
      uint8_t lc = (I.a != NONE && I.a < nv) ? lat[I.a] : OD;
      if (lc == CN)
        edge_wl.push_back(2 * b + (cval[I.a] != 0 ? 0 : 1));
      else if (lc == OD) {
        edge_wl.push_back(2 * b);
        edge_wl.push_back(2 * b + 1);
      }
      // TOP: the Cbr is in uses[cond]; revisited when the cond lowers
    }
  };
  auto visit_block = [&](uint32_t b) {
    if (!bexec[b])
      return;
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
      visit_instr(i);
  };
  bexec[0] = 1;
  visit_block(0);
  while (!edge_wl.empty() || !ssa_wl.empty()) {
    if (++steps > guard)
      return false; // non-termination safety: no rewrite
    if (!edge_wl.empty()) {
      uint32_t e = edge_wl.back();
      edge_wl.pop_back();
      if (eexec[e])
        continue;
      eexec[e] = 1;
      uint32_t b = e >> 1, slot = e & 1;
      uint32_t t = slot ? fn.succ1(b) : fn.succ0(b);
      if (t == NONE || t >= nb)
        continue;
      bexec[t] = 1;
      visit_block(t); // re-eval: new in-edge may lower its phis
      continue;
    }
    uint32_t v = ssa_wl.back();
    ssa_wl.pop_back();
    for (uint32_t i : uses[v])
      visit_instr(i); // re-eval each use
  }
  // ---- rewrite: CONST defs -> Const instrs; pruned Cbrs -> Br ---------------
  bool changed = false;
  for (uint32_t b = 0; b < nb; ++b) {
    if (!bexec[b])
      continue; // dead code: the unreach pass owns it
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k == K::Cbr && I.a != NONE && I.a < nv && lat[I.a] == CN) {
        I.k = K::Br;
        I.t = (cval[I.a] != 0) ? I.t : I.f;
        I.f = NONE;
        I.a = NONE;
        fn.succ[2 * b] = I.t;
        fn.succ[2 * b + 1] = NONE;
        os.sccp_brs++;
        changed = true;
        continue;
      }
      if (I.dst != NONE && I.dst < nv && lat[I.dst] == CN && I.k != K::Const) {
        I.k = K::Const;
        I.c = cval[I.dst];
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

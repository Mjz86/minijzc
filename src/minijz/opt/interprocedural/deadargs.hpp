#pragma once
// === deadargs.hpp ========================================
// interprocedural dead-argument elimination (DAE).

// ALGORITHM (opt_deadargs): (1) analysis -- per fn g, alive[g][p]
//   collects hard uses (any operand a/b2/c2, Bkpt args, and
//   version_base entries below nparams: that base var survives as a
//   local), softs[g][p] records Soft{h, q} forwarding edges (param p
//   passed as argument q of a call to h); eligibility drops main,
//   empty-param fns, tce_sel products, post-destruction shapes, param
//   redefinitions, and arity-mismatched callees. (2) least fixpoint:
//   alive(g, p) propagates through softs whenever the target argument
//   is alive (or its fn ineligible). (3) dead[g][p] = eligible &&
//   !alive. (4) rewrite 1 drops dead POSITIONS from I.args at every
//   call site (caller vreg ids untouched); rewrite 2 compacts each
//   DAE'd callee through the order-preserving permutation pi/mp --
//   dst, a, b2, c2, args, var_names, version_base base ids,
//   stk_arrays -- with Phi's stale `c` tag left as-is (unread after
//   ssa_construct).
// DATA STRUCTURES: the eligible/alive/softs/dead [g][p] tables, the
//   Soft{h, q} forwarding edges, and pi, the compaction map over the
//   callee's vreg space [0, nv).
// THEORY: alive only grows, so the iteration computes the LEAST
//   fixpoint -- exactly the observable parameters; a dead param is
//   read by no instruction, forwarded to no live parameter, and shown
//   by no Bkpt. Dropping an argument slot and dropping the parameter
//   are the two ends of one calling-convention edge, both rewritten
//   from the same dead table in this pass; pi is injective and
//   order-preserving on the survivors, so every remaining reference
//   stays consistent (base ids and SSA ids move together).
// DOWNSTREAM: driven after eliminate_dead_fns (the surviving call
//   sites are the final ones) and before the last per-fn round, whose
//   DCE reaps caller argument expressions that just went dead
//   (ostats[g].deadargs counts the removed params).
#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"

// --- 9e5. DEAD-ARGUMENT ELIMINATION (interprocedural)
// ------------------------------------- A parameter nobody observes can be
// removed from the function and from every call site. "Nobody observes"
// is the classic least-fixpoint over the whole program:
//     alive(g, p) = hard-use of vreg p in g's body
//                   OR some use of p as argument q of a call to h
//                      with alive(h, q)
// so a parameter that is only ever FORWARDED to the same dead parameter
// (self-recursion, mutual-recursion chains) dies with it. Hard uses are
// operands (a/b2/c2) of any instruction -- Load/Store/Print/Cbr/Ret/...
// -- and Call argument positions that feed LIVE parameters.
//
// Removing a parameter compacts the fn's vreg space: parameters are vregs
// 0..nparams-1, and base_of(v)=v below nvars, so the SAME order-preserving
// permutation is applied to base var ids (var_names, version_base entries)
// and SSA vreg ids (dst/a/b2/c2/args/stk_arrays). version_base/version_no
// keep their INDICES (nv-nvars is unchanged); only the base ids inside
// version_base move. Phi's `c` (the SSA-construction base-var tag) is left
// stale ON PURPOSE: after ssa_construct nothing in the pipeline reads it
// (destruction lowers phis from dst + a/b2/t/f; printers print dst/args;
// mem2reg only tags its OWN placeholder phis) -- verified by audit before
// this pass landed.
//
// Guards (any hit disqualifies the fn, not the whole pass):
//   * main (fns[0]) -- never called, nothing to rewrite at its sites;
//   * tailrec merge products (tce_sel != 0): param 0 is the sel dispatch;
//   * any call site with args.size() != nparams (arity mismatch = an
//     earlier pass left the program inconsistent -- never observed, but
//     rewriting around it would be wrong);
//   * any instruction DEFINING a param vreg (SSA violation paranoia);
//   * a param whose base var has other SSA versions (version_base entry
//     == p): the base var survives as a local, so its id must not move;
//   * a fn with slot_of already assigned (post-destruction shape): DAE
//     runs on SSA form only.
//
// Driven from optimize_prog AFTER eliminate_dead_fns (all inlining,
// constcall and callcse are done, so the remaining call sites are the
// final ones) and BEFORE the last per-fn optimize round: callers whose
// dropped argument expressions suddenly became dead get DCE'd there.
constexpr bool opt_deadargs(std::vector<Fn> &fns,
                            std::vector<OptStats> &ostats) {
  const uint32_t n = (uint32_t)fns.size();
  if (n < 2 || ostats.size() < n)
    return false;
  // ---- analysis -----------------------------------------------------------
  struct Soft { // a use of param p as argument q of a call to h
    uint32_t h, q;
  };
  std::vector<char> eligible(n, 1);
  std::vector<std::vector<char>> alive(n);              // [g][p]
  std::vector<std::vector<std::vector<Soft>>> softs(n); // [g][p] -> uses
  for (uint32_t g = 0; g < n; ++g) {
    const Fn &G = fns[g];
    const uint32_t P = G.nparams;
    alive[g].assign(P, 0);
    softs[g].assign(P, {});
    if (g == 0 || P == 0 || G.tce_sel != 0 || !G.slot_of.empty())
      eligible[g] = 0;
    // a base var with other SSA versions keeps its param id in the space
    for (uint32_t b : G.version_base)
      if (b < P)
        alive[g][b] = 1;
    // scan the body
    for (uint32_t bi = 0; bi < G.nblocks() && eligible[g]; ++bi)
      for (uint32_t i = G.blocks[bi].begin; i < G.blocks[bi].end; ++i) {
        const Instr &I = G.code[i];
        if (I.dst != NONE && I.dst < P) { // a def of a param: paranoia bail
          eligible[g] = 0;
          break;
        }
        for (uint32_t v : {I.a, I.b2, I.c2}) {
          if (v != NONE && v < P)
            alive[g][v] = 1; // hard use (operand of anything)
        }
        if (I.k == K::Bkpt) {       // observed at a debugging breakpoint: a
          for (uint32_t v : I.args) // hard use too (the value must survive
            if (v != NONE && v < P) // so the debugger can show it)
              alive[g][v] = 1;
        }
        if (I.k == K::Call) {
          uint32_t h = (uint32_t)I.c;
          if (h >= n)
            continue;
          if (I.args.size() != fns[h].nparams) { // arity mismatch: h keeps
            eligible[h] = 0;                     // all params, sites untouched
            continue;
          }
          for (uint32_t q = 0; q < I.args.size(); ++q) {
            uint32_t v = I.args[q];
            if (v != NONE && v < P)
              softs[g][v].push_back({h, q}); // soft use: alive iff h's q is
          }
        }
      }
  }
  // least fixpoint: alive propagates through forwarded arguments
  for (bool ch = true; ch;) {
    ch = false;
    for (uint32_t g = 0; g < n; ++g) {
      if (!eligible[g])
        continue;
      for (uint32_t p = 0; p < fns[g].nparams; ++p) {
        if (alive[g][p])
          continue;
        for (const Soft &s : softs[g][p]) {
          if (!eligible[s.h] || alive[s.h][s.q]) {
            alive[g][p] = 1; // feeds a LIVE param (or a non-candidate)
            ch = true;
            break;
          }
        }
      }
    }
  }
  // dead[g][p]
  std::vector<std::vector<char>> dead(n);
  bool any = false;
  for (uint32_t g = 0; g < n; ++g) {
    dead[g].assign(fns[g].nparams, 0);
    if (!eligible[g])
      continue;
    for (uint32_t p = 0; p < fns[g].nparams; ++p)
      if (!alive[g][p]) {
        dead[g][p] = 1;
        any = true;
      }
  }
  if (!any)
    return false;
  // ---- rewrite 1: drop dead argument positions at EVERY call site -----
  // (position-level surgery only; vreg ids of callers are untouched)
  for (uint32_t f = 0; f < n; ++f) {
    Fn &F = fns[f];
    for (uint32_t bi = 0; bi < F.nblocks(); ++bi)
      for (uint32_t i = F.blocks[bi].begin; i < F.blocks[bi].end; ++i) {
        Instr &I = F.code[i];
        if (I.k != K::Call)
          continue;
        uint32_t g = (uint32_t)I.c;
        if (g >= n || !eligible[g])
          continue;
        bool drop = false;
        for (uint32_t p = 0; p < dead[g].size(); ++p)
          drop |= dead[g][p] != 0;
        if (!drop)
          continue;
        if (I.args.size() != fns[g].nparams)
          continue; // guarded above (eligible[g] would be 0) -- belt+braces
        std::vector<uint32_t> keep{};
        keep.reserve(I.args.size());
        for (uint32_t p = 0; p < I.args.size(); ++p)
          if (!dead[g][p])
            keep.push_back(I.args[p]);
        I.args = std::move(keep);
      }
  }
  // ---- rewrite 2: compact each DAE'd fn's vreg space --------------------
  for (uint32_t g = 0; g < n; ++g) {
    Fn &G = fns[g];
    uint32_t ndead = 0;
    for (uint32_t p = 0; p < dead[g].size(); ++p)
      ndead += dead[g][p] != 0;
    if (!ndead)
      continue;
    const uint32_t nv = G.nvars + (uint32_t)G.version_base.size();
    // pi: order-preserving compaction over [0, nv); NONE marks a dropped id
    std::vector<uint32_t> pi(nv, NONE);
    uint32_t shift = 0;
    for (uint32_t v = 0; v < nv; ++v) {
      if (v < G.nparams && dead[g][v]) {
        ++shift;
        continue;
      }
      pi[v] = v - shift;
    }
    auto mp = [&](uint32_t v) -> uint32_t {
      return (v == NONE || v >= nv) ? v : pi[v];
    };
    for (uint32_t bi = 0; bi < G.nblocks(); ++bi)
      for (uint32_t i = G.blocks[bi].begin; i < G.blocks[bi].end; ++i) {
        Instr &I = G.code[i];
        if (I.dst != NONE)
          I.dst = mp(I.dst);
        if (I.a != NONE)
          I.a = mp(I.a);
        if (I.b2 != NONE)
          I.b2 = mp(I.b2);
        if (I.c2 != NONE)
          I.c2 = mp(I.c2);
        for (uint32_t &v : I.args)
          v = mp(v);
        // Phi's c (base-var tag) intentionally left stale: unread after
        // ssa_construct; Call's c is a fn index, not a vreg.
      }
    if (G.var_names.size() == G.nvars) {
      std::vector<std::string_view> nn{};
      nn.reserve(G.nvars - ndead);
      for (uint32_t v = 0; v < G.nvars; ++v)
        if (pi[v] != NONE)
          nn.push_back(G.var_names[v]);
      G.var_names = std::move(nn);
    }
    for (uint32_t &b : G.version_base)
      if (b != NONE && b < nv)
        b = pi[b];
    for (auto &sa : G.stk_arrays) {
      sa.root = mp(sa.root);
      for (uint32_t &v : sa.vregs)
        v = mp(v);
    }
    G.nvars -= ndead;
    G.nparams -= ndead;
    ostats[g].deadargs += ndead;
  }
  return true;
}

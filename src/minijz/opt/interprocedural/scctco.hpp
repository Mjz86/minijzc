#pragma once
// === scctco.hpp ========================================
// SCC tail-call optimization: collapse intra-SCC tail calls into
// loops (helpers, per-member collapse, the driver).
//
// ALGORITHM: opt_scc_tailcall builds the call graph, then per SCC
// (<= 8 members, main excluded, total <= 6000 instrs, every internal
// edge a tail call with matching arg count) snapshots the members
// (snap/snap_of) and runs scctco_member, which prepends D (block 0:
// t0/k1/z0) + H (block 1), drains the internal tail sites one per
// round (iter_cap): a self site becomes a back edge (promote/
// add_backedge), a sibling site inlines the sibling's snapshot once;
// then the 3b fixup rotates raw pass-through params, scctco_resucc
// runs, and scctco_fold sees the pristine shape.
//
// DATA STRUCTURES: ScctcoHead{entry, root_pred, root_vals, phis,
// tip_pred, tip_vals, cb, ce, trip_src, trip_phi, phi_vals}, the
// per-member merge heads (heads[]); trapb (the shared 1/0 trap
// block); t0/k1/z0, D's consts (511/1/0); T_cur + trip_read.
//
// THEORY: a tail call has no continuation -- the caller returns the
// callee's value unchanged -- so swapping [Call;Ret] for a CFG edge
// into the callee's code preserves the function's semantics: the
// same value is produced (a self site feeds the parameter phis, so
// the params become loop-carried induction variables; a sibling
// site's inlined copy rets ARE the caller's rets), while the loop-
// carried trip counter (511 at entry, -1 per edge, trap at <0)
// reproduces the 512-frame TRAP exactly at the 513th virtual call.
// The counter runs DOWN so the guard bounds the phi from both
// sides at once: interval analysis converges in ~2 rounds, not 512.
//
// DOWNSTREAM: scctco_fold turns count-down-by-c loops into guard +
// V0 & (c-1) (is_odd -> n & 1); members stop being recursive, so
// the inliner pulls them in and range analysis deletes the guard.

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "callgraph.hpp"
#include "scctco_fold.hpp"

// ============================================================================
// 9e1b. SCC tail-call optimization ("scctco")
// ============================================================================
// The inliner (9e2) never touches recursive functions; this pass fills that
// gap for TAIL recursion: an SCC of the call graph whose every internal
// edge is a tail call (`return f(..)` -- the call result flows straight out
// and nothing else reads it) is collapsed into intra-function loops:
//
//   fn is_even(n) { if (n == 0) { return 1; } return is_odd(n - 1); }
//   fn is_odd(n)  { if (n == 0) { return 0; } return is_even(n - 1); }
//
// is_odd becomes a single count-down-by-2 loop (is_even loses its only
// caller and dies), which the residue fold below rewrites to `n & 1`.
//
// Construction, per SCC member f (siblings are inlined from PRE-collapse
// snapshots, so every member collapses independently):
//   * a fresh entry D (block 0: trip=0, the cap constant, br H) and loop
//     header H (block 1: parameter phis, inserted at the first back edge)
//     are prepended; every read of a parameter is rewritten to its phi, so
//     the parameters become the loop-carried induction variables
//   * a SELF tail site `return f(args)` becomes a back edge into H feeding
//     the parameter phis; a second/third site merges through a chain of
//     2-pred phi blocks (binary search: both recursive arms -> one loop)
//   * a SIBLING tail site inlines the sibling's snapshot body ONCE (a tail
//     site has no continuation, so the callee's `ret`s simply become the
//     caller's `ret`s); any further edge to the same sibling jumps back to
//     that one copy through parameter phis promoted in its entry block
//   => every SCC edge is exactly one CFG edge: mutual recursion is a
//      trivial loop; self recursion with several arms is one loop with
//      several exits (nothing is duplicated beyond one copy per sibling).
//
// FRAME-PARITY GUARD: the language caps recursion at 512 frames -> TRAP in
// every backend, so each collapsed edge increments a loop-carried trip
// counter (threaded through every merge phi, sibling copies included: a
// copy's sites read the trip ARRIVING at its entry, which is the root
// site's incremented value) and traps past 512: recursion deep enough to
// trap still traps -- exactly at the 513th virtual call when the function
// is entered at frame depth 1 (a deeper entry can differ by the caller's
// frame count; the alternative, no guard, would turn guaranteed traps into
// step-budget timeouts).
//
// RESIDUE FOLD: right after the collapse (while the shape is pristine) a
// loop whose only parameter phi counts down by a constant step c, whose
// body is pure (no Print/Load/Store/Alloc/Len/Input/Call/Div/Mod) and
// whose exits are `V == r` returning the constant r for EVERY residue
// r in 0..c-1, is replaced by
//     if (V0 < 0 || V0 >= 512*c) -> TRAP   (the cap, now a range check)
//     ret V0 & (c-1)                        (c a power of two: strength
//                                            reduction of V0 % c)
// which is what turns is_odd above into `guard + n & 1`. Loops that do not
// match (binary search: two induction variables) simply stay loops -- TCO
// without the closed form. The folded body is tiny, so the plain inliner
// then pulls it into callers and tier-0 range analysis deletes the guard
// whenever V0's interval proves it dead.

// ---- small helpers ---------------------------------------------------------

// uses of vreg v anywhere in fn (a/b2/c2/args; phi args included)
constexpr uint32_t scctco_uses(const Fn &fn, uint32_t v) {
  uint32_t n = 0;
  for (const Instr &I : fn.code) {
    if (I.a == v)
      ++n;
    if (I.b2 == v)
      ++n;
    if (I.c2 == v)
      ++n;
    for (uint32_t u : I.args)
      if (u == v)
        ++n;
  }
  return n;
}

// Is the Call at (block b, index i) a TAIL call: its dst is read ONLY by
// the block's terminating Ret (which returns it)?
constexpr bool scctco_is_tail(const Fn &fn, uint32_t b, uint32_t i) {
  const Instr &I = fn.code[i];
  if (I.k != K::Call || I.dst == NONE)
    return false;
  const Block &B = fn.blocks[b];
  if (B.begin >= B.end)
    return false; // empty/tombstone: no terminator
  if (fn.code[B.end - 1].k != K::Ret)
    return false;
  if (fn.code[B.end - 1].a != I.dst)
    return false;
  return scctco_uses(fn, I.dst) == 1;
}

// prepend instrs to block `blk` (phis must stay first); later blocks shift
constexpr void scctco_prepend(Fn &fn, uint32_t blk,
                              const std::vector<Instr> &ins) {
  const uint32_t pos = fn.blocks[blk].begin;
  fn.code.insert(fn.code.begin() + pos, ins.begin(), ins.end());
  const int64_t d = (int64_t)ins.size();
  for (uint32_t q = 0; q < fn.nblocks(); ++q) {
    Block &B = fn.blocks[q];
    if (q == blk) {
      B.end = (uint32_t)((int64_t)B.end + d);
      continue;
    }
    if (B.begin > pos) { // a block ending AT pos is unaffected
      B.begin = (uint32_t)((int64_t)B.begin + d);
      B.end = (uint32_t)((int64_t)B.end + d);
    }
  }
}

// replace block b's code wholesale (block ranges after it shift)
constexpr void scctco_set_block(Fn &fn, uint32_t b, std::vector<Instr> ns) {
  const uint32_t bg = fn.blocks[b].begin, en = fn.blocks[b].end;
  fn.code.erase(fn.code.begin() + bg, fn.code.begin() + en);
  fn.code.insert(fn.code.begin() + bg, ns.begin(), ns.end());
  const int64_t d = (int64_t)ns.size() - (int64_t)(en - bg);
  fn.blocks[b].end = bg + (uint32_t)ns.size();
  for (uint32_t q = 0; q < fn.nblocks(); ++q) {
    if (q == b)
      continue;
    Block &B = fn.blocks[q];
    if (B.begin >= en) {
      B.begin = (uint32_t)((int64_t)B.begin + d);
      B.end = (uint32_t)((int64_t)B.end + d);
    }
  }
}

// point block b's terminator edge `from` -> `to` (from == NONE sets the
// unset false arm of a fresh guard Cbr)
constexpr void scctco_retarget(Fn &fn, uint32_t b, uint32_t from, uint32_t to) {
  const Block &B = fn.blocks[b];
  if (B.begin >= B.end)
    return; // empty/tombstone
  Instr &T = fn.code[B.end - 1];
  if (T.k == K::Br && T.t == from)
    T.t = to;
  else if (T.k == K::Cbr) {
    if (T.t == from)
      T.t = to;
    if (T.f == from)
      T.f = to;
  }
}

// rewrite uses of vreg `from` -> `to` in blocks [lo, hi) except `skip`
constexpr void scctco_rewrite(Fn &fn, uint32_t lo, uint32_t hi, uint32_t skip,
                              uint32_t from, uint32_t to) {
  for (uint32_t b = lo; b < hi && b < fn.nblocks(); ++b) {
    if (b == skip)
      continue;
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      Instr &I = fn.code[i];
      if (I.a == from)
        I.a = to;
      if (I.b2 == from)
        I.b2 = to;
      if (I.c2 == from)
        I.c2 = to;
      for (uint32_t &u : I.args)
        u = (u == from) ? to : u;
    }
  }
}

// rebuild succ from terminators, then preds (empty/tombstone blocks stay
// isolated: their range is [x,x), and reading code[x-1] would steal the
// PREVIOUS block's terminator)
constexpr void scctco_resucc(Fn &f) {
  f.succ.assign(2 * f.nblocks(), NONE);
  for (uint32_t b = 0; b < f.nblocks(); ++b) {
    if (f.blocks[b].begin >= f.blocks[b].end)
      continue; // empty range
    const Instr &T = f.code[f.blocks[b].end - 1];
    if (T.k == K::Br)
      f.succ[2 * b] = T.t;
    else if (T.k == K::Cbr) {
      f.succ[2 * b] = T.t;
      f.succ[2 * b + 1] = T.f;
    }
  }
  f.recompute_preds();
}

// ---- the residue fold (runs on the pristine collapsed shape) ---------------

// ---- collapse one SCC member ------------------------------------------------

struct ScctcoHead {
  uint32_t entry{NONE};     // block carrying the merge phis
  uint32_t root_pred{NONE}; // the one non-back-edge predecessor
  std::vector<uint32_t> root_vals{};
  bool phis{false};
  uint32_t tip_pred{NONE}; // current second predecessor
  std::vector<uint32_t> tip_vals{};
  uint32_t cb{NONE}, ce{NONE};      // the sibling copy's block range
  uint32_t trip_src{NONE};          // trip ARRIVING at the entry (pre-phi)
  uint32_t trip_phi{NONE};          // ... and the phi result (post-phi)
  std::vector<uint32_t> phi_vals{}; // self only: the param rotation phis
};

constexpr bool scctco_member(Fn &f, uint32_t self_fn,
                             const std::vector<Fn> &snap,
                             const std::vector<uint32_t> &snap_of, uint32_t nfn,
                             OptStats &os, uint8_t dbg) {
  const uint32_t nparams = f.nparams;
  if (nparams > 6)
    return false;
  for (uint32_t i = f.blocks[0].begin; i < f.blocks[0].end; ++i)
    if (f.code[i].k == K::Phi)
      return false; // entry must be phi-free

  // ---- 1. prepend D (block 0) + H (block 1); old blocks shift by 2 -------
  const uint32_t nb0 = f.nblocks();
  // trip counts DOWN from 511 (the frame budget): each collapsed edge does
  // r' = r - 1 and traps past r' < 0 (edge #512 = the 513th virtual call,
  // exactly the depth cap). A DOWN counter is also what makes the interval
  // range analysis converge in ~2 rounds: the guard's false arm refines
  // r' >= 0 immediately, bounding BOTH sides of the phi (an UP counter's
  // interval grows +1 per round for ~512 rounds before widening, which
  // multiplied by inlined copies made analysis of the caller quadratic).
  const uint32_t t0 = new_anon_vreg(f, 0); // r = 511 on the entry edge
  const uint32_t k1 = new_anon_vreg(f, 0); // 1 (the guard decrement)
  const uint32_t z0 = new_anon_vreg(f, 0); // 0 (the guard comparison)
  {
    std::vector<Instr> nc;
    nc.reserve(f.code.size() + 8);
    nc.push_back(Instr{.k = K::Const, .dst = t0, .c = 511});
    nc.push_back(Instr{.k = K::Const, .dst = k1, .c = 1});
    nc.push_back(Instr{.k = K::Const, .dst = z0, .c = 0});
    nc.push_back(Instr{.k = K::Br, .t = 1});
    nc.push_back(Instr{.k = K::Br, .t = 2}); // H: phis come with the 1st edge
    for (uint32_t b = 0; b < nb0; ++b)
      for (uint32_t i = f.blocks[b].begin; i < f.blocks[b].end; ++i) {
        Instr I = f.code[i];
        if (I.k == K::Br) {
          if (I.t != NONE)
            I.t += 2;
        } else if (I.k == K::Cbr) {
          if (I.t != NONE)
            I.t += 2;
          if (I.f != NONE)
            I.f += 2;
        } else if (I.k == K::Phi) {
          if (I.t != NONE)
            I.t += 2;
          if (I.f != NONE)
            I.f += 2;
        }
        nc.push_back(I);
      }
    std::vector<Block> nbl;
    std::vector<uint32_t> nsucc;
    nbl.push_back({0, 4}); // D:  [t0, k1, z0, br H]
    nsucc.push_back(1);
    nsucc.push_back(NONE);
    nbl.push_back({4, 5}); // H:  [br E] (phis come with the first edge)
    nsucc.push_back(2);
    nsucc.push_back(NONE);
    for (uint32_t b = 0; b < nb0; ++b) {
      nbl.push_back({5 + f.blocks[b].begin, 5 + f.blocks[b].end});
      nsucc.push_back(f.succ[2 * b] == NONE ? NONE : f.succ[2 * b] + 2);
      nsucc.push_back(f.succ[2 * b + 1] == NONE ? NONE : f.succ[2 * b + 1] + 2);
    }
    f.code = std::move(nc);
    f.blocks = std::move(nbl);
    f.succ = std::move(nsucc);
  }
  f.recompute_preds();

  ScctcoHead self{};
  self.entry = 1;
  self.root_pred = 0;
  for (uint32_t p = 0; p < nparams; ++p)
    self.root_vals.push_back(p);
  self.root_vals.push_back(t0);
  self.trip_src = t0;
  const uint32_t NV = nparams + 1; // carried values: params + trip

  std::vector<ScctcoHead> heads(nfn);
  uint32_t trapb = NONE;
  uint32_t T_cur = t0; // the trip vreg f-region sites read

  auto append_block = [&](const std::vector<Instr> &ins) -> uint32_t {
    uint32_t id = f.nblocks();
    uint32_t base = (uint32_t)f.code.size();
    f.code.insert(f.code.end(), ins.begin(), ins.end());
    f.blocks.push_back({base, (uint32_t)f.code.size()});
    f.succ.push_back(NONE);
    f.succ.push_back(NONE);
    return id;
  };

  // the trip vreg a site in block b increments: f-level, or the copy's
  // (a copy's sites count from the trip ARRIVING at the copy's entry)
  auto trip_read = [&](uint32_t blk) -> uint32_t {
    for (const ScctcoHead &h : heads) {
      if (h.cb == NONE || blk < h.cb || blk >= h.ce)
        continue;
      return h.phis ? h.trip_phi : h.trip_src;
    }
    return T_cur;
  };

  // materialize the merge phis at Hd.entry for (root edge + first edge b)
  auto promote = [&](ScctcoHead &Hd, uint32_t b,
                     const std::vector<uint32_t> &vals) {
    std::vector<Instr> phis;
    std::vector<uint32_t> res;
    for (uint32_t v = 0; v < NV; ++v) {
      uint32_t dst = new_anon_vreg(f, f.base_of(Hd.root_vals[v]));
      Instr P{
          .k = K::Phi, .dst = dst, .c = (int64_t)f.base_of(Hd.root_vals[v])};
      if (Hd.root_pred < b) {
        P.a = Hd.root_vals[v];
        P.t = Hd.root_pred;
        P.b2 = vals[v];
        P.f = b;
      } else {
        P.a = vals[v];
        P.t = b;
        P.b2 = Hd.root_vals[v];
        P.f = Hd.root_pred;
      }
      phis.push_back(P);
      res.push_back(dst);
    }
    if (Hd.root_pred == 0) // the self head: remember the param phis
      Hd.phi_vals.assign(res.begin(), res.begin() + nparams);
    scctco_prepend(f, Hd.entry, phis);
    if (Hd.cb == NONE) { // self: whole body (D=0 / H=1 excluded)
      for (uint32_t v = 0; v < nparams; ++v)
        scctco_rewrite(f, 2, f.nblocks(), NONE, Hd.root_vals[v], res[v]);
      scctco_rewrite(f, 2, f.nblocks(), NONE, Hd.trip_src, res[nparams]);
      T_cur = res[nparams];
    } else { // sibling copy: local rewrite (entry block excluded)
      for (uint32_t v = 0; v < nparams; ++v)
        scctco_rewrite(f, Hd.cb, Hd.ce, Hd.entry, Hd.root_vals[v], res[v]);
      scctco_rewrite(f, Hd.cb, Hd.ce, Hd.entry, Hd.trip_src, res[nparams]);
      // BUGFIX (bug 3, scctco sibling-copy rotation): the copy's ENTRY
      // block is real body code -- the sibling's first block (its
      // base-case test; for a one-block sibling even a guard site).
      // Its reads were left pinned to the RAW root-edge vregs, so on
      // every back edge that does not pass through the root edge's
      // defining block the entry test saw a STALE argument forever:
      // fa(8) -> fb(2) -> fb(0) kept testing the fa-entry value (2) at
      // fb's `n==0`, never hit the base case and spun to the trip TRAP.
      // The phis yield exactly the value that arrived on the taken
      // edge, so rewriting the entry block's NON-PHI reads to them is
      // exact. Phi instructions are skipped: their args must keep the
      // raw root-edge value (rewriting them would make each phi
      // self-referential on its root edge).
      for (uint32_t i = f.blocks[Hd.entry].begin; i < f.blocks[Hd.entry].end;
           ++i) {
        Instr &I = f.code[i];
        if (I.k == K::Phi)
          continue;
        for (uint32_t v = 0; v < nparams; ++v) {
          const uint32_t from = Hd.root_vals[v], to = res[v];
          if (I.a == from)
            I.a = to;
          if (I.b2 == from)
            I.b2 = to;
          if (I.c2 == from)
            I.c2 = to;
          for (uint32_t &u : I.args)
            if (u == from)
              u = to;
        }
        if (I.a == Hd.trip_src)
          I.a = res[nparams];
        if (I.b2 == Hd.trip_src)
          I.b2 = res[nparams];
        if (I.c2 == Hd.trip_src)
          I.c2 = res[nparams];
        for (uint32_t &u : I.args)
          if (u == Hd.trip_src)
            u = res[nparams];
      }
    }
    Hd.trip_phi = res[nparams];
    Hd.phis = true;
    Hd.tip_pred = b;
    Hd.tip_vals = vals;
  };

  // add a back edge (b, vals) to Hd, extending the merge chain as needed
  auto add_backedge = [&](ScctcoHead &Hd, uint32_t b,
                          const std::vector<uint32_t> &vals) {
    if (!Hd.phis) {
      promote(Hd, b, vals);
      scctco_retarget(f, b, NONE, Hd.entry);
      return;
    }
    // further edge: one more 2-pred chain block
    std::vector<Instr> phis;
    std::vector<uint32_t> res;
    for (uint32_t v = 0; v < NV; ++v) {
      uint32_t dst = new_anon_vreg(f, f.base_of(Hd.tip_vals[v]));
      Instr P{.k = K::Phi, .dst = dst, .c = (int64_t)f.base_of(Hd.tip_vals[v])};
      if (Hd.tip_pred < b) {
        P.a = Hd.tip_vals[v];
        P.t = Hd.tip_pred;
        P.b2 = vals[v];
        P.f = b;
      } else {
        P.a = vals[v];
        P.t = b;
        P.b2 = Hd.tip_vals[v];
        P.f = Hd.tip_pred;
      }
      phis.push_back(P);
      res.push_back(dst);
    }
    phis.push_back(Instr{.k = K::Br, .t = Hd.entry});
    const uint32_t M = append_block(phis);
    scctco_retarget(f, Hd.tip_pred, Hd.entry, M);
    scctco_retarget(f, b, NONE, M);
    // the entry phis' tip-side arg now arrives from M
    uint32_t v = 0;
    for (uint32_t i = f.blocks[Hd.entry].begin; i < f.blocks[Hd.entry].end;
         ++i) {
      Instr &P = f.code[i];
      if (P.k != K::Phi)
        break;
      if (P.t == Hd.tip_pred) {
        P.t = M;
        P.a = res[v];
      }
      if (P.f == Hd.tip_pred) {
        P.f = M;
        P.b2 = res[v];
      }
      ++v;
    }
    Hd.tip_pred = M;
    Hd.tip_vals = res;
  };

  // ---- 2. drain internal tail sites (one per outer round) ----------------
  // Hard iteration bound: each round converts exactly one tail site, and
  // the site count is bounded by the SCC's total code, so anything past
  // that means a construction bug (or a constexpr-evaluation pathology);
  // bailing leaves the remaining sites as ordinary calls, which is always
  // semantically safe -- just less optimized.
  const uint32_t iter_cap = 64 + 16 * (uint32_t)snap.size();
  uint32_t iters = 0;
  bool changed = false;
  for (bool progress = true; progress;) {
    if (++iters > iter_cap)
      return changed;
    progress = false;
    // find the first still-present internal tail call
    uint32_t sb = NONE, ci = NONE, callee = NONE;
    for (uint32_t b = 0; b < f.nblocks() && sb == NONE; ++b) {
      for (uint32_t i = f.blocks[b].begin; i < f.blocks[b].end; ++i) {
        const Instr &I = f.code[i];
        if (I.k != K::Call)
          continue;
        uint32_t g = (uint32_t)I.c;
        if (g >= nfn || snap_of[g] == NONE)
          continue; // not in this SCC
        if (!scctco_is_tail(f, b, i))
          return false; // cannot happen
        sb = b;
        ci = i;
        callee = g;
        break;
      }
    }
    if (sb == NONE)
      break;
    progress = true;
    changed = true;
    const std::vector<uint32_t> args = f.code[ci].args;
    // frame guard: r' = r - 1; trap if r' < 0 (the 513th virtual call)
    const uint32_t tripv = trip_read(sb);
    const uint32_t tp = new_anon_vreg(f, 0);
    const uint32_t cmp = new_anon_vreg(f, 0);
    if (trapb == NONE) {
      const uint32_t c1 = new_anon_vreg(f, 0), c0 = new_anon_vreg(f, 0),
                     dx = new_anon_vreg(f, 0);
      trapb = append_block(
          {Instr{.k = K::Const, .dst = c1, .c = 1},
           Instr{.k = K::Const, .dst = c0, .c = 0},
           Instr{.k = K::Bin, .b = B::Div, .dst = dx, .a = c1, .b2 = c0},
           Instr{.k = K::Ret, .a = dx}});
    }
    {
      std::vector<Instr> ns;
      for (uint32_t i = f.blocks[sb].begin; i < f.blocks[sb].end; ++i)
        if (i != ci && i + 1 != f.blocks[sb].end)
          ns.push_back(f.code[i]);
      ns.push_back(
          Instr{.k = K::Bin, .b = B::Sub, .dst = tp, .a = tripv, .b2 = k1});
      ns.push_back(
          Instr{.k = K::Bin, .b = B::Lt, .dst = cmp, .a = tp, .b2 = z0});
      ns.push_back(Instr{.k = K::Cbr, .a = cmp, .t = trapb, .f = NONE});
      scctco_set_block(f, sb, ns);
    }
    std::vector<uint32_t> vals = args;
    vals.push_back(tp);
    if (callee == self_fn) {
      add_backedge(self, sb, vals);
    } else if (heads[callee].entry == NONE) {
      // inline the sibling's PRE-collapse snapshot body once
      const Fn &G = snap[snap_of[callee]];
      const uint32_t nvG = G.nvars + (uint32_t)G.version_base.size();
      std::vector<uint32_t> vmap(nvG, NONE);
      for (uint32_t v = 0; v < nvG; ++v)
        vmap[v] = (v < G.nparams) ? args[v] : new_anon_vreg(f, G.base_of(v));
      auto mapv = [&](uint32_t v) -> uint32_t {
        return (v == NONE || v >= nvG) ? v : vmap[v];
      };
      const uint32_t nbG = G.nblocks();
      const uint32_t cb = f.nblocks();
      for (uint32_t g = 0; g < nbG; ++g) {
        std::vector<Instr> gb;
        for (uint32_t i = G.blocks[g].begin; i < G.blocks[g].end; ++i) {
          Instr I = G.code[i];
          if (I.dst != NONE)
            I.dst = mapv(I.dst);
          I.a = mapv(I.a);
          I.b2 = mapv(I.b2);
          if (I.c2 != NONE)
            I.c2 = mapv(I.c2);
          for (uint32_t &av : I.args)
            av = mapv(av);
          if (I.k == K::Phi) {
            if (I.t != NONE && I.t < nbG)
              I.t = cb + I.t;
            if (I.f != NONE && I.f < nbG)
              I.f = cb + I.f;
          } else if (I.k == K::Br) {
            if (I.t != NONE && I.t < nbG)
              I.t = cb + I.t;
          } else if (I.k == K::Cbr) {
            if (I.t != NONE && I.t < nbG)
              I.t = cb + I.t;
            if (I.f != NONE && I.f < nbG)
              I.f = cb + I.f;
          }
          gb.push_back(I);
        }
        append_block(gb);
      }
      const uint32_t ce = f.nblocks();
      heads[callee].entry = cb;
      heads[callee].root_pred = sb;
      heads[callee].root_vals = vals;
      heads[callee].cb = cb;
      heads[callee].ce = ce;
      heads[callee].trip_src = vals[nparams]; // the ARRIVING trip
      scctco_retarget(f, sb, NONE, cb);
    } else {
      add_backedge(heads[callee], sb, vals);
    }
  }

  // ---- 3b. raw-param rotation fixup --------------------------------------
  // A phi incoming arg that is still a RAW param vreg (arg < nparams)
  // means the tail call passed that parameter through unchanged: gcd's
  // `gcd(b, a % b)`, flat argument swaps, binary search's pass-through
  // bounds. Every BODY read of p_w was rewritten to the rotation phi
  // P_w -- but the phi args themselves were not: they sit in head blocks,
  // which the rewrites skip (the self entry) or which were appended after
  // the rewrite ran (merge-chain blocks). Pinned to the raw param, the
  // loop would feed the ORIGINAL argument back forever. On every edge
  // except the true entry (pred 0 = block D) the value that flows is the
  // CURRENT P_w, so patch those args to the phi. This is also harmless
  // for one-shot edges: on the first iteration P_w still holds the
  // original argument. When self never got a back edge (self.phis false,
  // nothing rotates anywhere), raw params are already correct.
  if (self.phis) {
    for (uint32_t b = 1; b < f.nblocks(); ++b)
      for (uint32_t i = f.blocks[b].begin; i < f.blocks[b].end; ++i) {
        Instr &P = f.code[i];
        if (P.k != K::Phi)
          continue;
        if (P.a != NONE && P.a < nparams && P.t != 0)
          P.a = self.phi_vals[P.a];
        if (P.b2 != NONE && P.b2 < nparams && P.f != 0)
          P.b2 = self.phi_vals[P.b2];
      }
  }

  scctco_resucc(f);
  if (dbg >= 2)
    print_fn(f, "[scctco] collapsed (pre-fold)", {});
  scctco_fold(f, 1, nparams, os, trapb, dbg);
  scctco_resucc(f);
  return changed;
}

// runtime-only tracer for the scctco driver (never reached on the
// constexpr path: cfg.debug is 0 there)
static void scctco_dbg_rt(uint8_t dbg, const char *what, long a, long b) {
  if (dbg < 1)
    return;
  std::printf("  [scctco] %s %ld %ld\n", what, a, b);
}

// ---- the SCC driver ---------------------------------------------------------

// Collapse every SCC of the call graph whose internal edges are all tail
// calls into loops; re-optimize each collapsed member (cheap tiers + lcf,
// unroll waits for the final round, exactly like the inliner's re-fold).
constexpr bool opt_scc_tailcall(std::vector<Fn> &fns,
                                std::vector<OptStats> &ostats,
                                const std::vector<std::string_view> &names,
                                PassCfg cfg) {
  const uint32_t n = (uint32_t)fns.size();
  if (n < 2)
    return false;
  CallGraph G = build_callgraph(fns);
  bool any = false;
  for (uint32_t s = 0; s < G.nscc; ++s) {
    const uint32_t b = G.scc_begin[s];
    const uint32_t e =
        (s + 1 < G.nscc) ? G.scc_begin[s + 1] : (uint32_t)G.members.size();
    if (e <= b)
      continue;
    std::vector<uint32_t> mem(G.members.begin() + b, G.members.begin() + e);
    if (mem.size() > 8)
      continue; // small SCCs only
    bool internal = false;
    for (uint32_t f : mem)
      for (ulen c : G.adj[f])
        if ((uint32_t)c < n && G.scc_of[(uint32_t)c] == s)
          internal = true;
    if (!internal)
      continue;
    // preconditions: every internal edge is a tail call with matching arg
    // count; total size bounded; main is never a member
    uint64_t total = 0;
    bool ok = true;
    for (uint32_t f : mem) {
      if (f == 0) {
        ok = false;
        break;
      }
      total += fns[f].code.size();
      for (uint32_t blk = 0; blk < fns[f].nblocks() && ok; ++blk)
        for (uint32_t i = fns[f].blocks[blk].begin; i < fns[f].blocks[blk].end;
             ++i) {
          const Instr &I = fns[f].code[i];
          if (I.k != K::Call)
            continue;
          uint32_t g = (uint32_t)I.c;
          if (g >= n || G.scc_of[g] != s)
            continue; // external edge
          if (!scctco_is_tail(fns[f], blk, i) ||
              I.args.size() != fns[g].nparams) {
            if (cfg.debug >= 1)
              std::printf("  [scctco] non-tail internal edge: f%u blk%u\n",
                          (unsigned)f, (unsigned)blk);
            ok = false;
            break;
          }
        }
    }
    if (!ok || total > 6000) {
      if (cfg.debug >= 1)
        scctco_dbg_rt(cfg.debug, "skip scc (ok,total)", (long)ok, (long)total);
      continue;
    }
    // pre-collapse snapshots + fn -> snapshot map
    std::vector<Fn> snap;
    std::vector<uint32_t> snap_of(n, NONE);
    for (uint32_t f : mem) {
      snap_of[f] = (uint32_t)snap.size();
      snap.push_back(fns[f]);
    }
    for (uint32_t f : mem) {
      // stats land on row 0 (<main>): collapsed members are routinely dead
      // (their only callers were inside the SCC), and eliminate_dead_fns
      // drops their stats rows with them
      if (!scctco_member(fns[f], f, snap, snap_of, n, ostats[0], cfg.debug)) {
        if (cfg.debug >= 1)
          scctco_dbg_rt(cfg.debug, "member refused", (long)f, 0);
        continue;
      }
      ostats[0].scctco++;
      any = true;
      // NOTE: no re-fold here (unlike the inliner's post-inline re-fold).
      // The collapsed body is left for the inliner rounds below (it is no
      // longer recursive, so single-use members inline into their callers,
      // which re-fold THOSE) and for optimize_prog's final full optimize,
      // which cleans every member. Skipping the intermediate full optimize
      // also keeps the constexpr (static_assert) build's retained
      // evaluation state -- two whole analysis+fixpoint rounds per SCC --
      // from bloating the compile-time memory budget.
    }
  }
  return any;
}

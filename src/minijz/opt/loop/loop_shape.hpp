#pragma once
// === loop_shape.hpp ========================================
// shared natural-loop recognition: the scan tables (const/def maps),
// the innermost-first header order, and the canonical loop shape that
// unroll / lcf / unswitch all require:
//   pre ----> h: [phis] instrs... cbr(cond) --body--> ... --> latch
//   h --exit--> X          (the header Cbr's exit arm leaves the loop)
// Extracted from the (previously triplicated) inline scans of unroll
// and lcf; the new passes (unswitch, lsimp) build on it too.
// ---- ALGORITHM / DATA STRUCTURES --------------------------------------
// loop_scan: one pass over the fn -- defi/defb per vreg; kc/kv when
// the def is a Const (marked from the LAST def: no ndefs table);
// headers = A.inner[h]==h, sorted nest-descending so the innermost
// loops come first. in_loop_flags: inL over all blocks from
// A.loops.range()[h] plus the header. header_phis: h's phis keyed
// to (pre, latch) as HPhi {dst, pre_arg, latch_arg}; ok iff EVERY
// phi carries both keys (h has exactly the two preds). loop_shape_
// of: unique outside pred (pre), unique latch, a Cbr terminator
// with one arm staying (body) and one leaving (X) -> LoopShape
// {pre, latch, X, body, ok}.
// ---- THEORY: why one shared canonical shape ---------------------------
// The shape -- one entry, one back edge, one header exit arm -- is
// the contract every restructuring pass needs: it keys the phis by
// (pre, latch), names the zero-iteration edge (h -> X), and gives
// the versioning paths their entry-rekey target. It replaced the
// triplicated inline scans of unroll and lcf. It is deliberately
// minimal: the only-exit-edge property (no breaks) is NOT required
// here -- callers that need it (unroll, lcf) check it themselves;
// unswitch allows internal exit edges. lcf keeps its own scans on
// purpose: LoopScan marks constants from the LAST def (lcf requires
// a single def), and loop_shape_of demands exactly ONE internal
// pred while lcf tolerates several back-edges (the last is the
// latch) -- see the NOTE in lcf_impl.hpp; unroll_impl keeps a
// local twin.
// ---- WHAT IT ENABLES (the consumers) ----------------------------------
// unswitch.hpp and rotate.hpp call loop_scan / in_loop_flags /
// loop_shape_of / header_phis directly; unswitch_shape.hpp and
// unswitch_bif.hpp build on them; lcf_impl.hpp and unroll_impl.hpp
// include it for HPhi; lcf_sim/lcf_rt/lsimp take the vocabulary.

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../analysis/an.hpp"
#include "../opt_generic.hpp"


// ---- the lcf/unroll shared gates (the ckpt-31 dedup wave) ---------------
// These three were verbatim twins in lcf_impl and unroll_impl (the unroll
// decomposition of ckpt 25 kept them local to stay byte-identical; the
// dedup wave moved them to the family vocabulary with the same guarantee).

// headers = blocks that are their own innermost loop, sorted
// nest-descending (innermost first). The pair-swap order is behavior:
// the passes visit headers in exactly this order.
constexpr std::vector<uint32_t> innermost_headers_by_nest(const An &A) {
  std::vector<uint32_t> headers{};
  for (uint32_t h = 0; h < A.n; ++h)
    if (A.inner[h] == h)
      headers.push_back(h);
  for (size_t i = 0; i + 1 < headers.size(); ++i)
    for (size_t j = i + 1; j < headers.size(); ++j)
      if (A.nest[headers[j]] > A.nest[headers[i]])
        std::swap(headers[i], headers[j]);
  return headers;
}

// the single-exit gate: the loop's ONLY exit edge must depart the
// header itself (an inner both-arms-out cbr or a break makes the loop
// untouchable for both lcf and unroll). Returns the unique exit
// target, or NONE when the loop does not qualify.
constexpr uint32_t loop_single_exit_target(const Fn &fn, uint32_t nb,
                                           const std::vector<char> &inL,
                                           uint32_t h) {
  std::vector<std::vector<uintlen_t>> rows(nb);
  for (uint32_t b = 0; b < nb; ++b) {
    if (fn.succ0(b) != NONE)
      rows[b].push_back(fn.succ0(b));
    if (fn.succ1(b) != NONE)
      rows[b].push_back(fn.succ1(b));
  }
  std::vector<opt_ns::exit_edge_t> ex{};
  if (!opt_ns::collect_exit_edges(rows, inL, opt_ns::none_v, opt_ns::none_v,
                                  opt_ns::none_v, opt_ns::none_v, ex))
    return NONE; // an inner both-arms-out cbr
  uint32_t X = NONE;
  for (const opt_ns::exit_edge_t &e : ex) {
    if (e.from != h)
      return NONE; // an inner exit edge (a break)
    if (X == NONE)
      X = uint32_t(e.to);
    else if (X != e.to)
      return NONE; // two different exits
  }
  return X; // NONE when the loop never exits
}

// K == 0 (the test provably false on entry): the header terminator
// becomes `br X` in place; the unreachable body dies in the next
// unreach/phi-degen round and the phis degenerate to their pre args.
constexpr void loop_zero_trip_exit(Fn &fn, uint32_t h, uint32_t X) {
  Instr &T = fn.code[fn.blocks[h].end - 1];
  T.k = K::Br;
  T.t = X;
  T.f = NONE;
  T.a = NONE;
  fn.succ[2 * h] = X;
  fn.succ[2 * h + 1] = NONE;
  fn.recompute_preds();
}

// ---- the scan tables (one pass over the fn) -------------------------
struct LoopScan {
  uint32_t nb{}, nv{};
  std::vector<char> kc{};               // vreg's def is a Const
  std::vector<val_t> kv{};              // ... and its value
  std::vector<uint32_t> defi{}, defb{}; // defining instr / block
  std::vector<uint32_t> headers{};      // loop headers, innermost first
};

constexpr LoopScan loop_scan(const Fn &fn, const An &A) {
  LoopScan S{};
  S.nb = fn.nblocks();
  S.nv = fn.nvars + (uint32_t)fn.version_base.size();
  S.kc.assign(S.nv, 0);
  S.kv.assign(S.nv, 0);
  S.defi.assign(S.nv, NONE);
  S.defb.assign(S.nv, NONE);
  for (uint32_t b = 0; b < S.nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.dst != NONE && I.dst < S.nv) {
        S.defi[I.dst] = i;
        S.defb[I.dst] = b;
        if (I.k == K::Const) {
          S.kc[I.dst] = 1;
          S.kv[I.dst] = I.c;
        }
      }
    }
  S.headers = innermost_headers_by_nest(A);
  return S;
}

// inL flags for one natural loop (header + body), sized for all blocks
constexpr void in_loop_flags(const Fn &fn, const An &A, uint32_t h,
                             std::vector<char> &inL) {
  inL.assign(fn.nblocks(), 0);
  for (ulen x : A.loops.range()[h])
    inL[(uint32_t)x] = 1;
  inL[h] = 1;
}

// one header phi, keyed to (pre, latch)
struct HPhi {
  uint32_t dst, pre_arg, latch_arg;
};

// all of h's phis keyed to (pre, latch). ok = every phi carries BOTH
// keys (the block has exactly the two preds); callers bail otherwise.
constexpr bool header_phis(const Fn &fn, uint32_t h, uint32_t pre,
                           uint32_t latch, std::vector<HPhi> &out) {
  out.clear();
  uint32_t nphi = 0;
  for (uint32_t i = fn.blocks[h].begin; i < fn.blocks[h].end; ++i) {
    const Instr &I = fn.code[i];
    if (I.k != K::Phi)
      break;
    ++nphi;
    uint32_t pa = (I.t == pre) ? I.a : ((I.f == pre) ? I.b2 : NONE);
    uint32_t la = (I.t == latch) ? I.a : ((I.f == latch) ? I.b2 : NONE);
    if (pa == NONE || la == NONE)
      continue;
    out.push_back({I.dst, pa, la});
  }
  return nphi && out.size() == nphi;
}

// the canonical loop shape around header h (see the file header).
// ok = recognized: unique outside pred (the preheader), unique latch,
// Cbr header terminator with exactly one arm leaving the loop to X and
// one arm staying inside (the body). The pred/arms CLASSIFICATION is
// the generic kernel (opt_ns::shape_ns::calculate_loop_shape -- pure
// CFG math, any pred-row shape); what stays here is the IR half: that
// the terminator IS a conditional branch, and the non-empty-block
// paranoia. The only-exit-edge property (no breaks) is NOT required
// here — callers that need it (unroll, lcf) check it themselves;
// unswitch allows internal exit edges.
struct LoopShape {
  uint32_t pre{NONE}, latch{NONE}, X{NONE}, body{NONE};
  bool ok{};
};

constexpr LoopShape loop_shape_of(const Fn &fn, const An &A,
                                  const std::vector<char> &inL, uint32_t h) {
  (void)A;
  LoopShape sh{};
  if (fn.blocks[h].begin == fn.blocks[h].end)
    return sh; // paranoia: empty blocks carry no terminator
  const Instr hterm = fn.code[fn.blocks[h].end - 1];
  if (hterm.k != K::Cbr)
    return sh; // the arms classification needs exactly two of them
  const opt_ns::loop_shape_t k = opt_ns::calculate_loop_shape(
      fn.preds[h], inL, (uintlen_t)hterm.t, (uintlen_t)hterm.f);
  if (!k.ok)
    return sh;
  sh.pre = (uint32_t)k.pre;
  sh.latch = (uint32_t)k.latch;
  sh.body = (uint32_t)k.body;
  sh.X = (uint32_t)k.X;
  sh.ok = true;
  return sh;
}

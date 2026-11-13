#pragma once
// === reassoc.hpp ========================================
// block-local linear-combination canonicalization (+,-,*c chains).
//
// ALGORITHM (opt_reassoc, three sweeps): (1) one pass over all
// blocks builds the per-vreg tables uses/ndefs/defi/defb; (2) a
// second pass fills kc/kv from single-def Const instrs; (3) a
// per-block rebuild into nc/nbl: `scope` marks only the current
// block, roots are single-def Add/Sub Bin instrs, lin_expand
// collects both operands (+1 / +-1), and when lin_cost(L) strictly
// beats the absorbed chain's cost the root is replaced by
// lin_emit's chain while the absorbed instrs ride `dropped`;
// fn.code/fn.blocks are replaced only when something changed.
//
// DATA STRUCTURES: the nv-sized scan tables (uses, ndefs, defi,
// defb, kc, kv); the block-local `scope` mask; `dropped`, the
// instr indices absorbed by earlier roots; nc/nbl, the rebuilt
// code and blocks; os.reassocs / os.reassoc_gone count rewrites.
//
// THEORY: read as x - y = x + (-1)*y, a chain of +, - and *c is a
// linear combination, and collecting it sorts terms by vreg, sums
// like coefficients and folds constants -- the canonical
// representative of the whole commutativity/associativity
// equivalence class. Chains that differ only in operand order or
// grouping re-emit as the SAME minimal sequence, the textual shape
// the dominator-scoped tier-0 cse can lock onto (the driver notes
// this canonicalization feeds LCF). Only single-use intermediates
// are absorbed, so no value moves past another use, and a rewrite
// commits only on a strict instruction-count reduction: progress
// guaranteed, the rebuilt chain is already minimal, rerun = no-op.
//
// DOWNSTREAM: canonical, cheaper chains that LCF then re-expands.

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "linform.hpp"

// --- 9b3. reassociation: block-local linear-combination canonicalization -----
// Chains of +, - and *-by-constant inside ONE basic block are collected into
// a canonical linear form and re-emitted minimally at the chain's root:
//   ((s + i*1) - 1 + i*2 - 2 + ... + i*9) - 9   ->   (s + i*45) - 45
// Only SINGLE-USE intermediates are absorbed, so no value ever moves past
// another use; a rewrite is committed only when it strictly reduces the
// instruction count (progress guaranteed; the rebuilt chain is already
// minimal, so a second run is a no-op).
constexpr bool opt_reassoc(Fn &fn, OptStats &os) {
  const uint32_t nb = fn.nblocks();
  if (!nb)
    return false;
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nv)
    return false;
  std::vector<uint32_t> uses(nv, 0), ndefs(nv, 0), defi(nv, NONE),
      defb(nv, NONE);
  std::vector<char> kc(nv, 0);
  std::vector<val_t> kv(nv, 0);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.dst != NONE && I.dst < nv) {
        ++ndefs[I.dst];
        defi[I.dst] = i;
        defb[I.dst] = b;
      }
      auto bump = [&](uint32_t v) {
        if (v != NONE && v < nv)
          ++uses[v];
      };
      bump(I.a);
      bump(I.b2);
      bump(I.c2);
      for (uint32_t v : I.args)
        bump(v);
    }
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k == K::Const && I.dst != NONE && I.dst < nv && ndefs[I.dst] == 1) {
        kc[I.dst] = 1;
        kv[I.dst] = I.c;
      }
    }
  bool changed = false;
  std::vector<Instr> nc{};
  nc.reserve(fn.code.size());
  std::vector<Block> nbl(nb);
  std::vector<char> scope(nb, 0);
  for (uint32_t b = 0; b < nb; ++b) {
    nbl[b].begin = (uint32_t)nc.size();
    scope.assign(nb, 0);
    scope[b] = 1;
    std::vector<uint32_t> dropped{}; // instrs absorbed by earlier roots
    auto was_dropped = [&](uint32_t i) {
      for (uint32_t x : dropped)
        if (x == i)
          return true;
      return false;
    };
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      if (was_dropped(i))
        continue;
      const Instr &I = fn.code[i];
      const bool root = I.k == K::Bin && (I.b == B::Add || I.b == B::Sub) &&
                        I.dst != NONE && I.dst < nv && ndefs[I.dst] == 1;
      if (root) {
        LinForm L{};
        std::vector<uint32_t> absr{};
        lin_expand(fn, nv, defi, defb, kc, kv, uses, ndefs, scope, i, L,
                   (val_t)1, I.a, absr);
        lin_expand(fn, nv, defi, defb, kc, kv, uses, ndefs, scope, i, L,
                   I.b == B::Add ? (val_t)1 : (val_t)-1, I.b2, absr);
        if (lin_cost(L) < (uint32_t)absr.size() + 1) {
          lin_emit(fn, nc, L, I.dst);
          for (uint32_t x : absr)
            dropped.push_back(x);
          os.reassocs++;
          os.reassoc_gone += (uint32_t)absr.size() + 1 - lin_cost(L);
          changed = true;
          continue; // root replaced by the materialized chain
        }
      }
      nc.push_back(I);
    }
    nbl[b].end = (uint32_t)nc.size();
  }
  if (changed) {
    fn.code = std::move(nc);
    fn.blocks = std::move(nbl);
  }
  return changed;
}

// --- 9a6. division/modulo by constant -> multiply + shift + bit ops ----------

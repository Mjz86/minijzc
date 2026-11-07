#pragma once
// === fold.hpp ========================================
// constant folding/propagation, branch folding, algebraic identities.

#include "minijz/base/pass_cfg.hpp"
#include "minijz/ir/instr.hpp"
#include "minijz/opt/analysis/an.hpp"

// ============================================================================
// 9. optimizer (all on SSA; analyses from the lib are recomputed on demand)
// ============================================================================

// --- 9a. constant folding / propagation / branch folding / identities -------
// known: vreg -> const value. iterates to fixpoint.
constexpr bool opt_constprop(Fn &fn, OptStats &os, bool is_main = false) {
  bool changed_any = false;
  for (int round = 0; round < 12; ++round) {
    bool changed = false;
    std::vector<char> known;
    std::vector<val_t> kval;
    auto is_known = [&](uint32_t v, val_t &out) -> bool {
      if (v == NONE) return false;
      if (v < known.size() && known[v]) { out = kval[v]; return true; }
      return false;
    };
    // collect consts
    known.assign(fn.nvars + fn.version_base.size(), 0);
    kval.assign(fn.nvars + fn.version_base.size(), 0);
    for (const Instr &I : fn.code)
      if (I.k == K::Const) { known[I.dst] = 1; kval[I.dst] = I.c; }
    for (uint32_t b = 0; b < fn.nblocks(); ++b) {
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        Instr &I = fn.code[i];
        val_t av{}, bv{};
        bool ka = is_known(I.a, av);
        bool kb = is_known(I.b2, bv);
        switch (I.k) {
        case K::Const: break;
        case K::Copy:
          if (ka) { I.k = K::Const; I.c = av; os.folds++; changed = true; }
          break;
        case K::Bin: {
          // algebraic identities (never introduce traps: the operand-defining
          // instrs still execute; we only recompute THIS instr's value)
          if (I.b == B::Add || I.b == B::Sub) {
            if (kb && bv == 0) { // x+0 = x ; x-0 = x
              I.k = K::Copy; os.copies++; changed = true; break;
            }
            if (ka && av == 0 && I.b == B::Add) { // 0+x = x
              I.k = K::Copy; I.a = I.b2; os.copies++; changed = true; break;
            }
          }
          if (I.b == B::Mul) {
            if ((ka && av == 0) || (kb && bv == 0)) {
              I.k = K::Const; I.c = 0; os.folds++; changed = true; break;
            }
            if (ka && av == 1) { I.k = K::Copy; I.a = I.b2; os.copies++; changed = true; break; }
            if (kb && bv == 1) { I.k = K::Copy; os.copies++; changed = true; break; }
          }
          if (I.b == B::Div || I.b == B::Mod) {
            if (kb && bv == 1) { // x/1 = x ; x%1 = 0 (divisor known: no trap)
              if (I.b == B::Div) { I.k = K::Copy; os.copies++; }
              else { I.k = K::Const; I.c = 0; os.folds++; }
              changed = true; break;
            }
          }
          if (I.b == B::And) { // x & 0 = 0 ; x & -1 = x
            if ((ka && av == 0) || (kb && bv == 0)) {
              I.k = K::Const; I.c = 0; os.folds++; changed = true; break;
            }
            if (ka && av == -1) { I.k = K::Copy; I.a = I.b2; os.copies++; changed = true; break; }
            if (kb && bv == -1) { I.k = K::Copy; os.copies++; changed = true; break; }
          }
          if (I.b == B::Or) { // x | 0 = x ; x | -1 = -1
            if (ka && av == -1) { I.k = K::Const; I.c = -1; os.folds++; changed = true; break; }
            if (kb && bv == -1) { I.k = K::Const; I.c = -1; os.folds++; changed = true; break; }
            if (ka && av == 0) { I.k = K::Copy; I.a = I.b2; os.copies++; changed = true; break; }
            if (kb && bv == 0) { I.k = K::Copy; os.copies++; changed = true; break; }
          }
          if (I.b == B::Xor) { // x ^ 0 = x (x ^ x handled below)
            if (ka && av == 0) { I.k = K::Copy; I.a = I.b2; os.copies++; changed = true; break; }
            if (kb && bv == 0) { I.k = K::Copy; os.copies++; changed = true; break; }
          }
          if (I.b == B::Shl || I.b == B::LShr || I.b == B::AShr) {
            if (kb && (bv & 63) == 0) { // shift by 0: identity
              I.k = K::Copy; os.copies++; changed = true; break;
            }
          }
          if (I.b == B::MulHi) {
            if ((ka && av == 0) || (kb && bv == 0)) {
              I.k = K::Const; I.c = 0; os.folds++; changed = true; break;
            }
          }
          if (I.a != NONE && I.a == I.b2) { // same vreg both sides (no traps)
            int which = -1; // -1 keep, 0 -> Const 0, 1 -> Const 1
            switch (I.b) {
            case B::Sub: case B::Lt: case B::Gt: which = 0; break;
            case B::Eq: case B::Le: case B::Ge: which = 1; break;
            case B::Xor: which = 0; break; // x ^ x = 0
            default: break; // Add/Mul/Div/Mod/Ne handled elsewhere or unsafe
            }
            if (I.b == B::Ne) which = 0; // x != x is always false
            if (which >= 0) {
              I.k = K::Const; I.c = which;
              os.folds++; changed = true; break;
            }
          }
          if (ka && kb) {
            Rv r{};
            switch (I.b) {
            case B::Add: r = w_add(av, bv); break;
            case B::Sub: r = w_sub(av, bv); break;
            case B::Mul: r = w_mul(av, bv); break;
            case B::Div: r = w_div(av, bv); break;
            case B::Mod: r = w_mod(av, bv); break;
            case B::Lt: r = {av < bv, false}; break;
            case B::Le: r = {av <= bv, false}; break;
            case B::Gt: r = {av > bv, false}; break;
            case B::Ge: r = {av >= bv, false}; break;
            case B::Eq: r = {av == bv, false}; break;
            case B::Ne: r = {av != bv, false}; break;
            case B::And: r = w_and(av, bv); break;
            case B::Or: r = w_or(av, bv); break;
            case B::Xor: r = w_xor(av, bv); break;
            case B::Shl: r = w_shl(av, bv); break;
            case B::LShr: r = w_lshr(av, bv); break;
            case B::AShr: r = w_ashr(av, bv); break;
            case B::MulHi: r = w_mulhi(av, bv); break;
            }
            if (!r.trap) { // trapping folds stay unfolded: runtime trap preserved
              I.k = K::Const; I.c = r.v; os.folds++; changed = true;
            }
          }
          break;
        }
        case K::Phi: {
          // all args the same const -> const ; all args identical -> copy
          uint32_t x = I.a, y = I.b2;
          if (x != NONE && y != NONE) {
            if (x == y) { // both same vreg: copy
              I.k = K::Copy; I.a = x; os.copies++; changed = true; break;
            }
            val_t xv{}, yv{};
            if (is_known(x, xv) && is_known(y, yv) && xv == yv) {
              I.k = K::Const; I.c = xv; os.folds++; changed = true; break;
            }
          }
          break;
        }
        case K::Depth: { // main's frame count is provably 1 everywhere
          if (is_main) {
            I.k = K::Const;
            I.c = 1;
            os.folds++;
            changed = true;
          }
          break;
        }
        case K::Cbr: {
          if (I.t == I.f && I.t != NONE) { // both arms identical: unconditional
            I.k = K::Br;
            I.f = NONE;
            fn.succ[2 * b + 1] = NONE;
            os.cbr_same++;
            changed = true;
            break;
          }
          if (ka) { // fold branch
            I.k = K::Br;
            I.t = (av != 0) ? I.t : I.f;
            I.f = NONE;
            fn.succ[2 * b] = I.t;
            fn.succ[2 * b + 1] = NONE;
            os.brs_folded++; changed = true;
          }
          break;
        }
        default: break;
        }
      }
    }
    if (!changed) break;
    changed_any = true;
  }
  return changed_any;
}

// --- 9a2. copy propagation ----------------------------------------------------

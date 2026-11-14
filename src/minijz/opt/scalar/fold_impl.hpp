#pragma once
// === fold_impl.hpp ========================================
// the FOLD SUPPORT layer, split out of fold.hpp (the code-quality
// continuation, round four -- the cdfold/lcf/unroll treatment):
// the pass context (the def-index map + the per-round known-const
// tables), the compare-def lookup and the predicate negation, the
// five compare-domain facts, the double-negation identity, the nine
// algebraic-identity families, the same-vreg facts, and the
// per-kind case handlers. The slim driver -- the 12-round fixpoint
// loop -- stays in fold.hpp.
//
// ALGORITHM (the phase list; every order below is the original's):
//   def-index  one pre-pass scan: vreg -> the instr index of its
//              def, the LAST def winning (SSA: normally the only
//              one; the guard is defensive, the original's). Built
//              ONCE at pass entry -- this pass rewrites instrs IN
//              PLACE (never moves or deletes them), so the indices
//              stay valid through all 12 rounds.
//   round      rebuild known/kval from every Const def in the fn
//              (a Const MADE by round k is visible to round k+1:
//              that is the fixpoint), then scan blocks in order,
//              instrs in order within each block.
//   per-instr  Copy of a known const -> Const; Bin (the cascade
//              below); Phi (both args the same vreg -> Copy, both
//              the same const -> Const); Depth -> Const 1 in main;
//              Cbr (identical arms -> Br; known cond -> Br toward
//              the taken arm). First match wins: the original
//              `break`s out of the case; the helpers return true.
//   Bin cascade: cmpfacts -> double-neg -> add/sub -> mul ->
//              div/mod -> and -> or -> xor -> shifts -> mulhi ->
//              same-vreg -> full-const (w_bin).
//
// THEORY (the fine print; the pass-level theory is fold.hpp's
// header):
//   * known/kval is the classic local constant environment keyed
//     by SSA NAME: one def per vreg means a Const def binds the
//     value every use sees, with no flow analysis -- the DIRECT
//     facts tier. Values needing control-flow reasoning (condi-
//     tional / loop-carried constants) are SCCP's tier, not this.
//   * defi powers the two-def identities (0-(0-x), the compare
//     facts). fold_cmp_def re-checks D.k == K::Bin at USE time, so
//     a def folded to Const earlier in THIS round no longer reads
//     as a compare -- and the const path picks it up through
//     known/kval instead. Both behaviors are the original's.
//   * TRAP DISCIPLINE. A Bin with both operands known but a
//     trapping wrapped evaluation (w_div/w_mod: divisor 0, or -1
//     with INT64_MIN) KEEPS the op -- traps are observable, so
//     they stay for the runtime. The identity rewrites never
//     introduce traps: they fire on operand PATTERNS that cannot
//     trap (x+0, x*0, x/1, ...) or on x-x/x^x shapes whose
//     operand-defining instrs still execute (only THIS instr's
//     recomputation is removed).
//   * the compare-domain facts exploit a compare result being
//     EXACTLY 0/1: (cmp)&1 and (cmp)%2 keep the compare (Mod by 2
//     on a 0/1 dividend cannot trap), (cmp)&~1 and (cmp)>>k
//     (k >= 1, either shift flavor) are exactly 0, and (cmp)^1 is
//     the NEGATED compare -- predicate inversion (Lt<->Ge, Le<->Gt,
//     Eq<->Ne): how a flipped test folds away with no NOT op.
//   * the full-const evaluator is the SHARED w_bin (instr.hpp) --
//     its table is identical to the former inline switch (the same
//     w_* per op, the same {cmp,false} results; the pass-level
//     theory in fold.hpp documents the wrapped-arithmetic model).
//     The one textual difference -- w_bin's unreachable trailing
//     `return {0, true}` vs the original's value-initialized
//     `Rv r{}` -- is dead in both: the B enum has exactly the 18
//     covered values, so every reachable path assigns r the same.
//   * the 12-round cap is belt and braces: each round at minimum
//     propagates every use of a newly-made Const, so local
//     propagation converges in a few rounds; the cap bounds a
//     pathological phi chain without changing any terminating
//     fixpoint. The return value feeds the pipeline driver's
//     fixpoint (os.iters), which is why the exact round structure
//     -- rebuild, scan, quiet-round exit -- is preserved to the
//     character.
//
// Byte-identical contract: the cascade order, each family's
// sub-order (annihilators before identities, ka before kb), the
// per-round table rebuild, the def-index scan bounds, and every
// counter increment are exactly the original's -- the fuzz
// fixtures, the demo tour, the opt reports and the register audit
// regenerate unchanged.

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"

// ---- the pass context -----------------------------------------------------
struct FoldCtx {  // what the former monolith held in pass-wide locals
  Fn &fn;         // the function under rewrite
  OptStats &os;   // the pass's counters
  uint32_t nv0{}; // pass-entry vreg universe
  std::vector<uint32_t> defi{}; // vreg -> its def's instr index (LAST wins)
  std::vector<char> known{};    // per-round: vreg currently a known Const
  std::vector<val_t> kval{};    // per-round: that Const's value
};

// vreg -> the instr index of its def; the LAST def wins (SSA makes
// it the only one; the dst < nv0 guard is the original's, kept
// defensive). Sizing/bounds are the pass-entry nv0, exactly as the
// original scanned.
constexpr std::vector<uint32_t> fold_def_index(const Fn &fn, uint32_t nv0) {
  std::vector<uint32_t> defi(nv0, NONE);
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
      if (fn.code[i].dst != NONE && fn.code[i].dst < nv0)
        defi[fn.code[i].dst] = i;
  return defi;
}

// the per-round rebuild: every live Const def (re-)enters the
// tables. A Const made by round k is visible to round k+1 -- that
// is the fixpoint. Consts made DURING a round wait for the rebuild
// (the original never mutates the tables mid-scan).
constexpr void fold_reset_tables(FoldCtx &S) {
  S.known.assign(S.fn.nvars + S.fn.version_base.size(), 0);
  S.kval.assign(S.fn.nvars + S.fn.version_base.size(), 0);
  for (const Instr &I : S.fn.code)
    if (I.k == K::Const) {
      S.known[I.dst] = 1;
      S.kval[I.dst] = I.c;
    }
}

// is vreg v a currently-known constant? (out = the value)
constexpr bool fold_is_known(const FoldCtx &S, uint32_t v, val_t &out) {
  if (v == NONE)
    return false;
  if (v < S.known.size() && S.known[v]) {
    out = S.kval[v];
    return true;
  }
  return false;
}

// ---- the compare-domain facts (a compare result is EXACTLY 0/1) -----------
// Is vreg v EXACTLY one compare result? Out: the compare's operands
// (ca, cb) and predicate (cop). Re-checks D.k == K::Bin at USE
// time -- a def folded to Const this round no longer qualifies (the
// original's behavior; the const path picks such values up instead).
constexpr bool fold_cmp_def(const FoldCtx &S, uint32_t v, uint32_t &ca,
                            uint32_t &cb, B &cop) {
  if (v == NONE || v >= S.nv0 || S.defi[v] == NONE)
    return false;
  const Instr &D = S.fn.code[S.defi[v]];
  if (D.k != K::Bin)
    return false;
  switch (D.b) {
  case B::Lt:
  case B::Le:
  case B::Gt:
  case B::Ge:
  case B::Eq:
  case B::Ne:
    ca = D.a;
    cb = D.b2;
    cop = D.b;
    return true;
  default:
    return false;
  }
}

// the negation of a comparison (predicate inversion); non-compares
// pass through unchanged -- the original's default arm.
constexpr B fold_inv_cmp(B b) {
  switch (b) {
  case B::Lt:
    return B::Ge;
  case B::Le:
    return B::Gt;
  case B::Gt:
    return B::Le;
  case B::Ge:
    return B::Lt;
  case B::Eq:
    return B::Ne;
  case B::Ne:
    return B::Eq;
  default:
    return b;
  }
}

// the facts where the compare itself survives: (cmp)&1 = cmp and
// (cmp)%2 = cmp (no trap: a 0/1 dividend cannot trap on modulus 2).
constexpr bool fold_cmp_keep(Instr &I, OptStats &os, val_t bv) {
  if ((I.b == B::And && bv == 1) || (I.b == B::Mod && bv == 2)) {
    I.k = K::Copy; // Copy of I.a: the compare's own vreg (untouched)
    os.copies++;
    os.cmpfacts++;
    return true;
  }
  return false;
}

// the facts where the result is exactly 0: (cmp)&~1 = 0 and
// (cmp)>>k = 0 (k >= 1, either shift flavor: 0/1 shifted right
// dies; the shift count operand is known, so no shift-trap either).
constexpr bool fold_cmp_zero(Instr &I, OptStats &os, val_t bv) {
  if ((I.b == B::And && bv == -2) ||
      ((I.b == B::LShr || I.b == B::AShr) && bv >= 1)) {
    I.k = K::Const;
    I.c = 0;
    os.folds++;
    os.cmpfacts++;
    return true;
  }
  return false;
}

// the negation fact: (cmp)^1 = NOT(cmp) -- rewrite to the inverted
// predicate on the compare's own operands. The counters: cmpfacts
// only (no copies/folds -- the instr stays a Bin).
constexpr bool fold_cmp_not(Instr &I, OptStats &os, val_t bv, uint32_t ca,
                            uint32_t cb, B cop) {
  if (I.b != B::Xor || bv != 1)
    return false;
  I.b = fold_inv_cmp(cop); // the inverted predicate, same operands
  I.a = ca;
  I.b2 = cb;
  os.cmpfacts++;
  return true;
}

// the family gate + dispatch: b-side a known const AND a-side
// exactly a compare result -> the 0/1-domain facts, in the
// original's order (the guards are mutually exclusive, so keep ->
// zero -> not preserves the original's five-way order exactly).
constexpr bool fold_bin_cmpfacts(FoldCtx &S, Instr &I, bool kb, val_t bv) {
  if (!kb)
    return false;
  uint32_t ca = NONE, cb = NONE;
  B cop{};
  if (!fold_cmp_def(S, I.a, ca, cb, cop))
    return false;
  if (fold_cmp_keep(I, S.os, bv))
    return true;
  if (fold_cmp_zero(I, S.os, bv))
    return true;
  return fold_cmp_not(I, S.os, bv, ca, cb, cop);
}

// ---- the two-def identity: 0 - (0 - x) = x -------------------------------
// I is `0 - y` (a known-0 left operand); y's def is `0 - x` with
// x's left operand known 0 -> Copy of x. The I.b2 != I.a guard
// blocks `0 - (0 - 0)` reading itself as its own inner def.
constexpr bool fold_bin_double_neg(FoldCtx &S, Instr &I, bool ka, val_t av) {
  if (I.b != B::Sub || !ka || av != 0 || I.b2 == NONE || I.b2 >= S.nv0 ||
      S.defi[I.b2] == NONE || I.b2 == I.a)
    return false;
  const Instr &D = S.fn.code[S.defi[I.b2]];
  val_t zv{};
  if (D.k != K::Bin || D.b != B::Sub || D.a == NONE || D.a >= S.nv0 ||
      !fold_is_known(S, D.a, zv) || zv != 0)
    return false;
  I.k = K::Copy; // Copy of D.b2: the original x
  I.a = D.b2;
  S.os.copies++;
  return true;
}

// ---- the algebraic identities (never introduce traps) ---------------------
// x+0 = x ; x-0 = x ; 0+x = x
constexpr bool fold_bin_addsub(Instr &I, OptStats &os, bool ka, bool kb,
                               val_t av, val_t bv) {
  if (I.b != B::Add && I.b != B::Sub)
    return false;
  if (kb && bv == 0) { // x+0 = x ; x-0 = x
    I.k = K::Copy;
    os.copies++;
    return true;
  }
  if (ka && av == 0 && I.b == B::Add) { // 0+x = x
    I.k = K::Copy;
    I.a = I.b2;
    os.copies++;
    return true;
  }
  return false;
}

// x*0 = 0*x = 0 (annihilator) ; 1*x = x ; x*1 = x
constexpr bool fold_bin_mul(Instr &I, OptStats &os, bool ka, bool kb, val_t av,
                            val_t bv) {
  if (I.b != B::Mul)
    return false;
  if ((ka && av == 0) || (kb && bv == 0)) { // annihilator -> 0
    I.k = K::Const;
    I.c = 0;
    os.folds++;
    return true;
  }
  if (ka && av == 1) { // 1*x = x
    I.k = K::Copy;
    I.a = I.b2;
    os.copies++;
    return true;
  }
  if (kb && bv == 1) { // x*1 = x
    I.k = K::Copy;
    os.copies++;
    return true;
  }
  return false;
}

// x/1 = x ; x%1 = 0 (divisor known 1: provably no trap)
constexpr bool fold_bin_divmod(Instr &I, OptStats &os, bool kb, val_t bv) {
  if ((I.b != B::Div && I.b != B::Mod) || !kb || bv != 1)
    return false;
  if (I.b == B::Div) { // x/1 = x
    I.k = K::Copy;
    os.copies++;
  } else { // x%1 = 0
    I.k = K::Const;
    I.c = 0;
    os.folds++;
  }
  return true;
}

// x&0 = 0&x = 0 ; x&-1 = x ; -1&x = x
constexpr bool fold_bin_and(Instr &I, OptStats &os, bool ka, bool kb, val_t av,
                            val_t bv) {
  if (I.b != B::And)
    return false;
  if ((ka && av == 0) || (kb && bv == 0)) { // annihilator -> 0
    I.k = K::Const;
    I.c = 0;
    os.folds++;
    return true;
  }
  if (ka && av == -1) { // -1&x = x (all-ones mask)
    I.k = K::Copy;
    I.a = I.b2;
    os.copies++;
    return true;
  }
  if (kb && bv == -1) { // x&-1 = x
    I.k = K::Copy;
    os.copies++;
    return true;
  }
  return false;
}

// x|-1 = -1|x = -1 ; 0|x = x ; x|0 = x
constexpr bool fold_bin_or(Instr &I, OptStats &os, bool ka, bool kb, val_t av,
                           val_t bv) {
  if (I.b != B::Or)
    return false;
  if (ka && av == -1) { // -1|x = -1
    I.k = K::Const;
    I.c = -1;
    os.folds++;
    return true;
  }
  if (kb && bv == -1) { // x|-1 = -1
    I.k = K::Const;
    I.c = -1;
    os.folds++;
    return true;
  }
  if (ka && av == 0) { // 0|x = x
    I.k = K::Copy;
    I.a = I.b2;
    os.copies++;
    return true;
  }
  if (kb && bv == 0) { // x|0 = x
    I.k = K::Copy;
    os.copies++;
    return true;
  }
  return false;
}

// x^0 = 0^x = x (x^x is below, in the same-vreg facts)
constexpr bool fold_bin_xor(Instr &I, OptStats &os, bool ka, bool kb, val_t av,
                            val_t bv) {
  if (I.b != B::Xor)
    return false;
  if (ka && av == 0) { // 0^x = x
    I.k = K::Copy;
    I.a = I.b2;
    os.copies++;
    return true;
  }
  if (kb && bv == 0) { // x^0 = x
    I.k = K::Copy;
    os.copies++;
    return true;
  }
  return false;
}

// shift by 0: identity (x<<0 = x, x>>0 = x, both logical and
// arithmetic right -- the count is taken & 63 like the emitters).
constexpr bool fold_bin_shift(Instr &I, OptStats &os, bool kb, val_t bv) {
  if ((I.b == B::Shl || I.b == B::LShr || I.b == B::AShr) && kb &&
      (bv & 63) == 0) {
    I.k = K::Copy;
    os.copies++;
    return true;
  }
  return false;
}

// mulhi(x, 0) = mulhi(0, x) = 0 (the high half of a 128-bit
// product with a zero operand is zero).
constexpr bool fold_bin_mulhi(Instr &I, OptStats &os, bool ka, bool kb,
                              val_t av, val_t bv) {
  if (I.b != B::MulHi)
    return false;
  if ((ka && av == 0) || (kb && bv == 0)) {
    I.k = K::Const;
    I.c = 0;
    os.folds++;
    return true;
  }
  return false;
}

// ---- the same-vreg facts (x on both sides; no traps possible) ------------
// x&x = x ; x|x = x ; x-x = 0 ; x<x = 0 ; x>x = 0 ; x==x = 1 ;
// x<=x = 1 ; x>=x = 1 ; x^x = 0 ; x!=x = 0. Add/Mul/Div/Mod are
// deliberately NOT folded: x+x and x*x change the value, and x/x or
// x%x can TRAP (x == 0) -- the original's table, kept exactly.
constexpr bool fold_bin_same_vreg(Instr &I, OptStats &os) {
  if (I.a == NONE || I.a != I.b2)
    return false;
  if (I.b == B::And || I.b == B::Or) { // x&x = x ; x|x = x
    I.k = K::Copy;
    os.copies++;
    return true;
  }
  int which = -1; // -1 keep, 0 -> Const 0, 1 -> Const 1
  switch (I.b) {
  case B::Sub:
  case B::Lt:
  case B::Gt:
    which = 0;
    break;
  case B::Eq:
  case B::Le:
  case B::Ge:
    which = 1;
    break;
  case B::Xor:
    which = 0; // x ^ x = 0
    break;
  default:
    break; // Add/Mul/Div/Mod/Ne handled elsewhere or unsafe
  }
  if (I.b == B::Ne)
    which = 0; // x != x is always false
  if (which < 0)
    return false;
  I.k = K::Const;
  I.c = which;
  os.folds++;
  return true;
}

// ---- the full-const fold (the shared wrapped evaluator) ------------------
// Both operands known -> the wrapped value, unless it traps. The
// evaluator is instr.hpp's w_bin -- the SAME table the former
// inline switch spelled out (see the header's equivalence note).
constexpr bool fold_bin_consts(Instr &I, OptStats &os, bool ka, bool kb,
                               val_t av, val_t bv) {
  if (!ka || !kb)
    return false;
  Rv r = w_bin(I.b, av, bv);
  if (r.trap)
    return false; // trapping folds stay unfolded: the runtime trap
                  // is observable
  I.k = K::Const;
  I.c = r.v;
  os.folds++;
  return true;
}

// ---- the Bin cascade (first match wins; the original's order) ------------
constexpr bool fold_bin(FoldCtx &S, Instr &I, bool ka, bool kb, val_t av,
                        val_t bv) {
  if (fold_bin_cmpfacts(S, I, kb, bv))
    return true; // t is exactly 0/1 (a compare result)
  if (fold_bin_double_neg(S, I, ka, av))
    return true; // 0 - (0 - x) = x
  if (fold_bin_addsub(I, S.os, ka, kb, av, bv))
    return true;
  if (fold_bin_mul(I, S.os, ka, kb, av, bv))
    return true;
  if (fold_bin_divmod(I, S.os, kb, bv))
    return true;
  if (fold_bin_and(I, S.os, ka, kb, av, bv))
    return true;
  if (fold_bin_or(I, S.os, ka, kb, av, bv))
    return true;
  if (fold_bin_xor(I, S.os, ka, kb, av, bv))
    return true;
  if (fold_bin_shift(I, S.os, kb, bv))
    return true;
  if (fold_bin_mulhi(I, S.os, ka, kb, av, bv))
    return true;
  if (fold_bin_same_vreg(I, S.os))
    return true;                                   // x op x
  return fold_bin_consts(I, S.os, ka, kb, av, bv); // both known
}

// ---- the per-kind handlers ------------------------------------------------
// Copy of a known const -> Const (os.folds, not os.copies: it is
// the copy itself that disappears).
constexpr bool fold_copy(Instr &I, OptStats &os, bool ka, val_t av) {
  if (!ka)
    return false;
  I.k = K::Const;
  I.c = av;
  os.folds++;
  return true;
}

// Phi: both args the same vreg -> Copy; both args the same known
// const -> Const. (This IR's phis carry two args; t/f are the pred
// keys. A pred-keyed merge the two arms disagree on stays a phi.)
constexpr bool fold_phi(FoldCtx &S, Instr &I) {
  uint32_t x = I.a, y = I.b2;
  if (x == NONE || y == NONE)
    return false;
  if (x == y) { // both arms carry the same vreg: copy
    I.k = K::Copy;
    I.a = x;
    S.os.copies++;
    return true;
  }
  val_t xv{}, yv{};
  if (!fold_is_known(S, x, xv) || !fold_is_known(S, y, yv) || xv != yv)
    return false;
  I.k = K::Const; // both arms the same constant
  I.c = xv;
  S.os.folds++;
  return true;
}

// Depth: main's frame count is provably 1 everywhere in main (the
// marker exists so tail-call elimination can materialize recursion
// budgets; inside main there is no caller below it).
constexpr bool fold_depth(Instr &I, OptStats &os, bool is_main) {
  if (!is_main)
    return false;
  I.k = K::Const;
  I.c = 1;
  os.folds++;
  return true;
}

// Cbr: identical arms -> Br (the dead arm becomes unreachable code
// for the unreach pass); a known cond -> Br toward the taken arm.
// fn.succ is rewritten in the SAME order the terminator fields are
// (the t side is chosen BEFORE f is nulled).
constexpr bool fold_cbr(Fn &fn, Instr &I, OptStats &os, uint32_t b, bool ka,
                        val_t av) {
  if (I.t == I.f && I.t != NONE) { // both arms identical
    I.k = K::Br;
    I.f = NONE;
    fn.succ[2 * b + 1] = NONE;
    os.cbr_same++;
    return true;
  }
  if (ka) { // fold the branch toward the taken arm
    I.k = K::Br;
    I.t = (av != 0) ? I.t : I.f;
    I.f = NONE;
    fn.succ[2 * b] = I.t;
    fn.succ[2 * b + 1] = NONE;
    os.brs_folded++;
    return true;
  }
  return false;
}

// ---- the dispatch, the round, the context ---------------------------------
// One instruction, the original's switch. ka/kb/av/bv are looked
// up BEFORE the switch (every kind sees the same operand facts,
// whether it uses them or not -- the original's shape).
constexpr bool fold_instr(FoldCtx &S, Instr &I, uint32_t b, bool is_main) {
  val_t av{}, bv{};
  bool ka = fold_is_known(S, I.a, av);
  bool kb = fold_is_known(S, I.b2, bv);
  switch (I.k) {
  case K::Const:
    return false; // the tables' own source: nothing to do
  case K::Copy:
    return fold_copy(I, S.os, ka, av);
  case K::Bin:
    return fold_bin(S, I, ka, kb, av, bv);
  case K::Phi:
    return fold_phi(S, I);
  case K::Depth:
    return fold_depth(I, S.os, is_main);
  case K::Cbr:
    return fold_cbr(S.fn, I, S.os, b, ka, av);
  default:
    return false; // Br/Print/Exit/Alloc/Load/Store/Len/Input/Call/
                  // Ret/Bkpt: nothing local to fold here
  }
}

// One fixpoint round: rebuild the const tables (a Const made by
// the previous round is now visible), then scan everything, blocks
// in order, instrs in order within each block. `|=` never short-
// circuits: every instr is visited exactly as the original did.
constexpr bool fold_round(FoldCtx &S, bool is_main) {
  fold_reset_tables(S);
  bool changed = false;
  for (uint32_t b = 0; b < S.fn.nblocks(); ++b)
    for (uint32_t i = S.fn.blocks[b].begin; i < S.fn.blocks[b].end; ++i)
      changed |= fold_instr(S, S.fn.code[i], b, is_main);
  return changed;
}

// The pass context: the def-index ONCE at pass entry (instrs are
// only rewritten in place, so the indices stay valid all rounds),
// the known-const tables per round.
constexpr FoldCtx fold_ctx(Fn &fn, OptStats &os) {
  FoldCtx S{fn, os};
  S.nv0 = fn.nvars + (uint32_t)fn.version_base.size();
  S.defi = fold_def_index(fn, S.nv0);
  return S;
}

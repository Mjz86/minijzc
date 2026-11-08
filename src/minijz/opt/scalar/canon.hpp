#pragma once
// === canon.hpp ========================================
// canonicalization: comparison direction, commutative order,
// NOT-of-compare combination (notcomb).

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../analysis/an.hpp"

// --- 9a4. canonicalization
// ---------------------------------------------------- Puts every pure Bin into
// ONE canonical shape so equivalent computations become identical (which is
// what makes CSE hit) and identities match:
//   * comparisons: `a > b` -> `b < a`, `a >= b` -> `b <= a`  (one direction)
//   * commutative ops (+ * == !=): known-constant operand on the RIGHT,
//     otherwise the smaller vreg id first — exactly the key order the CSE
//     pass uses, so a canonical instruction always enters the CSE table in
//     canonical order (`7*x` and `x*7` unify; `a>b` and `b<a` unify).
// Comparisons never trap and swapping commutative operands preserves
// wrapping two's-complement results, so both rewrites are always sound.
// The pass is idempotent and strictly progress-making (no oscillation).
constexpr bool opt_canon(Fn &fn, OptStats &os) {
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  std::vector<char> kc(nv, 0);
  std::vector<val_t> kv(nv, 0);
  for (const Instr &I : fn.code)
    if (I.k == K::Const && I.dst != NONE && I.dst < nv) {
      kc[I.dst] = 1;
      kv[I.dst] = I.c;
    }
  auto key = [&](uint32_t v) -> uint64_t {
    if (v == NONE)
      return 0;
    if (v < nv && kc[v])
      return ((uint64_t)1 << 63) ^ (uint64_t)kv[v];
    return (uint64_t)v;
  };
  bool changed = false;
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k != K::Bin)
        continue;
      if (I.b == B::Gt || I.b == B::Ge) { // canonical comparison direction
        std::swap(I.a, I.b2);
        I.b = (I.b == B::Gt) ? B::Lt : B::Le;
        os.canon_cmps++;
        changed = true;
      } else if (I.b == B::Add || I.b == B::Mul || I.b == B::Eq ||
                 I.b == B::Ne || I.b == B::And || I.b == B::Or ||
                 I.b == B::Xor ||
                 I.b == B::MulHi) { // commutative canonical order
        if (key(I.b2) < key(I.a)) {
          std::swap(I.a, I.b2);
          os.canon_swaps++;
          changed = true;
        }
      }
    }
  // ---- NOT-of-compare combination ---------------------------------------
  // A comparison result is exactly 0/1, so testing it against 0/1 is just
  // the comparison (or its negation):
  //     t2 = (t1 == 0)  <->  t2 = <inverted t1 compare>   (that's !t1)
  //     t2 = (t1 != 0)  <->  t2 = <t1 compare, new dst>
  // (== 1 / != 1 likewise). Combined with branch fusion this is what turns
  // `if (!((a != b))) goto L;` into `if (a == b) goto L;` -- the negation of
  // a compare is trivially combinable into the compare itself.
  {
    std::vector<uint32_t> defi(nv, NONE), ndefs(nv, 0), uses(nv, 0);
    for (uint32_t i = 0; i < fn.code.size(); ++i) {
      const Instr &I = fn.code[i];
      auto bump = [&](uint32_t v) {
        if (v != NONE && v < nv)
          ++uses[v];
      };
      bump(I.a);
      bump(I.b2);
      bump(I.c2);
      for (uint32_t v : I.args)
        bump(v);
      if (I.dst != NONE && I.dst < nv) {
        ++ndefs[I.dst];
        defi[I.dst] = i;
      }
    }
    auto inv = [](B b) -> B {
      // BUGFIX (bug 4, comparison negation): the negation of a comparison
      // is the involutive partner Lt<->Ge, Le<->Gt. The Gt and Ge rows
      // had their results swapped (Gt->Lt, Ge->Le): NOT(a>b) became a<b
      // instead of a<=b, and NOT(a>=b) became a<=b instead of a<b, so
      // e.g. !!(x < y) miscompiled (fuzz seed 1211: !!(input() < z9)
      // evaluated 1 where the oracle says 0 at the equality point).
      switch (b) {
      case B::Lt:
        return B::Ge; // NOT(a<b)  = a>=b
      case B::Le:
        return B::Gt; // NOT(a<=b) = a>b
      case B::Gt:
        return B::Le; // NOT(a>b)  = a<=b
      case B::Ge:
        return B::Lt; // NOT(a>=b) = a<b
      case B::Eq:
        return B::Ne;
      case B::Ne:
        return B::Eq;
      default:
        return b;
      }
    };
    for (uint32_t b = 0; b < fn.nblocks(); ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        Instr &I = fn.code[i];
        if (I.k != K::Bin || (I.b != B::Eq && I.b != B::Ne))
          continue;
        if (I.b2 == NONE || I.b2 >= nv || !kc[I.b2])
          continue; // const right
        val_t cv = kv[I.b2];
        if (cv != 0 && cv != 1)
          continue;
        uint32_t t1 = I.a;
        if (t1 == NONE || t1 >= nv)
          continue;
        uint32_t d = defi[t1];
        if (d == NONE || ndefs[t1] != 1 || uses[t1] != 1)
          continue;
        const Instr &D = fn.code[d];
        if (D.k != K::Bin)
          continue;
        if (D.b != B::Lt && D.b != B::Le && D.b != B::Gt && D.b != B::Ge &&
            D.b != B::Eq && D.b != B::Ne)
          continue;
        bool invert = (I.b == B::Eq) ? (cv == 0) : (cv == 1);
        I.b = invert ? inv(D.b) : D.b;
        I.a = D.a;
        I.b2 = D.b2;
        if ((I.b == B::Eq || I.b == B::Ne) && key(I.b2) < key(I.a))
          std::swap(I.a, I.b2); // keep the canonical operand order
        os.notcombs++;
        changed = true;
      }
  }
  return changed;
}

// --- 9a3. dominator-scoped common subexpression elimination
// -------------------

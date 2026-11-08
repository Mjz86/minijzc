#pragma once
// === divmagic.hpp ========================================
// div/mod-by-constant -> mulhi+shift (reciprocal from mjz_lib).

#include "minijz/base/pass_cfg.hpp"
#include "minijz/ir/instr.hpp"
#include "minijz/graph_lib.hpp"

// --- 9a6. division/modulo by constant -> multiply + shift + bit ops ----------
// The reciprocal (inv, shift) for |D| comes from mjz_lib's
// get_devide_inverse_and_shift (uintN.hpp), sized for the range [0, 2^63]
// (every |int64|, INT64_MIN's bit pattern included). Soundness gate, checked
// per divisor (eps = inv*D - 2^shift >= 0, M' = (2^63+1)/D):
//     M'*eps < inv   <=>   floor(n*inv / 2^shift) == n/D  for all n <= 2^63
// (verified against 119M brute-force cases across odd/even/huge divisors).
// Signed lowering (truncating; no trap is possible: D is a known safe const):
//   |n| = (n ^ (n>>a63)) - (n>>a63);  q = mulhi(|n|, inv) >> (shift-64);
//   sign re-applied by xor/sub;  D<0 negates;  r = n - q*D (wrapping-exact).
// Powers of two: q = (n + ((n>>a63) & (2^t-1))) >>a t;
//   r = (n & (2^t-1)) - ((n>>a63) & ((n & (2^t-1)) != 0 ? 2^t : 0)).
constexpr bool divmagic_of(val_t D, uint64_t &inv, uint32_t &shift) {
  uint64_t Dam = (uint64_t)(D < 0 ? (val_t)(0 - (uint64_t)D) : D);
  if (Dam < 3 || (Dam & (Dam - 1)) == 0)
    return false; // pow2: shifts only
  auto [invN, sh, bits, ss2, thr] = get_devide_inverse_and_shift<V, 64>(
      (uint64_t)1 << 63, Dam, false, (uint64_t)0, false);
  (void)bits;
  (void)ss2;
  (void)thr;
  if (invN.nth_word(1) != 0 || invN.nth_word(2) != 0)
    return false;
  uint32_t s = (uint32_t)sh;
  if (s < 64 || s > 127)
    return false;
  uint64_t m = (uint64_t)invN.nth_word(0);
  // exactness gate (128-bit math; every term fits)
  unsigned __int128 prod = (unsigned __int128)m * Dam;
  unsigned __int128 two_s = (unsigned __int128)1 << s;
  if (prod < two_s)
    return false; // eps < 0: reciprocal too small
  unsigned __int128 eps = prod - two_s;
  unsigned __int128 Mp = (((unsigned __int128)1 << 63) + 1) / Dam;
  if (Mp * eps >= (unsigned __int128)m)
    return false;
  inv = m;
  shift = s;
  return true;
}
// Build the lowering of `d(n) = I.a <op> D` into `out` (last def = I.dst).
constexpr bool build_divmagic(Fn &fn, const Instr &I, val_t D,
                              std::vector<Instr> &out) {
  const uint32_t n = I.a;
  const bool is_mod = (I.b == B::Mod);
  const uint32_t base = fn.base_of(n);
  auto tmp = [&]() { return new_anon_vreg(fn, base); };
  auto konst = [&](val_t c) {
    uint32_t v = new_anon_vreg(fn, 0);
    out.push_back({.k = K::Const, .dst = v, .c = c});
    return v;
  };
  auto bin = [&](B b, uint32_t dst, uint32_t a, uint32_t b2) {
    out.push_back({.k = K::Bin, .b = b, .dst = dst, .a = a, .b2 = b2});
  };
  uint64_t Dam = (uint64_t)(D < 0 ? (val_t)(0 - (uint64_t)D) : D);
  const uint32_t c63 = konst(63);
  const uint32_t sg = tmp(); // n >>a 63 (all-ones when negative)
  bin(B::AShr, sg, n, c63);
  uint32_t q = NONE;
  if ((Dam & (Dam - 1)) == 0) { // |D| = 2^t
    uint32_t t = 63 - (uint32_t)__builtin_clzll(Dam);
    uint32_t cm = konst((val_t)(Dam - 1));
    uint32_t bias = tmp();
    bin(B::And, bias, sg, cm);
    uint32_t nb = tmp();
    bin(B::Add, nb, n, bias);
    uint32_t cs = konst((val_t)t);
    if (D > 0 && !is_mod) {
      bin(B::AShr, I.dst, nb, cs); // q = (n + bias) >>a t
      return true;
    }
    q = tmp();
    bin(B::AShr, q, nb, cs);
    if (D < 0) { // q = 0 - q
      uint32_t z = konst(0);
      uint32_t nq = tmp();
      bin(B::Sub, nq, z, q);
      q = nq;
    }
    if (is_mod) {
      uint32_t tt = tmp();
      bin(B::And, tt, n, cm); // low bits of n
      uint32_t z = konst(0);
      uint32_t nz = tmp();
      bin(B::Ne, nz, tt, z); // != 0 -> 0/1
      uint32_t cd = konst((val_t)Dam);
      uint32_t m2 = tmp();
      bin(B::Mul, m2, nz, cd);
      uint32_t sub = tmp();
      bin(B::And, sub, sg, m2);
      bin(B::Sub, I.dst, tt, sub); // r = tt - (neg ? 2^t : 0)
      return true;
    }
    // div, D < 0: q was negated above
    out.push_back({.k = K::Copy, .dst = I.dst, .a = q});
    return true;
  }
  // general: exact reciprocal multiply
  uint64_t inv{};
  uint32_t sh{};
  if (!divmagic_of(D, inv, sh))
    return false;
  uint32_t ap = tmp();
  bin(B::Xor, ap, n, sg);
  uint32_t ap2 = tmp();
  bin(B::Sub, ap2, ap, sg); // |n| (pattern 2^63 for INT64_MIN)
  uint32_t ci = konst((val_t)inv);
  uint32_t mh = tmp();
  bin(B::MulHi, mh, ap2, ci);
  uint32_t q0 = mh;
  if (sh > 64) {
    uint32_t cs = konst((val_t)(sh - 64));
    q0 = tmp();
    bin(B::LShr, q0, mh, cs);
  }
  uint32_t xq = tmp();
  bin(B::Xor, xq, q0, sg);
  uint32_t qq = tmp();
  bin(B::Sub, qq, xq, sg); // sign re-applied
  if (D > 0 && !is_mod) {
    out.back().dst = I.dst;
    return true;
  }
  q = qq;
  if (D < 0) {
    uint32_t z = konst(0);
    uint32_t nq = tmp();
    bin(B::Sub, nq, z, q);
    q = nq;
  }
  if (is_mod) {
    uint32_t cd = konst(D);
    uint32_t p = tmp();
    bin(B::Mul, p, q, cd);
    bin(B::Sub, I.dst, n, p); // r = n - q*D (wrapping-exact)
    return true;
  }
  out.push_back({.k = K::Copy, .dst = I.dst, .a = q});
  return true;
}
constexpr bool opt_divmagic(Fn &fn, OptStats &os) {
  const uint32_t nb = fn.nblocks();
  if (!nb)
    return false;
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nv)
    return false;
  std::vector<uint32_t> ndefs(nv, 0);
  for (const Instr &I : fn.code)
    if (I.dst != NONE && I.dst < nv)
      ++ndefs[I.dst];
  std::vector<char> kc(nv, 0);
  std::vector<val_t> kv(nv, 0);
  for (const Instr &I : fn.code)
    if (I.k == K::Const && I.dst != NONE && I.dst < nv && ndefs[I.dst] == 1) {
      kc[I.dst] = 1;
      kv[I.dst] = I.c;
    }
  bool changed = false;
  std::vector<Instr> nc{};
  nc.reserve(fn.code.size());
  std::vector<Block> nbl(nb);
  for (uint32_t b = 0; b < nb; ++b) {
    nbl[b].begin = (uint32_t)nc.size();
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k == K::Bin && (I.b == B::Div || I.b == B::Mod) && I.b2 != NONE &&
          I.b2 < nv && kc[I.b2]) {
        val_t D = kv[I.b2];
        const bool aconst = I.a != NONE && I.a < nv && kc[I.a];
        if (!aconst && D != 0 && D != 1 && D != -1) {
          std::vector<Instr> seq{};
          if (build_divmagic(fn, I, D, seq)) {
            for (const Instr &x : seq)
              nc.push_back(x);
            os.divmagics++;
            changed = true;
            continue; // original Div/Mod replaced
          }
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

// --- 9d2. loop unrolling
// -------------------------------------------------------

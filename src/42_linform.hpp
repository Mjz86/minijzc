#pragma once

// --- 9b2. linear forms: shared by reassociation & loop closed forms ------
// A linear form is  sum(coeff_i * v_i) + K  with terms sorted by vreg id.
// Two's-complement +,-,* are exact mod 2^64 and never trap, so collecting
// and re-emitting them is always sound.
struct LinForm {
  std::vector<std::pair<uint32_t, val_t>> t{}; // (vreg, coeff != 0), sorted
  val_t c{};
};
constexpr void lin_term(LinForm &L, val_t k, uint32_t v) {
  if (k == 0) return;
  for (size_t i = 0; i < L.t.size(); ++i) {
    if (L.t[i].first == v) {
      L.t[i].second = (val_t)((uint64_t)L.t[i].second + (uint64_t)k);
      if (L.t[i].second == 0) L.t.erase(L.t.begin() + (long)i);
      return;
    }
  }
  size_t i = 0;
  while (i < L.t.size() && L.t[i].first < v) ++i;
  L.t.insert(L.t.begin() + (long)i, {v, k});
}
constexpr void lin_const(LinForm &L, val_t k, val_t cv) {
  L.c = (val_t)((uint64_t)L.c + (uint64_t)k * (uint64_t)cv);
}
// Accumulate k*v into L. Constants fold; SINGLE-USE +,-,*by-const (and Copy)
// defs located in `scope` blocks are absorbed recursively (their one user is
// the chain being expanded); everything else becomes a leaf term. `use_idx`
// (or NONE) enforces def-before-use when scopes are block-local. Absorbed
// instruction indices are recorded in `absorbed`.
constexpr void lin_expand(const Fn &fn, uint32_t nv,
                          const std::vector<uint32_t> &defi,
                          const std::vector<uint32_t> &defb,
                          const std::vector<char> &kc,
                          const std::vector<val_t> &kv,
                          const std::vector<uint32_t> &uses,
                          const std::vector<uint32_t> &ndefs,
                          const std::vector<char> &scope, uint32_t use_idx,
                          LinForm &L, val_t k, uint32_t v,
                          std::vector<uint32_t> &absorbed) {
  if (v == NONE || k == 0) return;
  if (v < nv && kc[v]) { lin_const(L, k, kv[v]); return; }
  if (v < nv && defi[v] != NONE && ndefs[v] == 1 && uses[v] == 1 &&
      defb[v] < (uint32_t)scope.size() && scope[defb[v]] &&
      (use_idx == NONE || defi[v] < use_idx)) {
    const Instr &D = fn.code[defi[v]];
    if (D.k == K::Bin && D.b == B::Add) {
      lin_expand(fn, nv, defi, defb, kc, kv, uses, ndefs, scope, defi[v], L, k,
                 D.a, absorbed);
      lin_expand(fn, nv, defi, defb, kc, kv, uses, ndefs, scope, defi[v], L, k,
                 D.b2, absorbed);
      absorbed.push_back(defi[v]);
      return;
    }
    if (D.k == K::Bin && D.b == B::Sub) {
      lin_expand(fn, nv, defi, defb, kc, kv, uses, ndefs, scope, defi[v], L, k,
                 D.a, absorbed);
      lin_expand(fn, nv, defi, defb, kc, kv, uses, ndefs, scope, defi[v], L,
                 (val_t)(0 - (uint64_t)k), D.b2, absorbed);
      absorbed.push_back(defi[v]);
      return;
    }
    if (D.k == K::Bin && D.b == B::Mul) {
      if (D.b2 != NONE && D.b2 < nv && kc[D.b2]) {
        lin_expand(fn, nv, defi, defb, kc, kv, uses, ndefs, scope, defi[v], L,
                   (val_t)((uint64_t)k * (uint64_t)kv[D.b2]), D.a, absorbed);
        absorbed.push_back(defi[v]);
        return;
      }
      if (D.a != NONE && D.a < nv && kc[D.a]) {
        lin_expand(fn, nv, defi, defb, kc, kv, uses, ndefs, scope, defi[v], L,
                   (val_t)((uint64_t)k * (uint64_t)kv[D.a]), D.b2, absorbed);
        absorbed.push_back(defi[v]);
        return;
      }
    }
    if (D.k == K::Copy && D.a != NONE) { // identity through a copy
      lin_expand(fn, nv, defi, defb, kc, kv, uses, ndefs, scope, defi[v], L, k,
                 D.a, absorbed);
      absorbed.push_back(defi[v]);
      return;
    }
  }
  lin_term(L, k, v); // leaf
}
// minimal-instruction cost of materializing L (see lin_emit)
constexpr uint32_t lin_cost(const LinForm &L) {
  uint32_t n = 0, terms = 0;
  bool first = true;
  for (const auto &p : L.t) {
    if (p.second == 0) continue;
    ++terms;
    if (first) {
      n += (p.second == 1) ? 0 : 2; // Mul by const k (any sign) or bare v
      first = false;
    } else {
      n += (p.second == 1 || p.second == -1) ? 1 : 3; // (Const+Mul) + Add/Sub
    }
  }
  if (!terms) return 1;                          // pure constant
  if (L.c != 0) n += 2;                          // Const + Add
  return n ? n : 1;                              // single +x term: Copy
}
// materialize L into `out`; the LAST instruction defines `dst`
constexpr void lin_emit(Fn &fn, std::vector<Instr> &out, const LinForm &L,
                        uint32_t dst) {
  const size_t base = out.size(); // where THIS call started emitting: the
                                   // final dst-landing may only rewrite an
                                   // instruction emitted here, never some
                                   // unrelated earlier instr that happens to
                                   // define acc (e.g. the Len defining the
                                   // single bare term of L -- renaming it
                                   // would leave every user of that term
                                   // reading a ghost vreg)
  std::vector<std::pair<uint32_t, val_t>> ts{};
  for (const auto &p : L.t)
    if (p.second != 0) ts.push_back(p);
  if (ts.empty()) {
    out.push_back({.k = K::Const, .dst = dst, .c = L.c});
    return;
  }
  auto mkconst = [&](val_t c) {
    uint32_t v = new_anon_vreg(fn, 0);
    out.push_back({.k = K::Const, .dst = v, .c = c});
    return v;
  };
  uint32_t acc = NONE;
  for (size_t j = 0; j < ts.size(); ++j) {
    uint32_t v = ts[j].first;
    val_t k = ts[j].second;
    uint32_t t = v;
    if (j == 0) {
      if (k == 1) {
        acc = v;
      } else if (k == -1) { // 0 - v
        uint32_t z = mkconst(0);
        uint32_t r = new_anon_vreg(fn, fn.base_of(v));
        out.push_back({.k = K::Bin, .b = B::Sub, .dst = r, .a = z, .b2 = v});
        acc = r;
      } else { // Mul by the (possibly negative) constant itself
        uint32_t cv = mkconst(k);
        uint32_t r = new_anon_vreg(fn, fn.base_of(v));
        out.push_back({.k = K::Bin, .b = B::Mul, .dst = r, .a = v, .b2 = cv});
        acc = r;
      }
      continue;
    }
    // subsequent terms: k == 1 -> Add(v); k == -1 -> Sub(v); otherwise
    // scale by k itself (the Mul carries the sign) and Add — exact even for
    // k == INT64_MIN, whose "magnitude" would wrap back to itself
    if (k != 1 && k != -1) {
      uint32_t cv = mkconst(k);
      uint32_t r = new_anon_vreg(fn, fn.base_of(v));
      out.push_back({.k = K::Bin, .b = B::Mul, .dst = r, .a = v, .b2 = cv});
      t = r;
    }
    uint32_t r = new_anon_vreg(fn, fn.base_of(v));
    out.push_back({.k = K::Bin, .b = (k == -1) ? B::Sub : B::Add, .dst = r,
                   .a = acc, .b2 = t});
    acc = r;
  }
  if (L.c != 0) {
    uint32_t cv = mkconst(L.c);
    uint32_t r = new_anon_vreg(fn, 0);
    out.push_back({.k = K::Bin, .b = B::Add, .dst = r, .a = acc, .b2 = cv});
    acc = r;
  }
  // acc must land in dst: rewrite the last def WE emitted (never an earlier
  // unrelated one -- see `base` above), or emit a Copy
  if (out.size() > base && out.back().dst == acc && acc != dst) {
    out.back().dst = dst;
  } else if (acc != dst) {
    out.push_back({.k = K::Copy, .dst = dst, .a = acc});
  }
}

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
  if (!nb) return false;
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nv) return false;
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
        if (v != NONE && v < nv) ++uses[v];
      };
      bump(I.a);
      bump(I.b2);
      bump(I.c2);
      for (uint32_t v : I.args) bump(v);
    }
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k == K::Const && I.dst != NONE && I.dst < nv &&
          ndefs[I.dst] == 1) {
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
      for (uint32_t x : dropped) if (x == i) return true;
      return false;
    };
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      if (was_dropped(i)) continue;
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
          for (uint32_t x : absr) dropped.push_back(x);
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
  if (Dam < 3 || (Dam & (Dam - 1)) == 0) return false; // pow2: shifts only
  auto [invN, sh, bits, ss2, thr] =
      get_devide_inverse_and_shift<V, 64>((uint64_t)1 << 63, Dam, false,
                                          (uint64_t)0, false);
  (void)bits;
  (void)ss2;
  (void)thr;
  if (invN.nth_word(1) != 0 || invN.nth_word(2) != 0) return false;
  uint32_t s = (uint32_t)sh;
  if (s < 64 || s > 127) return false;
  uint64_t m = (uint64_t)invN.nth_word(0);
  // exactness gate (128-bit math; every term fits)
  unsigned __int128 prod = (unsigned __int128)m * Dam;
  unsigned __int128 two_s = (unsigned __int128)1 << s;
  if (prod < two_s) return false; // eps < 0: reciprocal too small
  unsigned __int128 eps = prod - two_s;
  unsigned __int128 Mp = (((unsigned __int128)1 << 63) + 1) / Dam;
  if (Mp * eps >= (unsigned __int128)m) return false;
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
      bin(B::And, tt, n, cm);       // low bits of n
      uint32_t z = konst(0);
      uint32_t nz = tmp();
      bin(B::Ne, nz, tt, z);        // != 0 -> 0/1
      uint32_t cd = konst((val_t)Dam);
      uint32_t m2 = tmp();
      bin(B::Mul, m2, nz, cd);
      uint32_t sub = tmp();
      bin(B::And, sub, sg, m2);
      bin(B::Sub, I.dst, tt, sub);  // r = tt - (neg ? 2^t : 0)
      return true;
    }
    // div, D < 0: q was negated above
    out.push_back({.k = K::Copy, .dst = I.dst, .a = q});
    return true;
  }
  // general: exact reciprocal multiply
  uint64_t inv{};
  uint32_t sh{};
  if (!divmagic_of(D, inv, sh)) return false;
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
  if (!nb) return false;
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nv) return false;
  std::vector<uint32_t> ndefs(nv, 0);
  for (const Instr &I : fn.code)
    if (I.dst != NONE && I.dst < nv) ++ndefs[I.dst];
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
      if (I.k == K::Bin && (I.b == B::Div || I.b == B::Mod) &&
          I.b2 != NONE && I.b2 < nv && kc[I.b2]) {
        val_t D = kv[I.b2];
        const bool aconst = I.a != NONE && I.a < nv && kc[I.a];
        if (!aconst && D != 0 && D != 1 && D != -1) {
          std::vector<Instr> seq{};
          if (build_divmagic(fn, I, D, seq)) {
            for (const Instr &x : seq) nc.push_back(x);
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

// --- 9d2. loop unrolling -------------------------------------------------------
// Full unroll of simple counted loops with a CONSTANT trip count, computed
// EXACTLY (the induction advances with the program's wrapping add/sub, so
// even wrap-around trip counts are honored; the count is capped at `limit`).
// Shape required (all from the lib's natural-loop analysis):
//   * one back edge: the header's unique in-loop predecessor (the latch)
//   * one outside predecessor (the preheader)
//   * the ONLY exit edge is the header's comparison (no `break`s)
//   * induction phi i = phi(i0, i+c) with constant i0 and c != 0
//   * header test `i < bound` / `i <= bound` (either operand order), bound
//     constant
// The loop becomes K body copies + a tail copy of the header (whose test is
// provably false -> Br exit). Header phis turn into per-copy constants (the
// induction) and chained values (accumulators); uses outside the loop are
// rewritten to the tail's exit values; the exit block's phis are rekeyed
// from the header to the tail. constprop + DCE then fold pure arithmetic
// loops into constants at compile time.
constexpr bool opt_unroll(Fn &fn, const An &A, OptStats &os,
                          uint32_t limit) {
  if (!limit) return false;
  if (A.n != fn.nblocks() || !fn.nblocks()) return false;
  const uint32_t nb = fn.nblocks();
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nv) return false;
  std::vector<char> kc(nv, 0);
  std::vector<val_t> kv(nv, 0);
  std::vector<uint32_t> defi(nv, NONE), defb(nv, NONE);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.dst != NONE && I.dst < nv) {
        defi[I.dst] = i;
        defb[I.dst] = b;
      }
      if (I.k == K::Const && I.dst != NONE && I.dst < nv) {
        kc[I.dst] = 1;
        kv[I.dst] = I.c;
      }
    }
  std::vector<uint32_t> headers;
  for (uint32_t h = 0; h < A.n; ++h)
    if (A.inner[h] == h) headers.push_back(h);
  for (size_t i = 0; i + 1 < headers.size(); ++i)
    for (size_t j = i + 1; j < headers.size(); ++j)
      if (A.nest[headers[j]] > A.nest[headers[i]])
        std::swap(headers[i], headers[j]);
  for (uint32_t h : headers) {
    auto row = A.loops.range()[h];
    if (row.empty()) continue;
    std::vector<char> inL(nb, 0);
    for (ulen x : row) inL[(uint32_t)x] = 1;
    inL[h] = 1;
    // unique outside pred (preheader) + unique latch
    uint32_t pre = NONE, latch = NONE, outside = 0;
    for (uint32_t p : fn.preds[h]) {
      if (inL[p]) latch = p;
      else { pre = p; ++outside; }
    }
    if (outside != 1 || pre == NONE || latch == NONE) continue;
    if (fn.blocks[h].begin == fn.blocks[h].end) continue;
    const Instr hterm = fn.code[fn.blocks[h].end - 1];
    if (hterm.k != K::Cbr) continue;
    // the only exit edge is the header's; find the exit and the body arm
    uint32_t X = NONE, body = NONE;
    {
      bool bad = false;
      for (uint32_t b = 0; b < nb && !bad; ++b) {
        if (!inL[b]) continue;
        for (uint32_t s : {fn.succ0(b), fn.succ1(b)}) {
          if (s == NONE) continue;
          if (!inL[s]) {
            if (b != h) { bad = true; break; } // a break: not unrollable
            if (X == NONE) X = s;
            else if (X != s) { bad = true; break; }
          }
        }
      }
      if (bad || X == NONE) continue;
    }
    if (hterm.t == X && hterm.f != NONE && inL[hterm.f]) body = hterm.f;
    else if (hterm.f == X && inL[hterm.t]) body = hterm.t;
    else continue;
    if (body == h) continue; // self-loop header: skip this rare shape
    // ---- header phis: (dst, pre_arg, latch_arg); ALL of them must match --
    struct HPhi { uint32_t dst, pre_arg, latch_arg; };
    std::vector<HPhi> hphis;
    {
      uint32_t nphi = 0;
      for (uint32_t i = fn.blocks[h].begin; i < fn.blocks[h].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k != K::Phi) break;
        ++nphi;
        uint32_t pa = (I.t == pre) ? I.a : ((I.f == pre) ? I.b2 : NONE);
        uint32_t la = (I.t == latch) ? I.a : ((I.f == latch) ? I.b2 : NONE);
        if (pa == NONE || la == NONE) continue;
        hphis.push_back({I.dst, pa, la});
      }
      if (!nphi || hphis.size() != nphi) continue;
    }
    // ---- find the induction phi + the header test ------------------------
    uint32_t ind = NONE, ind_idx = NONE, cond = hterm.a;
    val_t step_mag = 0;
    bool step_sub = false;
    if (cond == NONE || cond >= nv || defi[cond] == NONE ||
        defb[cond] != h)
      continue;
    for (size_t pi = 0; pi < hphis.size() && ind == NONE; ++pi) {
      uint32_t L = hphis[pi].latch_arg;
      if (L >= nv || defi[L] == NONE || !inL[defb[L]]) continue;
      const Instr D = fn.code[defi[L]];
      if (D.k != K::Bin) continue;
      val_t mag = 0;
      bool sub = false, okst = false;
      if (D.b == B::Add) {
        if (D.a == hphis[pi].dst && D.b2 < nv && kc[D.b2] && kv[D.b2] != 0) {
          mag = kv[D.b2]; okst = true;
        } else if (D.b2 == hphis[pi].dst && D.a < nv && kc[D.a] &&
                   kv[D.a] != 0) {
          mag = kv[D.a]; okst = true;
        }
      } else if (D.b == B::Sub) {
        if (D.a == hphis[pi].dst && D.b2 < nv && kc[D.b2] && kv[D.b2] != 0) {
          mag = kv[D.b2]; sub = true; okst = true;
        }
      }
      if (!okst) continue;
      // the header test compares THIS phi against a constant bound
      const Instr C = fn.code[defi[cond]];
      if (C.k != K::Bin || (C.b != B::Lt && C.b != B::Le)) continue;
      bool uses_it = false;
      if (C.a == hphis[pi].dst && C.b2 < nv && kc[C.b2]) uses_it = true;
      else if (C.b2 == hphis[pi].dst && C.a < nv && kc[C.a]) uses_it = true;
      if (!uses_it) continue;
      if (!kc[hphis[pi].pre_arg]) continue; // i0 must be constant
      ind = hphis[pi].dst;
      ind_idx = (uint32_t)pi;
      step_mag = mag;
      step_sub = sub;
    }
    if (ind == NONE) continue;
    const HPhi IP = hphis[ind_idx];
    const val_t i0 = kv[IP.pre_arg];
    const Instr C = fn.code[defi[cond]];
    const bool lhs_is_ind = (C.a == ind);
    const val_t bound = lhs_is_ind ? kv[C.b2] : kv[C.a];
    uint32_t K = NONE;
    std::vector<val_t> ivk{}; // ivk[k] = induction value AT iteration k,
                              // including the failing K-th test (the exit
                              // value the loop leaves in i)
    {
      val_t iv = i0;
      for (uint32_t k = 0;; ++k) {
        val_t lhs = lhs_is_ind ? iv : bound;
        val_t rhs = lhs_is_ind ? bound : iv;
        bool go = (C.b == B::Lt) ? (lhs < rhs) : (lhs <= rhs);
        ivk.push_back(iv);
        if (!go) { K = k; break; }
        if (k >= limit) break; // more than `limit` iterations: skip
        iv = step_sub ? w_sub(iv, step_mag).v : w_add(iv, step_mag).v;
      }
    }
    if (K == NONE || K > limit) continue;
    uint32_t loop_instrs = 0;
    for (uint32_t b = 0; b < nb; ++b)
      if (inL[b]) loop_instrs += fn.blocks[b].end - fn.blocks[b].begin;
    if ((K + 1) * loop_instrs > 400) continue; // code growth cap
    // ---- safety: outside uses may only reference header-defined values ----
    {
      std::vector<char> loopdef(nv, 0);
      for (uint32_t v = 0; v < nv; ++v)
        if (defi[v] != NONE && inL[defb[v]]) loopdef[v] = 1;
      bool bad = false;
      for (uint32_t b = 0; b < nb && !bad; ++b) {
        if (inL[b]) continue;
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
          const Instr &I = fn.code[i];
          auto chk = [&](uint32_t v) {
            if (v != NONE && v < nv && loopdef[v] && defb[v] != h)
              bad = true;
          };
          if (I.k == K::Phi) { // only the exit edge from h may carry it
            chk(I.a);
            chk(I.b2);
            if (bad && b == X) {
              // args keyed to preds other than h must not be loop values
              bad = false;
              if (I.t != NONE && I.t != h && I.a != NONE && I.a < nv &&
                  loopdef[I.a])
                bad = true;
              if (I.f != NONE && I.f != h && I.b2 != NONE && I.b2 < nv &&
                  loopdef[I.b2])
                bad = true;
            }
          } else {
            chk(I.a);
            chk(I.b2);
            chk(I.c2);
            for (uint32_t v : I.args) chk(v);
          }
          if (bad) break;
        }
      }
      if (bad) continue;
    }
    // ================= commit =================
    os.unrolls++;
    if (K == 0) {
      // zero iterations: the (provably false) test becomes Br exit; the
      // unreachable body dies and the header phis degenerate to their
      // preheader args in the next unreachable/phi-degen round
      Instr &T = fn.code[fn.blocks[h].end - 1];
      T.k = K::Br;
      T.t = X;
      T.f = NONE;
      T.a = NONE;
      fn.succ[2 * h] = X;
      fn.succ[2 * h + 1] = NONE;
      fn.recompute_preds();
      return true;
    }
    // block / vreg maps for copies 0..K (copy K = header-only tail)
    std::vector<std::vector<uint32_t>> bmap(K + 1,
                                            std::vector<uint32_t>(nb, NONE));
    std::vector<std::vector<uint32_t>> fresh(K + 1,
                                             std::vector<uint32_t>(nv, NONE));
    for (uint32_t k = 0; k <= K; ++k)
      for (uint32_t b = 0; b < nb; ++b) {
        if (!inL[b]) continue;
        if (k == K && b != h) continue; // tail: header only
        uint32_t id = (uint32_t)fn.blocks.size();
        fn.blocks.push_back({});
        fn.succ.push_back(NONE);
        fn.succ.push_back(NONE);
        bmap[k][b] = id;
      }
    for (uint32_t k = 0; k <= K; ++k)
      for (uint32_t v = 0; v < nv; ++v) {
        if (defi[v] == NONE || !inL[defb[v]]) continue;
        if (k == K && defb[v] != h) continue;
        fresh[k][v] = new_anon_vreg(fn, fn.base_of(v));
      }
    // entry value of each header phi at copy k (uses of the phi see this)
    //   induction: the constant i0 + k*step (materialized in the header)
    //   accumulator: k == 0 ? pre_arg : map(latch_arg, k-1)
    std::vector<std::vector<uint32_t>> entry(K + 1,
                                             std::vector<uint32_t>(
                                                 hphis.size(), NONE));
    // map a vreg at copy k (uses): phi -> entry, loop def -> fresh, else v
    std::vector<uint32_t> phi_idx(nv, NONE);
    for (size_t pi = 0; pi < hphis.size(); ++pi) phi_idx[hphis[pi].dst] = (uint32_t)pi;
    auto mapv = [&](uint32_t v, uint32_t k) -> uint32_t {
      if (v == NONE) return NONE;
      if (v < nv && phi_idx[v] != NONE) return entry[k][phi_idx[v]];
      if (v < nv && fresh[k][v] != NONE) return fresh[k][v];
      return v;
    };
    // fill entry rows in k order (the induction gets its const vreg)
    for (uint32_t k = 0; k <= K; ++k) {
      // induction const first (it IS the entry value of the induction)
      entry[k][ind_idx] = fresh[k][ind];
      for (size_t pi = 0; pi < hphis.size(); ++pi) {
        if ((uint32_t)pi == ind_idx) continue;
        entry[k][pi] = (k == 0) ? hphis[pi].pre_arg
                                : mapv(hphis[pi].latch_arg, k - 1);
      }
    }
    // emit copies (append-only: existing block ranges stay valid)
    auto remap_args_of = [&](Instr &c, uint32_t k) {
      for (uint32_t &av : c.args) av = mapv(av, k);
    };
    for (uint32_t k = 0; k <= K; ++k) {
      bool tail = (k == K);
      // header copy: [const i_k] + h's non-phi instrs + Br(body/X)
      {
        uint32_t Hk = bmap[k][h];
        fn.blocks[Hk].begin = (uint32_t)fn.code.size();
        fn.code.push_back({.k = K::Const, .dst = fresh[k][ind],
                           .c = ivk[k]});
        for (uint32_t i = fn.blocks[h].begin; i + 1 < fn.blocks[h].end;
             ++i) {
          const Instr &I = fn.code[i];
          if (I.k == K::Phi) continue;
          Instr c = I;
          if (c.dst != NONE && c.dst < nv) c.dst = fresh[k][c.dst];
          c.a = mapv(c.a, k);
          c.b2 = mapv(c.b2, k);
          if (c.c2 != NONE) c.c2 = mapv(c.c2, k);
          remap_args_of(c, k);
          fn.code.push_back(c);
        }
        uint32_t tgt = tail ? X : bmap[k][body];
        fn.code.push_back({.k = K::Br, .t = tgt});
        fn.blocks[Hk].end = (uint32_t)fn.code.size();
        fn.succ[2 * Hk] = tgt;
        fn.succ[2 * Hk + 1] = NONE;
      }
      if (tail) continue;
      // body copies in original block order
      for (uint32_t b = 0; b < nb; ++b) {
        if (!inL[b] || b == h) continue;
        uint32_t Bk = bmap[k][b];
        fn.blocks[Bk].begin = (uint32_t)fn.code.size();
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
          const Instr &I = fn.code[i];
          if (i + 1 == fn.blocks[b].end) { // terminator
            if (I.k == K::Ret || I.k == K::Exit) {
              Instr c = I;
              c.a = mapv(c.a, k);
              fn.code.push_back(c);
            } else if (I.k == K::Br || I.k == K::Cbr) {
              Instr c = I;
              c.a = mapv(c.a, k);
              c.t = (I.t == h) ? bmap[k + 1][h]
                               : ((I.t != NONE && bmap[k][I.t] != NONE)
                                      ? bmap[k][I.t]
                                      : I.t);
              if (I.k == K::Cbr)
                c.f = (I.f == h) ? bmap[k + 1][h]
                                 : ((I.f != NONE && bmap[k][I.f] != NONE)
                                        ? bmap[k][I.f]
                                        : I.f);
              fn.code.push_back(c);
            } else {
              fn.code.push_back(I); // unreachable-block junk: copy as-is
            }
            continue;
          }
          Instr c = I;
          if (I.k == K::Phi) {
            if (c.dst != NONE && c.dst < nv) c.dst = fresh[k][c.dst];
            c.a = mapv(c.a, k);
            c.b2 = mapv(c.b2, k);
            if (c.t != NONE && bmap[k][c.t] != NONE) c.t = bmap[k][c.t];
            if (c.f != NONE && bmap[k][c.f] != NONE) c.f = bmap[k][c.f];
          } else {
            if (c.dst != NONE && c.dst < nv) c.dst = fresh[k][c.dst];
            c.a = mapv(c.a, k);
            c.b2 = mapv(c.b2, k);
            if (c.c2 != NONE) c.c2 = mapv(c.c2, k);
            remap_args_of(c, k);
          }
          fn.code.push_back(c);
        }
        fn.blocks[Bk].end = (uint32_t)fn.code.size();
        fn.succ[2 * Bk] = (bmap[k][fn.succ0(b)] != NONE && inL[fn.succ0(b)])
                              ? bmap[k][fn.succ0(b)]
                              : fn.succ0(b);
        fn.succ[2 * Bk + 1] =
            (fn.succ1(b) != NONE && inL[fn.succ1(b)] &&
             bmap[k][fn.succ1(b)] != NONE)
                ? bmap[k][fn.succ1(b)]
                : fn.succ1(b);
        // the back edge from this copy's latch goes to the NEXT copy
        if (b == latch) fn.succ[2 * Bk] = bmap[k + 1][h];
      }
    }
    // rewire the preheader edge h -> first copy
    for (uint32_t i = fn.blocks[pre].begin; i < fn.blocks[pre].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k == K::Br && I.t == h) I.t = bmap[0][h];
      else if (I.k == K::Cbr) {
        if (I.t == h) I.t = bmap[0][h];
        if (I.f == h) I.f = bmap[0][h];
      }
    }
    if (fn.succ[2 * pre] == h) fn.succ[2 * pre] = bmap[0][h];
    if (fn.succ[2 * pre + 1] == h) fn.succ[2 * pre + 1] = bmap[0][h];
    // exit-block phis: the h-edge now arrives from the tail
    for (uint32_t i = fn.blocks[X].begin; i < fn.blocks[X].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k != K::Phi) break;
      if (I.t == h) {
        I.t = bmap[K][h];
        I.a = mapv(I.a, K);
      } else if (I.f == h) {
        I.f = bmap[K][h];
        I.b2 = mapv(I.b2, K);
      }
    }
    // uses outside the loop -> tail values (h-defined only, per the scan)
    for (uint32_t b = 0; b < nb; ++b) {
      if (inL[b]) continue;
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        Instr &I = fn.code[i];
        if (I.a != NONE && I.a < nv && defb[I.a] == h) I.a = mapv(I.a, K);
        if (I.b2 != NONE && I.b2 < nv && defb[I.b2] == h) I.b2 = mapv(I.b2, K);
        if (I.c2 != NONE && I.c2 < nv && defb[I.c2] == h) I.c2 = mapv(I.c2, K);
        for (uint32_t &av : I.args)
          if (av < nv && defb[av] == h) av = mapv(av, K);
      }
    }
    fn.recompute_preds();
    return true; // one unroll per call: the fixpoint driver re-analyzes
  }
  return false;
}

// --- 9d5. loop closed-form evaluation (constant trip counts) -----------------
// A counted loop with a constant trip count N whose body is pure scalar math
// (no memory, calls, prints, or inner control flow) does not need to run at
// all: every loop-carried variable x evolves as an AFFINE RECURRENCE
//     x_{k+1} = a*x_k + b*i_k + g      (i = the induction, i_k = i0 + d*k)
// whose closed form is a pair of compile-time constants (K1, K0):
//     x_N = K1 * x0 + K0,
//     K1 = a^N,  K0 = (C + B*(N-1))*S - B*T,  C = b*i0 + g,  B = b*d,
//     S(n) = sum_{j<n} a^j (geometric series),  T(n) = sum_{j<n} j*a^j
// — clang-style additive (a=1) AND geometric (a!=1) series, computed EXACTLY
// mod 2^64 by bit-doubling (no modular inverses: wrapping loops are fine).
// The loop is replaced by `x = K1*x0 + K0` in the preheader (x0 may be a
// runtime value!). Bodies that are not affine (x*x, mutual dependencies)
// fall back to exact compile-time SIMULATION of the N iterations (traps
// abort the rewrite and keep the loop). One loop per call; the fixpoint
// driver re-analyzes (inner loops fold first, enabling their parents).
inline constexpr uint64_t LCF_TRIP_BUDGET = (uint64_t)1 << 21; // ~2.1M trips
inline constexpr uint64_t LCF_SIM_BUDGET = 40000000ull;        // sim steps
constexpr uint64_t lcf_pow(uint64_t a, uint64_t n) { // a^n mod 2^64
  uint64_t r = 1;
  while (n) {
    if (n & 1) r *= a;
    a *= a;
    n >>= 1;
  }
  return r;
}
// S(n) = sum_{j<n} a^j, T(n) = sum_{j<n} j*a^j, out P = a^n, all mod 2^64,
// exact by doubling: S(2m)=S(m)(1+a^m); T(2m)=T(m)(1+a^m)+m*a^m*S(m);
// S(2m+1)=S(2m)+a^(2m); T(2m+1)=T(2m)+(2m)*a^(2m)
constexpr void lcf_geo(uint64_t a, uint64_t n, uint64_t &S, uint64_t &T,
                       uint64_t &P) {
  S = 0;
  T = 0;
  P = 1;
  if (!n) return;
  uint64_t m = 0;
  int hi = 63;
  while (!((n >> hi) & 1)) --hi;
  for (int bit = hi; bit >= 0; --bit) {
    const uint64_t Sm = S, Tm = T, Pm = P;
    S = Sm * (Pm + 1);
    T = Tm * (Pm + 1) + Pm * Sm * m;
    P = Pm * Pm;
    m <<= 1;
    if ((n >> bit) & 1) {
      S += P;
      T += m * P;
      P *= a;
      ++m;
    }
  }
}
constexpr bool opt_lcf(Fn &fn, const An &A, OptStats &os) {
  if (A.n != fn.nblocks() || !fn.nblocks()) return false;
  const uint32_t nb = fn.nblocks();
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nv) return false;
  std::vector<char> kc(nv, 0);
  std::vector<val_t> kv(nv, 0);
  std::vector<uint32_t> defi(nv, NONE), defb(nv, NONE);
  std::vector<uint32_t> uses(nv, 0), ndefs(nv, 0);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.dst != NONE && I.dst < nv) {
        ++ndefs[I.dst];
        defi[I.dst] = i;
        defb[I.dst] = b;
      }
      auto bump = [&](uint32_t v) {
        if (v != NONE && v < nv) ++uses[v];
      };
      bump(I.a);
      bump(I.b2);
      bump(I.c2);
      for (uint32_t v : I.args) bump(v);
    }
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k == K::Const && I.dst != NONE && I.dst < nv &&
          ndefs[I.dst] == 1) {
        kc[I.dst] = 1;
        kv[I.dst] = I.c;
      }
    }
  std::vector<uint32_t> headers;
  for (uint32_t h = 0; h < A.n; ++h)
    if (A.inner[h] == h) headers.push_back(h);
  for (size_t i = 0; i + 1 < headers.size(); ++i)
    for (size_t j = i + 1; j < headers.size(); ++j)
      if (A.nest[headers[j]] > A.nest[headers[i]])
        std::swap(headers[i], headers[j]);
  for (uint32_t h : headers) {
    auto row = A.loops.range()[h];
    if (row.empty()) continue;
    std::vector<char> inL(nb, 0);
    for (ulen x : row) inL[(uint32_t)x] = 1;
    inL[h] = 1;
    uint32_t pre = NONE, latch = NONE, outside = 0;
    for (uint32_t p : fn.preds[h]) {
      if (inL[p]) latch = p;
      else { pre = p; ++outside; }
    }
    if (outside != 1 || pre == NONE || latch == NONE) continue;
    if (fn.blocks[h].begin == fn.blocks[h].end) continue;
    const Instr hterm = fn.code[fn.blocks[h].end - 1];
    if (hterm.k != K::Cbr) continue;
    uint32_t X = NONE, body = NONE;
    {
      bool bad = false;
      for (uint32_t b = 0; b < nb && !bad; ++b) {
        if (!inL[b]) continue;
        for (uint32_t s : {fn.succ0(b), fn.succ1(b)}) {
          if (s == NONE) continue;
          if (!inL[s]) {
            if (b != h) { bad = true; break; }
            if (X == NONE) X = s;
            else if (X != s) { bad = true; break; }
          }
        }
      }
      if (bad || X == NONE) continue;
    }
    if (hterm.t == X && hterm.f != NONE && inL[hterm.f]) body = hterm.f;
    else if (hterm.f == X && inL[hterm.t]) body = hterm.t;
    else continue;
    if (body == h) continue; // self-loop header: rare shape, skip
    struct HPhi { uint32_t dst, pre_arg, latch_arg; };
    std::vector<HPhi> hphis;
    {
      uint32_t nphi = 0;
      for (uint32_t i = fn.blocks[h].begin; i < fn.blocks[h].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k != K::Phi) break;
        ++nphi;
        uint32_t pa = (I.t == pre) ? I.a : ((I.f == pre) ? I.b2 : NONE);
        uint32_t la = (I.t == latch) ? I.a : ((I.f == latch) ? I.b2 : NONE);
        if (pa == NONE || la == NONE) continue;
        hphis.push_back({I.dst, pa, la});
      }
      if (!nphi || hphis.size() != nphi) continue;
    }
    // ---- induction phi + header test (same shape unroll requires) ----------
    uint32_t ind = NONE, ind_idx = NONE, cond = hterm.a;
    val_t step_mag = 0;
    bool step_sub = false;
    if (cond == NONE || cond >= nv || defi[cond] == NONE ||
        defb[cond] != h)
      continue;
    for (size_t pi = 0; pi < hphis.size() && ind == NONE; ++pi) {
      uint32_t L = hphis[pi].latch_arg;
      if (L >= nv || defi[L] == NONE || !inL[defb[L]]) continue;
      const Instr D = fn.code[defi[L]];
      if (D.k != K::Bin) continue;
      val_t mag = 0;
      bool sub = false, okst = false;
      if (D.b == B::Add) {
        if (D.a == hphis[pi].dst && D.b2 < nv && kc[D.b2] && kv[D.b2] != 0) {
          mag = kv[D.b2];
          okst = true;
        } else if (D.b2 == hphis[pi].dst && D.a < nv && kc[D.a] &&
                   kv[D.a] != 0) {
          mag = kv[D.a];
          okst = true;
        }
      } else if (D.b == B::Sub) {
        if (D.a == hphis[pi].dst && D.b2 < nv && kc[D.b2] && kv[D.b2] != 0) {
          mag = kv[D.b2];
          sub = true;
          okst = true;
        }
      }
      if (!okst) continue;
      const Instr C = fn.code[defi[cond]];
      if (C.k != K::Bin || (C.b != B::Lt && C.b != B::Le)) continue;
      bool uses_it = false;
      if (C.a == hphis[pi].dst && C.b2 < nv) uses_it = true;
      else if (C.b2 == hphis[pi].dst && C.a < nv) uses_it = true;
      if (!uses_it) continue;
      ind = hphis[pi].dst;
      ind_idx = (uint32_t)pi;
      step_mag = mag;
      step_sub = sub;
    }
    if (ind == NONE) {
      // ---- GENERAL SIMULATOR --------------------------------------------
      // The test is not a counted `<`/`<=` induction comparison (e.g.
      // `while (h != 0)` -- Euclid's GCD). If every loop input is a known
      // constant and the body is straight-line pure math, the loop's whole
      // execution is determined: simulate header test + body chain per
      // iteration with the program's exact wrapping/trapping arithmetic; a
      // trap or budget overflow aborts (the loop stays). This is the pass
      // that folds the GCD demo to the constant 21 -- note it needs no
      // induction variable at all, just constants and purity.
      bool ok = true;
      for (const HPhi &p : hphis)
        if (!(p.pre_arg < nv && kc[p.pre_arg])) ok = false;
      { // purity: header = phis + Const/Copy/Bin + Cbr; others + Br only
        for (uint32_t b2 = 0; b2 < nb && ok; ++b2) {
          if (!inL[b2]) continue;
          for (uint32_t i = fn.blocks[b2].begin; i < fn.blocks[b2].end;
               ++i) {
            const Instr &I = fn.code[i];
            if (i + 1 == fn.blocks[b2].end) {
              if (b2 == h) {
                if (I.k != K::Cbr) ok = false;
              } else if (I.k != K::Br)
                ok = false;
              break;
            }
            if (I.k == K::Phi) {
              if (b2 != h) ok = false;
            } else if (I.k == K::Const || I.k == K::Copy) {
            } else if (I.k == K::Bin) {
            } else
              ok = false;
            if (!ok) break;
          }
        }
      }
      { // outside uses may reference ONLY header phi dsts
        std::vector<char> phid(nv, 0);
        for (const HPhi &p : hphis) phid[p.dst] = 1;
        for (uint32_t b2 = 0; b2 < nb && ok; ++b2) {
          if (inL[b2]) continue;
          for (uint32_t i = fn.blocks[b2].begin; i < fn.blocks[b2].end && ok;
               ++i) {
            const Instr &I = fn.code[i];
            auto chk = [&](uint32_t v) {
              if (v != NONE && v < nv && defi[v] != NONE && inL[defb[v]] &&
                  !phid[v])
                ok = false;
            };
            chk(I.a);
            chk(I.b2);
            chk(I.c2);
            for (uint32_t v : I.args) chk(v);
          }
        }
      }
      { // pre must not already reach the exit (phi rekey would dup preds)
        for (uint32_t p : fn.preds[X])
          if (p == pre) ok = false;
      }
      std::vector<uint32_t> chain{};
      if (ok) { // the straight-line body chain from `body` to the latch
        uint32_t cur2 = body, guard = 0;
        while (cur2 != h && guard++ <= nb) {
          chain.push_back(cur2);
          cur2 = fn.succ0(cur2);
        }
        if (cur2 != h) ok = false;
      }
      if (ok) { // every outside-defined operand used inside is a constant
        for (uint32_t b2 = 0; b2 < nb && ok; ++b2) {
          if (!inL[b2]) continue;
          for (uint32_t i = fn.blocks[b2].begin;
               i < fn.blocks[b2].end && ok; ++i) {
            const Instr &I = fn.code[i];
            auto ck = [&](uint32_t v) {
              if (v != NONE && v < nv &&
                  !(defi[v] != NONE && inL[defb[v]]) && !kc[v])
                ok = false;
            };
            ck(I.a);
            ck(I.b2);
            ck(I.c2);
          }
        }
      }
      if (ok) {
        std::vector<val_t> val(nv, 0);
        for (const HPhi &p : hphis) val[p.dst] = kv[p.pre_arg];
        std::vector<val_t> nxt(hphis.size());
        bool trap = false, done = false;
        uint64_t steps = 0;
        auto eval = [&](const Instr &I) -> bool {
          switch (I.k) {
          case K::Const:
            val[I.dst] = I.c;
            return true;
          case K::Copy:
            val[I.dst] = val[I.a];
            return true;
          case K::Bin: {
            Rv r = w_bin(I.b, val[I.a], val[I.b2]);
            if (r.trap) return false;
            val[I.dst] = r.v;
            return true;
          }
          default:
            return false;
          }
        };
        for (uint64_t it = 0; it < LCF_TRIP_BUDGET && !trap && !done; ++it) {
          for (uint32_t i = fn.blocks[h].begin; i + 1 < fn.blocks[h].end;
               ++i) {
            if (fn.code[i].k == K::Phi) continue;
            if (!eval(fn.code[i])) trap = true;
          }
          if (trap) break;
          if (val[hterm.a] == 0) { // header test fails: exit with the
            done = true;           // current phi values as final values
            break;
          }
          for (uint32_t cb : chain) {
            for (uint32_t i = fn.blocks[cb].begin; i + 1 < fn.blocks[cb].end;
                 ++i) // skip the trailing Br (purity-checked above)
              if (!eval(fn.code[i])) trap = true;
            if (trap) break;
          }
          if (trap) break;
          if ((steps += 1 + chain.size()) > LCF_SIM_BUDGET) break;
          for (size_t pi = 0; pi < hphis.size(); ++pi)
            nxt[pi] = val[hphis[pi].latch_arg];
          for (size_t pi = 0; pi < hphis.size(); ++pi)
            val[hphis[pi].dst] = nxt[pi];
        }
        if (done && !trap) {
          // ---- commit: consts into the preheader; the loop dies --------
          std::vector<uint32_t> finalv(hphis.size(), NONE);
          std::vector<Instr> batch{};
          for (size_t pi = 0; pi < hphis.size(); ++pi) {
            const uint32_t fv = new_anon_vreg(fn, fn.base_of(hphis[pi].dst));
            finalv[pi] = fv;
            batch.push_back(
                {.k = K::Const, .dst = fv, .c = val[hphis[pi].dst]});
          }
          { // splice into the preheader, before its terminator
            uint32_t pe = fn.blocks[pre].end - 1;
            fn.code.insert(fn.code.begin() + (long)pe, batch.begin(),
                           batch.end());
            const uint32_t kk = (uint32_t)batch.size();
            for (uint32_t x = 0; x < nb; ++x) {
              if (x == pre) {
                fn.blocks[x].end += kk;
                continue;
              }
              if (fn.blocks[x].begin >= pe + 1) fn.blocks[x].begin += kk;
              if (fn.blocks[x].end >= pe + 1) fn.blocks[x].end += kk;
            }
          }
          os.lcf_gensims++;
          // rewire: the preheader jumps straight to the exit; loop dies
          for (uint32_t i = fn.blocks[pre].begin; i < fn.blocks[pre].end;
               ++i) {
            Instr &I = fn.code[i];
            if (I.k == K::Br && I.t == h) I.t = X;
            else if (I.k == K::Cbr) {
              if (I.t == h) I.t = X;
              if (I.f == h) I.f = X;
            }
          }
          if (fn.succ[2 * pre] == h) fn.succ[2 * pre] = X;
          if (fn.succ[2 * pre + 1] == h) fn.succ[2 * pre + 1] = X;
          for (uint32_t b2 = 0; b2 < nb; ++b2) {
            if (inL[b2]) continue;
            for (uint32_t i = fn.blocks[b2].begin; i < fn.blocks[b2].end;
                 ++i) {
              Instr &I = fn.code[i];
              auto rw = [&](uint32_t &v) {
                if (v == NONE || v >= nv) return;
                for (size_t pi = 0; pi < hphis.size(); ++pi)
                  if (v == hphis[pi].dst) {
                    v = finalv[pi];
                    return;
                  }
              };
              rw(I.a);
              rw(I.b2);
              rw(I.c2);
              for (uint32_t &av : I.args) rw(av);
            }
          }
          for (uint32_t i = fn.blocks[X].begin; i < fn.blocks[X].end; ++i) {
            Instr &I = fn.code[i]; // exit phis: h-edge departs from pre now
            if (I.k != K::Phi) break;
            if (I.t == h) I.t = pre;
            else if (I.f == h) I.f = pre;
          }
          fn.recompute_preds();
          return true; // one loop per call: the driver re-analyzes
        }
      }
      continue; // not general-simulable: try nothing else for this loop
    }
    const HPhi IP = hphis[ind_idx];
    const Instr C = fn.code[defi[cond]];
    const bool lhs_is_ind = (C.a == ind);
    const uint32_t Bv = lhs_is_ind ? C.b2 : C.a; // the bound operand
    const bool i0_const = kc[IP.pre_arg];
    const bool bound_const = (Bv != NONE && Bv < nv && kc[Bv]);
    // a runtime bound must be defined outside the loop (otherwise the trip
    // count depends on the body and no closed form exists)
    if (!bound_const) {
      if (Bv == NONE || Bv >= nv) continue;
      if (defi[Bv] != NONE && inL[defb[Bv]]) continue;
    }
    // ---- exact compile-time trip count N (when both sides are constant) ----
    uint64_t N = ~0ull;
    if (i0_const && bound_const) {
      val_t i0 = kv[IP.pre_arg];
      val_t bound = kv[Bv];
      val_t iv = i0;
      for (uint64_t k = 0;; ++k) {
        val_t lhs = lhs_is_ind ? iv : bound;
        val_t rhs = lhs_is_ind ? bound : iv;
        bool go = (C.b == B::Lt) ? (lhs < rhs) : (lhs <= rhs);
        if (!go) { N = k; break; }
        if (k >= LCF_TRIP_BUDGET) break;
        iv = step_sub ? w_sub(iv, step_mag).v : w_add(iv, step_mag).v;
      }
    }
    const bool ct_trip = (N != ~0ull);
    if (ct_trip && N == 0) { // zero trips: test provably false -> exit now
      Instr &T = fn.code[fn.blocks[h].end - 1];
      T.k = K::Br;
      T.t = X;
      T.f = NONE;
      T.a = NONE;
      fn.succ[2 * h] = X;
      fn.succ[2 * h + 1] = NONE;
      fn.recompute_preds();
      return true;
    }
    // ---- safety: outside uses may reference ONLY header phi dsts ----------
    {
      std::vector<char> phid(nv, 0);
      for (const HPhi &p : hphis) phid[p.dst] = 1;
      bool bad = false;
      for (uint32_t b = 0; b < nb && !bad; ++b) {
        if (inL[b]) continue;
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
          const Instr &I = fn.code[i];
          auto chk = [&](uint32_t v) {
            if (v != NONE && v < nv && defi[v] != NONE && inL[defb[v]] &&
                !phid[v])
              bad = true;
          };
          chk(I.a);
          chk(I.b2);
          chk(I.c2);
          for (uint32_t v : I.args) chk(v);
          if (bad) break;
        }
      }
      if (bad) continue;
    }
    // pre must not already reach the exit (phi rekey would duplicate preds)
    {
      bool bad = false;
      for (uint32_t p : fn.preds[X])
        if (p == pre) bad = true;
      if (bad) continue;
    }
    // ---- body purity: straight-line scalar math only ----------------------
    // (header: phis + Const/Copy/Bin + the Cbr; other blocks: Const/Copy/Bin
    //  ending in Br — no memory, calls, prints, or inner branches)
    {
      bool ok = true;
      for (uint32_t b = 0; b < nb && ok; ++b) {
        if (!inL[b]) continue;
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
          const Instr &I = fn.code[i];
          if (i + 1 == fn.blocks[b].end) { // terminator
            if (b == h) {
              if (I.k != K::Cbr) ok = false;
            } else if (I.k != K::Br)
              ok = false;
            break;
          }
          if (I.k == K::Phi) {
            if (b != h) ok = false;
          } else if (I.k == K::Const || I.k == K::Copy) {
          } else if (I.k == K::Bin) {
          } else
            ok = false;
          if (!ok) break;
        }
      }
      if (!ok) continue;
    }
    // ---- may-trap scan: Div/Mod that is not PROVEN trap-free ------------
    // The analytic closed form cannot preserve a runtime trap (it deletes the
    // loop); such loops are only foldable by the trap-checking SIMULATOR, or
    // once trap-elimination has marked every Div/Mod in the loop `nt`.
    bool loop_maytrap = false;
    for (uint32_t b = 0; b < nb && !loop_maytrap; ++b) {
      if (!inL[b]) continue;
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k != K::Bin || (I.b != B::Div && I.b != B::Mod)) continue;
        if (I.nt) continue; // proven trap-free by the range analysis
        if (!(I.b2 < nv && kc[I.b2] && kv[I.b2] != 0)) {
          loop_maytrap = true;
          break;
        }
      }
    }
    // ---- analytic: every phi must evolve as  x' = a*x + b*i + g ------------
    struct Evo { bool ok; uint64_t a, b, g; };
    std::vector<Evo> evo(hphis.size(), {false, 0, 0, 0});
    bool analytic = true;
    {
      std::vector<char> scope(nb, 0);
      for (uint32_t b = 0; b < nb; ++b) scope[b] = inL[b];
      for (size_t pi = 0; pi < hphis.size() && analytic; ++pi) {
        LinForm L{};
        std::vector<uint32_t> absr{};
        lin_expand(fn, nv, defi, defb, kc, kv, uses, ndefs, scope, NONE, L,
                   (val_t)1, hphis[pi].latch_arg, absr);
        (void)absr; // informational: the whole loop is removed anyway
        Evo e{true, 0, 0, (uint64_t)L.c};
        for (const auto &p : L.t) {
          if (p.first == hphis[pi].dst)
            e.a = (uint64_t)p.second;
          else if (p.first == ind)
            e.b = (uint64_t)p.second;
          else
            e.ok = false;
        }
        evo[pi] = e;
        analytic &= e.ok;
      }
    }
    std::vector<uint32_t> finalv(hphis.size(), NONE);
    bool folded = false;
    if (!ct_trip) {
      // ---- runtime trip count: guarded closed form + the loop as fallback -
      // Shape: `while (i < B) { pure body; i += d; }` with constant d >= 1
      // and a RUNTIME B (or a constant bound too big to step at compile
      // time). The trip count is computed at runtime together with a
      // wrap-safety guard; the guard fails exactly when the induction would
      // leave the signed range before reaching B (the honest answer is then
      // the original loop), so the closed form only ever runs when exact:
      //   c0 = (i0 < B);  D = B - i0 (wrapping);  Dm = c0 ? D : 0;
      //   N = ceil(Dm/d) [Lt]  or  Dm/d + c0 [Le];   iN = i0 + N*d;
      //   gok = !(c0 && ((D < 0) || (iN < B)))   [Lt; Le: iN <= B]
      // Every loop-carried x evolves additively (a == 1):
      //   x_N = x0 + (b*i0 + g)*N + (b*d) * N(N-1)/2
      // (T = N(N-1)/2 exactly mod 2^64 via the even-factor trick).
      bool rt_ok = !step_sub && step_mag >= 1 && lhs_is_ind;
      if (rt_ok) { // exit must have exactly one pred (the header), no phis
        if (fn.preds[X].size() != 1 || fn.preds[X][0] != h) rt_ok = false;
        for (uint32_t i = fn.blocks[X].begin; i < fn.blocks[X].end && rt_ok;
             ++i)
          if (fn.code[i].k == K::Phi) rt_ok = false;
        const Instr &pt = fn.code[fn.blocks[pre].end - 1];
        if (pt.k != K::Br || pt.t != h) rt_ok = false; // also idempotence
        for (const Evo &e : evo)
          if (!e.ok || e.a != 1) rt_ok = false; // additive recurrences only
      }
      if (!rt_ok) continue;
      // ---- vacuous-guard proof --------------------------------------------
      // With a CONSTANT i0 >= 0 and unit step, the induction can never wrap
      // before the exit: whenever the condition c0 holds, D = B - i0 lands
      // in (0, MAX] (no wrap, since B <= MAX and i0 >= 0), so `dneg' is
      // false; and iN = i0 + N reproduces B bit-exactly (N = Dm, and
      // i0 + (B - i0) = B in the mod-2^64 ring), so `bneg' is false too.
      // The guard is therefore identically false for EVERY runtime B. If
      // additionally the loop body is pure arithmetic that provably cannot
      // trap (Div/Mod carry `nt`) and observes nothing, the closed form is
      // exact unconditionally: emit it WITHOUT the guard and WITHOUT the
      // fallback loop (the now-unreachable loop is removed by unreach, and
      // its merge-phi args degenerate away).
      bool body_pure = true;
      for (uint32_t b2 = 0; b2 < nb && body_pure; ++b2) {
        if (!inL[b2]) continue;
        for (uint32_t i2 = fn.blocks[b2].begin; i2 < fn.blocks[b2].end;
             ++i2) {
          const Instr &BI = fn.code[i2];
          if (BI.k == K::Phi || BI.k == K::Const || BI.k == K::Copy ||
              BI.k == K::Br || BI.k == K::Cbr)
            continue;
          if (BI.k == K::Bin) {
            if ((BI.b == B::Div || BI.b == B::Mod) && !BI.nt) {
              body_pure = false; // a MAY-trap div/mod must run for real
              break;
            }
            continue;
          }
          body_pure = false; // Print/Call/Input/Alloc/Load/Store/Len/Ret
          break;
        }
      }
      const bool vacuous =
          body_pure && IP.pre_arg != NONE && IP.pre_arg < nv &&
          kc[IP.pre_arg] && kv[IP.pre_arg] >= 0 && step_mag == 1;
      const bool is_le = (C.b == B::Le);
      const uint32_t i0v = IP.pre_arg;
      const uint32_t G = fn.nblocks();
      const uint32_t CF = G + 1;
      fn.blocks.push_back({});
      fn.blocks.push_back({});
      fn.succ.push_back(NONE);
      fn.succ.push_back(NONE);
      fn.succ.push_back(NONE);
      fn.succ.push_back(NONE);
      std::vector<Instr> gi{}, cfi{};
      auto konst = [&](std::vector<Instr> &out, val_t c) {
        uint32_t v = new_anon_vreg(fn, 0);
        out.push_back({.k = K::Const, .dst = v, .c = c});
        return v;
      };
      auto bin = [&](std::vector<Instr> &out, B b, uint32_t dst, uint32_t a,
                     uint32_t b2) {
        out.push_back({.k = K::Bin, .b = b, .dst = dst, .a = a, .b2 = b2});
      };
      auto nt = [&]() { return new_anon_vreg(fn, 0); };
      // ---- guard block G: N, iN, gok ----
      const uint32_t z0 = konst(gi, 0);
      const uint32_t c0 = nt();
      bin(gi, is_le ? B::Le : B::Lt, c0, i0v, Bv);
      const uint32_t Dv = nt();
      bin(gi, B::Sub, Dv, Bv, i0v);
      const uint32_t msk = nt();
      bin(gi, B::Sub, msk, z0, c0);
      const uint32_t Dm = nt();
      bin(gi, B::And, Dm, Dv, msk);
      const uint32_t kd = konst(gi, step_mag);
      const uint32_t qv = nt();
      bin(gi, B::Div, qv, Dm, kd);
      const uint32_t Nv = nt();
      if (is_le) {
        bin(gi, B::Add, Nv, qv, c0);
      } else {
        const uint32_t rv = nt();
        bin(gi, B::Mod, rv, Dm, kd);
        const uint32_t rnz = nt();
        bin(gi, B::Ne, rnz, rv, z0);
        bin(gi, B::Add, Nv, qv, rnz);
      }
      if (vacuous) { // closed form only: no guard, no fallback loop
        gi.push_back({.k = K::Br, .t = CF});
      } else {
        const uint32_t diN = nt();
        bin(gi, B::Mul, diN, Nv, kd);
        const uint32_t iN = nt();
        bin(gi, B::Add, iN, i0v, diN);
        const uint32_t dneg = nt();
        bin(gi, B::Lt, dneg, Dv, z0);
        const uint32_t bneg = nt();
        bin(gi, is_le ? B::Le : B::Lt, bneg, iN, Bv);
        const uint32_t ov = nt();
        bin(gi, B::Or, ov, dneg, bneg);
        const uint32_t badv = nt();
        bin(gi, B::And, badv, c0, ov);
        const uint32_t gok = nt();
        bin(gi, B::Eq, gok, badv, z0);
        gi.push_back({.k = K::Cbr, .a = gok, .t = CF, .f = h});
      }
      // ---- closed-form block CF ----
      uint32_t Tv = NONE;
      for (const Evo &e : evo) // T is needed iff some increment uses i
        if (e.b != 0) {
          const uint32_t one = konst(cfi, 1);
          const uint32_t e0 = nt();
          bin(cfi, B::And, e0, Nv, one);
          const uint32_t two = nt();
          bin(cfi, B::Add, two, e0, e0);
          const uint32_t Nm = nt();
          bin(cfi, B::Sub, Nm, Nv, e0);
          const uint32_t h2 = nt();
          bin(cfi, B::LShr, h2, Nm, one);
          const uint32_t t1 = nt();
          bin(cfi, B::Add, t1, Nm, two);
          const uint32_t o1 = nt();
          bin(cfi, B::Sub, o1, t1, one);
          Tv = nt();
          bin(cfi, B::Mul, Tv, h2, o1);
          break;
        }
      std::vector<uint32_t> cfv(hphis.size(), NONE);
      for (size_t pi = 0; pi < hphis.size(); ++pi) {
        const Evo &e = evo[pi];
        const uint32_t x0 = hphis[pi].pre_arg;
        if (e.b == 0 && e.g == 0) {
          cfv[pi] = x0; // untouched by the loop
          continue;
        }
        uint32_t Cv = NONE; // b*i0 + g
        if (e.b == 0) {
          Cv = konst(cfi, (val_t)e.g);
        } else {
          uint32_t bi = i0v;
          if (e.b != 1) {
            uint32_t kb = konst(cfi, (val_t)e.b);
            uint32_t t = nt();
            bin(cfi, B::Mul, t, i0v, kb);
            bi = t;
          }
          if (e.g == 0) {
            Cv = bi;
          } else {
            uint32_t kg = konst(cfi, (val_t)e.g);
            Cv = nt();
            bin(cfi, B::Add, Cv, bi, kg);
          }
        }
        uint32_t acc = x0;
        { // acc += C * N
          uint32_t cn = nt();
          bin(cfi, B::Mul, cn, Cv, Nv);
          uint32_t a2 = nt();
          bin(cfi, B::Add, a2, acc, cn);
          acc = a2;
        }
        if (e.b != 0) { // acc += (b*d) * T
          uint32_t kbd = konst(cfi, (val_t)(e.b * (uint64_t)step_mag));
          uint32_t bt = nt();
          bin(cfi, B::Mul, bt, Tv, kbd);
          uint32_t a3 = nt();
          bin(cfi, B::Add, a3, acc, bt);
          acc = a3;
        }
        cfv[pi] = acc;
      }
      cfi.push_back({.k = K::Br, .t = X});
      // ---- commit: append G/CF, rewrite outside uses, insert merge phis ----
      fn.blocks[G].begin = (uint32_t)fn.code.size();
      for (const Instr &x : gi) fn.code.push_back(x);
      fn.blocks[G].end = (uint32_t)fn.code.size();
      fn.blocks[CF].begin = (uint32_t)fn.code.size();
      for (const Instr &x : cfi) fn.code.push_back(x);
      fn.blocks[CF].end = (uint32_t)fn.code.size();
      fn.succ[2 * G] = CF;
      fn.succ[2 * G + 1] = vacuous ? NONE : h; // vacuous: G never enters h
      fn.succ[2 * CF] = X;
      fn.succ[2 * CF + 1] = NONE;
      { // outside uses of the loop phis -> merge-phi values (below); done
        // BEFORE the phis exist so their own loop-side args stay untouched
        std::vector<uint32_t> phidst(hphis.size(), NONE);
        for (size_t pi = 0; pi < hphis.size(); ++pi)
          phidst[pi] = new_anon_vreg(fn, fn.base_of(hphis[pi].dst));
        for (uint32_t b = 0; b < nb; ++b) {
          if (inL[b]) continue;
          for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
            Instr &I = fn.code[i];
            auto rw = [&](uint32_t &v) {
              if (v == NONE || v >= nv) return;
              for (size_t pi = 0; pi < hphis.size(); ++pi)
                if (v == hphis[pi].dst) {
                  v = phidst[pi];
                  return;
                }
            };
            rw(I.a);
            rw(I.b2);
            rw(I.c2);
            for (uint32_t &av : I.args) rw(av);
          }
        }
        std::vector<Instr> xphis{};
        for (size_t pi = 0; pi < hphis.size(); ++pi)
          xphis.push_back({.k = K::Phi, .dst = phidst[pi],
                           .a = hphis[pi].dst, .b2 = cfv[pi],
                           .t = h, .f = CF});
        uint32_t xpos = fn.blocks[X].begin;
        fn.code.insert(fn.code.begin() + (long)xpos, xphis.begin(),
                       xphis.end());
        const uint32_t k = (uint32_t)xphis.size();
        for (uint32_t b = 0; b < fn.nblocks(); ++b) {
          if (b == X) {
            fn.blocks[b].end += k;
            continue;
          }
          if (fn.blocks[b].begin > xpos) fn.blocks[b].begin += k;
          if (fn.blocks[b].end > xpos) fn.blocks[b].end += k;
        }
      }
      { // preheader now enters the guard instead of the header
        for (uint32_t i = fn.blocks[pre].begin; i < fn.blocks[pre].end; ++i) {
          Instr &I = fn.code[i];
          if (I.k == K::Br && I.t == h) I.t = G;
        }
        if (fn.succ[2 * pre] == h) fn.succ[2 * pre] = G;
        if (fn.succ[2 * pre + 1] == h) fn.succ[2 * pre + 1] = G;
        // the header phis key their entry arg by `pre`; that edge is now
        // G->h, so the keys must follow -- otherwise SSA destruction drops
        // the entry values on the (only) path into the fallback loop and
        // the loop would start from garbage phi state
        for (uint32_t i = fn.blocks[h].begin; i < fn.blocks[h].end; ++i) {
          Instr &P = fn.code[i];
          if (P.k != K::Phi) break;
          if (P.t == pre) P.t = G;
          else if (P.f == pre) P.f = G;
        }
      }
      fn.recompute_preds();
      os.lcf_rt++;
      return true; // one loop per call: the fixpoint driver re-analyzes
    }
    auto splice_into_preheader = [&](std::vector<Instr> &batch) {
      uint32_t pe = fn.blocks[pre].end - 1; // terminator (pre-insert)
      fn.code.insert(fn.code.begin() + (long)pe, batch.begin(), batch.end());
      const uint32_t k = (uint32_t)batch.size();
      for (uint32_t x = 0; x < nb; ++x) {
        if (x == pre) {
          fn.blocks[x].end += k;
          continue;
        }
        if (fn.blocks[x].begin >= pe + 1) fn.blocks[x].begin += k;
        if (fn.blocks[x].end >= pe + 1) fn.blocks[x].end += k;
      }
    };
    if (analytic && !loop_maytrap) {
      // closed form: x_N = K1*x0 + K0 with K1 = a^N and the series constants
      std::vector<Instr> batch{};
      const uint64_t Nm1 = N - 1;
      const uint64_t i0u = (uint64_t)kv[IP.pre_arg];
      const uint64_t du =
          step_sub ? (uint64_t)0 - (uint64_t)step_mag : (uint64_t)step_mag;
      for (size_t pi = 0; pi < hphis.size(); ++pi) {
        const Evo &e = evo[pi];
        uint64_t S = 0, T = 0, P = 0;
        lcf_geo(e.a, N, S, T, P);
        const uint64_t K1 = P; // a^N
        const uint64_t Cc = e.b * i0u + e.g;
        const uint64_t Bc = e.b * du;
        const uint64_t K0 = (Cc + Bc * Nm1) * S - Bc * T;
        const uint32_t x0 = hphis[pi].pre_arg;
        const uint32_t fv = new_anon_vreg(fn, fn.base_of(hphis[pi].dst));
        finalv[pi] = fv;
        if (K1 == 1 && K0 == 0) {
          batch.push_back({.k = K::Copy, .dst = fv, .a = x0});
        } else if (K1 == 1) {
          uint32_t c = new_anon_vreg(fn, 0);
          batch.push_back({.k = K::Const, .dst = c, .c = (val_t)K0});
          batch.push_back({.k = K::Bin, .b = B::Add, .dst = fv, .a = x0,
                           .b2 = c});
        } else if (K0 == 0) {
          uint32_t c = new_anon_vreg(fn, 0);
          batch.push_back({.k = K::Const, .dst = c, .c = (val_t)K1});
          batch.push_back({.k = K::Bin, .b = B::Mul, .dst = fv, .a = x0,
                           .b2 = c});
        } else {
          uint32_t c1 = new_anon_vreg(fn, 0);
          uint32_t t = new_anon_vreg(fn, fn.base_of(hphis[pi].dst));
          batch.push_back({.k = K::Const, .dst = c1, .c = (val_t)K1});
          batch.push_back({.k = K::Bin, .b = B::Mul, .dst = t, .a = x0,
                           .b2 = c1});
          uint32_t c2 = new_anon_vreg(fn, 0);
          batch.push_back({.k = K::Const, .dst = c2, .c = (val_t)K0});
          batch.push_back(
              {.k = K::Bin, .b = B::Add, .dst = fv, .a = t, .b2 = c2});
        }
      }
      splice_into_preheader(batch);
      os.lcf_loops++;
      folded = true;
    } else {
      // ---- fallback: exact compile-time simulation of all N iterations -----
      bool ok = true;
      for (const HPhi &p : hphis)
        if (!(p.pre_arg < nv && kc[p.pre_arg])) ok = false;
      uint64_t loop_instrs = 0;
      for (uint32_t b = 0; b < nb; ++b)
        if (inL[b]) loop_instrs += fn.blocks[b].end - fn.blocks[b].begin;
      if (ok && N * loop_instrs > LCF_SIM_BUDGET) ok = false;
      if (ok) { // every outside-defined operand must be a known constant
        for (uint32_t b = 0; b < nb && ok; ++b) {
          if (!inL[b]) continue;
          for (uint32_t i = fn.blocks[b].begin;
               i < fn.blocks[b].end && ok; ++i) {
            const Instr &I = fn.code[i];
            auto ck = [&](uint32_t v) {
              if (v != NONE && v < nv &&
                  !(defi[v] != NONE && inL[defb[v]]) && !kc[v])
                ok = false;
            };
            ck(I.a);
            ck(I.b2);
            ck(I.c2);
          }
        }
      }
      if (ok) { // the straight-line body chain, from `body` to the latch
        std::vector<uint32_t> chain{};
        uint32_t cur = body, guard = 0;
        while (cur != h && guard++ <= nb) {
          chain.push_back(cur);
          cur = fn.succ0(cur);
        }
        if (cur != h) ok = false;
        if (ok) {
          std::vector<val_t> val(nv, 0);
          for (const HPhi &p : hphis) val[p.dst] = kv[p.pre_arg];
          std::vector<val_t> nxt(hphis.size());
          bool trap = false;
          auto eval = [&](const Instr &I) -> bool {
            switch (I.k) {
            case K::Const:
              val[I.dst] = I.c;
              return true;
            case K::Copy:
              val[I.dst] = val[I.a];
              return true;
            case K::Bin: {
              Rv r = w_bin(I.b, val[I.a], val[I.b2]);
              if (r.trap) return false;
              val[I.dst] = r.v;
              return true;
            }
            default:
              return false;
            }
          };
          for (uint64_t it = 0; it < N && !trap; ++it) {
            for (uint32_t i = fn.blocks[h].begin;
                 i + 1 < fn.blocks[h].end; ++i) {
              if (fn.code[i].k == K::Phi) continue;
              if (!eval(fn.code[i])) trap = true;
            }
            if (trap) break;
            for (uint32_t cb : chain) {
              for (uint32_t i = fn.blocks[cb].begin; i < fn.blocks[cb].end;
                   ++i)
                if (!eval(fn.code[i])) trap = true;
              if (trap) break;
            }
            if (trap) break;
            for (size_t pi = 0; pi < hphis.size(); ++pi)
              nxt[pi] = val[hphis[pi].latch_arg];
            for (size_t pi = 0; pi < hphis.size(); ++pi)
              val[hphis[pi].dst] = nxt[pi];
          }
          if (!trap) {
            std::vector<Instr> batch{};
            for (size_t pi = 0; pi < hphis.size(); ++pi) {
              const uint32_t fv =
                  new_anon_vreg(fn, fn.base_of(hphis[pi].dst));
              finalv[pi] = fv;
              batch.push_back(
                  {.k = K::Const, .dst = fv, .c = val[hphis[pi].dst]});
            }
            splice_into_preheader(batch);
            os.lcf_sims++;
            folded = true;
          }
        }
      }
    }
    if (!folded) continue;
    // ---- rewire: preheader jumps straight to the exit; loop dies -----------
    for (uint32_t i = fn.blocks[pre].begin; i < fn.blocks[pre].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k == K::Br && I.t == h) I.t = X;
      else if (I.k == K::Cbr) {
        if (I.t == h) I.t = X;
        if (I.f == h) I.f = X;
      }
    }
    if (fn.succ[2 * pre] == h) fn.succ[2 * pre] = X;
    if (fn.succ[2 * pre + 1] == h) fn.succ[2 * pre + 1] = X;
    for (uint32_t b = 0; b < nb; ++b) { // outside uses of phis -> final values
      if (inL[b]) continue;
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        Instr &I = fn.code[i];
        auto rw = [&](uint32_t &v) {
          if (v == NONE || v >= nv) return;
          for (size_t pi = 0; pi < hphis.size(); ++pi)
            if (v == hphis[pi].dst) {
              v = finalv[pi];
              return;
            }
        };
        rw(I.a);
        rw(I.b2);
        rw(I.c2);
        for (uint32_t &av : I.args) rw(av);
      }
    }
    for (uint32_t i = fn.blocks[X].begin; i < fn.blocks[X].end; ++i) {
      Instr &I = fn.code[i]; // exit phis: the h-edge now departs from pre
      if (I.k != K::Phi) break;
      if (I.t == h) I.t = pre;
      else if (I.f == h) I.f = pre;
    }
    fn.recompute_preds();
    return true; // one loop per call: the driver re-analyzes
  }
  return false;
}

// After passes convert phis to Copy/Const in place, the "all phis first"
// block invariant breaks — consumers scan until the first non-phi. Restore
// it by stably moving remaining phis to the front of each block.
constexpr void normalize_phis(Fn &fn) {
  for (uint32_t b = 0; b < fn.nblocks(); ++b) {
    uint32_t begin = fn.blocks[b].begin, end = fn.blocks[b].end;
    if (end - begin < 2) continue;
    bool need = false;
    for (uint32_t i = begin; i < end; ++i) {
      if (fn.code[i].k == K::Phi) continue;
      for (uint32_t j = i + 1; j < end; ++j)
        if (fn.code[j].k == K::Phi) { need = true; break; }
      break;
    }
    if (!need) continue;
    std::vector<Instr> phis{}, rest{};
    for (uint32_t i = begin; i < end; ++i)
      (fn.code[i].k == K::Phi ? phis : rest).push_back(fn.code[i]);
    uint32_t w = begin;
    for (const Instr &I : phis) fn.code[w++] = I;
    for (const Instr &I : rest) fn.code[w++] = I;
  }
}

// ============================================================================
// 9e3. CONST-CALL EVALUATION (interprocedural)
// ============================================================================
// A call whose arguments are all compile-time constants and whose callee (and
// every fn that callee transitively calls) is PURE ARITHMETIC -- only
// Const/Copy/Bin/Br/Cbr/Phi/Call/Ret, so no Print, no input(), no arena
// (Alloc/Load/Store/Len), no observable side effect and no handle value -- is
// evaluated exactly at compile time with the same wrapping/trapping arithmetic
// the runtime uses (a trap, the 512-frame recursion cap, or the step budget
// aborts the attempt and keeps the runtime call: a runtime TRAP is
// observable, so folding it away would be a miscompile). This is what turns
// `fact(4)` and the mutually recursive `is_even(10)` into constants:
// recursion is exactly why inlining can never touch them, and exactly why a
// whole-program evaluator can -- the recursive call graph is just a
// control-flow graph the evaluator walks.

// call depth cap shared by EVERY runner (interpreters, VM, emitted C,
// and emitted asm all enforce the same limit: runaway recursion TRAPs
// everywhere) -- also enforced by the compile-time evaluator below, so a
// program that would TRAP at depth 512 is never silently folded
inline constexpr uint32_t MJZ_MAX_CALL_DEPTH = 512;
// step budget for one const-call evaluation attempt (shared across the
// whole recursive invocation; generous: the demo programs need < 200)
inline constexpr uint64_t MJZ_CONSTCALL_STEPS = 1ull << 20;

// fn-local purity: every instruction is pure arithmetic / control flow
constexpr bool fn_local_pure(const Fn &f) {
  for (const Instr &I : f.code)
    switch (I.k) {
    case K::Const:
    case K::Copy:
    case K::Bin:   // Div/Mod may trap -> the EVALUATOR aborts on real traps
    case K::Br:
    case K::Cbr:
    case K::Phi:
    case K::Call:  // nested calls are checked transitively
    case K::Ret:
      break;
    default:
      return false; // Print/Input/Alloc/Load/Store/Len/Exit: observable
    }
  return true;
}
// transitive purity over the call graph (cycle-safe DFS)
constexpr bool fn_pure_closure(const std::vector<Fn> &fns, uint32_t fi) {
  const uint32_t n = (uint32_t)fns.size();
  std::vector<uint8_t> seen(n, 0);
  std::vector<uint32_t> work{fi};
  seen[fi] = 1;
  while (!work.empty()) {
    uint32_t f = work.back();
    work.pop_back();
    if (!fn_local_pure(fns[f])) return false;
    for (const Instr &I : fns[f].code)
      if (I.k == K::Call) {
        uint32_t g = (uint32_t)I.c;
        if (g < n && !seen[g]) {
          seen[g] = 1;
          work.push_back(g);
        }
      }
  }
  return true;
}

// exact SSA execution of one pure fn; mirrors run_prog's semantics: phis are
// resolved on the edge they came from (read all, then write all), the env is
// flat with default 0 ("storage is zero-initialized"), Ret yields val[a].
constexpr bool eval_pure_fn(const std::vector<Fn> &fns, uint32_t fi,
                            const std::vector<val_t> &args, val_t &out,
                            uint32_t depth, uint64_t &steps) {
  if (depth >= MJZ_MAX_CALL_DEPTH) return false; // runtime would TRAP
  const Fn &F = fns[fi];
  if (F.nblocks() == 0) return false;
  const uint32_t nv = F.nvars + (uint32_t)F.version_base.size();
  std::vector<val_t> val(nv, 0);
  for (size_t k = 0; k < args.size() && k < F.nparams; ++k)
    if (k < nv) val[k] = args[k];
  uint32_t blk = 0;
  uint32_t pc = F.blocks[0].begin;
  for (;;) {
    if (++steps > MJZ_CONSTCALL_STEPS) return false; // budget exhausted
    if (blk >= F.nblocks() || pc >= F.blocks[blk].end) return false;
    const Instr &I = F.code[pc];
    switch (I.k) {
    case K::Phi: // resolved on the incoming edge, never inline
      ++pc;
      continue;
    case K::Const:
      if (I.dst >= nv) return false;
      val[I.dst] = I.c;
      ++pc;
      continue;
    case K::Copy:
      if (I.dst >= nv || I.a >= nv) return false;
      val[I.dst] = val[I.a];
      ++pc;
      continue;
    case K::Bin: {
      if (I.a >= nv || I.b2 >= nv || I.dst >= nv) return false;
      Rv r = w_bin(I.b, val[I.a], val[I.b2]);
      if (r.trap) return false; // runtime trap is observable: keep the call
      val[I.dst] = r.v;
      ++pc;
      continue;
    }
    case K::Br:
    case K::Cbr: {
      uint32_t next = (I.k == K::Br)
                          ? I.t
                          : ((val[I.a] != 0) ? I.t : I.f);
      if (next >= F.nblocks()) return false;
      // stage the successor's phis for THIS edge: read all, then write all
      std::vector<std::pair<uint32_t, val_t>> staged{};
      for (uint32_t j = F.blocks[next].begin; j < F.blocks[next].end; ++j) {
        const Instr &P = F.code[j];
        if (P.k != K::Phi) break;
        uint32_t arg = NONE;
        if (P.t == blk && P.a != NONE) arg = P.a;
        else if (P.f == blk && P.b2 != NONE) arg = P.b2;
        if (arg != NONE && arg < nv && P.dst < nv)
          staged.push_back({P.dst, val[arg]});
      }
      for (const auto &st : staged) val[st.first] = st.second;
      blk = next;
      pc = F.blocks[blk].begin;
      continue;
    }
    case K::Ret:
      out = (I.a != NONE && I.a < nv) ? val[I.a] : 0;
      return true;
    case K::Call: {
      std::vector<val_t> cargs{};
      for (uint32_t a : I.args) {
        if (a >= nv) return false;
        cargs.push_back(val[a]);
      }
      val_t r{};
      if (!eval_pure_fn(fns, (uint32_t)I.c, cargs, r, depth + 1, steps))
        return false;
      if (I.dst < nv) val[I.dst] = r;
      ++pc;
      continue;
    }
    default:
      return false; // impure instruction: impossible after the purity check
    }
  }
}

// fold every constant-argument call to a provably-pure callee
constexpr bool opt_constcall(std::vector<Fn> &fns,
                             std::vector<OptStats> &ostats) {
  const uint32_t n = (uint32_t)fns.size();
  if (n < 2 || ostats.size() < n) return false;
  std::vector<uint8_t> pure(n, 0);
  for (uint32_t f = 0; f < n; ++f)
    pure[f] = fn_pure_closure(fns, f) ? 1 : 0;
  bool changed = false;
  for (uint32_t f = 0; f < n; ++f) {
    Fn &F = fns[f];
    const uint32_t nv = F.nvars + (uint32_t)F.version_base.size();
    if (!nv || F.nblocks() == 0) continue;
    // def index (SSA: one def per vreg) + bounded const-value walk
    std::vector<uint32_t> def_at(nv, NONE);
    for (uint32_t b = 0; b < F.nblocks(); ++b)
      for (uint32_t i = F.blocks[b].begin; i < F.blocks[b].end; ++i) {
        const Instr &I = F.code[i];
        if (I.dst != NONE && I.dst < nv) def_at[I.dst] = i;
      }
    auto cval = [&](uint32_t v, val_t &out) -> bool {
      for (int d = 0; d < 16; ++d) { // bounded Copy/phi-of-same chains
        if (v == NONE || v >= nv) return false;
        uint32_t at = def_at[v];
        if (at == NONE) return false;
        const Instr &D = F.code[at];
        if (D.k == K::Const) {
          out = D.c;
          return true;
        }
        if (D.k == K::Copy && D.a != NONE) {
          v = D.a;
          continue;
        }
        if (D.k == K::Phi && D.a != NONE && D.a == D.b2) {
          v = D.a;
          continue;
        }
        return false;
      }
      return false;
    };
    for (uint32_t b = 0; b < F.nblocks(); ++b)
      for (uint32_t i = F.blocks[b].begin; i < F.blocks[b].end; ++i) {
        Instr &I = F.code[i];
        if (I.k != K::Call) continue;
        uint32_t g = (uint32_t)I.c;
        if (g >= n || !pure[g] || I.args.size() != fns[g].nparams) continue;
        std::vector<val_t> args{};
        bool allc = true;
        for (uint32_t a : I.args) {
          val_t v{};
          if (!cval(a, v)) {
            allc = false;
            break;
          }
          args.push_back(v);
        }
        if (!allc) continue;
        val_t out{};
        uint64_t steps = 0;
        if (!eval_pure_fn(fns, g, args, out, 0, steps)) continue;
        // same dst: every user reads the constant directly
        I.k = K::Const;
        I.b = B::Add;
        I.a = NONE;
        I.b2 = NONE;
        I.t = NONE;
        I.f = NONE;
        I.c = out;
        I.c2 = NONE;
        I.nt = false;
        I.args.clear();
        ostats[f].constcalls++;
        changed = true;
      }
  }
  return changed;
}

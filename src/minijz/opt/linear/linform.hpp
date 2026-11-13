#pragma once
// === linform.hpp ========================================
// linear forms: shared by reassociation & loop closed forms.
//
// ALGORITHM: lin_term/lin_const keep the form sum(coeff_i * v_i) + K
// (same-vreg terms merge, a zero coefficient deletes one, constants
// fold into LinForm::c); lin_expand absorbs a def chain recursively
// -- constants (kc/kv), Add, Sub (negated k), Mul-by-constant and
// Copy -- leaving everything else a leaf term; lin_cost prices the
// minimal sequence and lin_emit materializes it, landing dst by
// rewriting the last instruction IT emitted, never an earlier def.
//
// DATA STRUCTURES: LinForm::t is the (vreg, coeff) list, sorted by
// vreg id -- the canonical order that lets equal chains compare
// equal; the scan tables defi/defb/kc/kv/uses/ndefs/scope are
// caller-owned; `absorbed` records folded instrs; use_idx enforces
// def-before-use in a block-local scope.
//
// THEORY: two's-complement +,-,* are exact mod 2^64 and never trap,
// so collecting a chain and re-emitting it is value-preserving. In
// analysis mode the same expansion reads the affine loop recurrence
// x -> a*x + b*i + g: a is the coefficient on the feedback value, b
// the one on the induction variable, g = L.c -- exactly what
// absorbing the (always multi-use) induction update produces, and
// what lcf's series algebra (a^n, S(n), T(n)) and runtime Tv
// consume. One engine serves both modes; only the use gate differs:
// reassociation RE-emits what it expands, so a def with other users
// must stay put (uses[v] == 1); the lcf scan only READS the form
// (the loop is deleted on commit) and a def's value is independent
// of its user count, so `analysis` admits multi-use defs.
//
// DOWNSTREAM: the callers are opt_reassoc (rewrite mode) and lcf
// (analysis mode, the recurrence scan).

#include "../../ir/instr.hpp"

// --- 9b2. linear forms: shared by reassociation & loop closed forms ------
// A linear form is  sum(coeff_i * v_i) + K  with terms sorted by vreg id.
// Two's-complement +,-,* are exact mod 2^64 and never trap, so collecting
// and re-emitting them is always sound.
struct LinForm {
  std::vector<std::pair<uint32_t, val_t>> t{}; // (vreg, coeff != 0), sorted
  val_t c{};
};
constexpr void lin_term(LinForm &L, val_t k, uint32_t v) {
  if (k == 0)
    return;
  for (size_t i = 0; i < L.t.size(); ++i) {
    if (L.t[i].first == v) {
      L.t[i].second = (val_t)((uint64_t)L.t[i].second + (uint64_t)k);
      if (L.t[i].second == 0)
        L.t.erase(L.t.begin() + (long)i);
      return;
    }
  }
  size_t i = 0;
  while (i < L.t.size() && L.t[i].first < v)
    ++i;
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
// `analysis` (the recurrence scan's mode, checkpoint 20): multi-use defs
// absorb too. Reassociation RE-emits the chain it expands, so absorbing a
// shared def there would duplicate or delete live computation; the lcf
// recurrence scan only READS the linear form (the whole loop is deleted
// when the closed form commits), and a def's VALUE does not depend on how
// many users it has -- absorbing the induction update (whose users are the
// feedback phi + the accumulator it feeds: always >= 2 uses) is exactly
// what turns `while (i < B) { s = s + i; i = i + 1; }` into the sum-of-series
// recurrence s' = s + i + 1 (coefficient on the induction), which the
// analytic T(n) series and the runtime Tv construction both consume.
constexpr void
lin_expand(const Fn &fn, uint32_t nv, const std::vector<uint32_t> &defi,
           const std::vector<uint32_t> &defb, const std::vector<char> &kc,
           const std::vector<val_t> &kv, const std::vector<uint32_t> &uses,
           const std::vector<uint32_t> &ndefs, const std::vector<char> &scope,
           uint32_t use_idx, LinForm &L, val_t k, uint32_t v,
           std::vector<uint32_t> &absorbed, bool analysis = false) {
  if (v == NONE || k == 0)
    return;
  if (v < nv && kc[v]) {
    lin_const(L, k, kv[v]);
    return;
  }
  if (v < nv && defi[v] != NONE && ndefs[v] == 1 &&
      (uses[v] == 1 || analysis) && defb[v] < (uint32_t)scope.size() &&
      scope[defb[v]] && (use_idx == NONE || defi[v] < use_idx)) {
    const Instr &D = fn.code[defi[v]];
    if (D.k == K::Bin && D.b == B::Add) {
      lin_expand(fn, nv, defi, defb, kc, kv, uses, ndefs, scope, defi[v], L, k,
                 D.a, absorbed, analysis);
      lin_expand(fn, nv, defi, defb, kc, kv, uses, ndefs, scope, defi[v], L, k,
                 D.b2, absorbed, analysis);
      absorbed.push_back(defi[v]);
      return;
    }
    if (D.k == K::Bin && D.b == B::Sub) {
      lin_expand(fn, nv, defi, defb, kc, kv, uses, ndefs, scope, defi[v], L, k,
                 D.a, absorbed, analysis);
      lin_expand(fn, nv, defi, defb, kc, kv, uses, ndefs, scope, defi[v], L,
                 (val_t)(0 - (uint64_t)k), D.b2, absorbed, analysis);
      absorbed.push_back(defi[v]);
      return;
    }
    if (D.k == K::Bin && D.b == B::Mul) {
      if (D.b2 != NONE && D.b2 < nv && kc[D.b2]) {
        lin_expand(fn, nv, defi, defb, kc, kv, uses, ndefs, scope, defi[v], L,
                   (val_t)((uint64_t)k * (uint64_t)kv[D.b2]), D.a, absorbed,
                   analysis);
        absorbed.push_back(defi[v]);
        return;
      }
      if (D.a != NONE && D.a < nv && kc[D.a]) {
        lin_expand(fn, nv, defi, defb, kc, kv, uses, ndefs, scope, defi[v], L,
                   (val_t)((uint64_t)k * (uint64_t)kv[D.a]), D.b2, absorbed,
                   analysis);
        absorbed.push_back(defi[v]);
        return;
      }
    }
    if (D.k == K::Copy && D.a != NONE) { // identity through a copy
      lin_expand(fn, nv, defi, defb, kc, kv, uses, ndefs, scope, defi[v], L, k,
                 D.a, absorbed, analysis);
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
    if (p.second == 0)
      continue;
    ++terms;
    if (first) {
      n += (p.second == 1) ? 0 : 2; // Mul by const k (any sign) or bare v
      first = false;
    } else {
      n += (p.second == 1 || p.second == -1) ? 1 : 3; // (Const+Mul) + Add/Sub
    }
  }
  if (!terms)
    return 1; // pure constant
  if (L.c != 0)
    n += 2;         // Const + Add
  return n ? n : 1; // single +x term: Copy
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
    if (p.second != 0)
      ts.push_back(p);
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
    out.push_back({.k = K::Bin,
                   .b = (k == -1) ? B::Sub : B::Add,
                   .dst = r,
                   .a = acc,
                   .b2 = t});
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

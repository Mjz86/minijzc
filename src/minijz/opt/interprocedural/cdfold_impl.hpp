#pragma once
// === cdfold_impl.hpp ========================================
// the countdown-fold SUPPORT layer, split out of cdfold.hpp (the code-
// quality continuation: "break the big functions, break the big files"):
// the CdAff algebra, the bounded resolver, the loop-shape matchers (one
// small constexpr function per phase of the former 642-line matcher),
// and the two-buffer emitter (guard / closed form). The slim driver --
// the per-header loop -- stays in cdfold.hpp.
//
// Byte-identical contract: the phase order, the def-site scan bounds
// (`nb`, the PASS-ENTRY block count, vs fn.nblocks(), the CURRENT one),
// the validation order, and the vreg allocation order are exactly the
// original's -- the fuzz fixtures and the register audit regenerate
// unchanged.

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../analysis/an.hpp"
#include "../linear/linform.hpp"

// ============================================================================
// 9e5. COUNTDOWN LOOP CLOSED FORM (cdfold) -- support layer
// ============================================================================
// The loop shape tail-call elimination emits (after the cheap tiers have
// spliced and folded it) is a COUNTDOWN with an exact recursion budget:
//
//   pre:  bInit = CAP+1 - Depth(); [sel0 = Const]      (entry state)
//   H:    selH = phi[sel0][selL]; nP = phi[n0][nL]; bH = phi[bInit][bL]
//         cbr (selH == 1) ? E_t : E_f                  (member dispatch)
//   E_m:  cbr (nP == 0) ? X_m : A_m                    (the base test)
//   X_m:  [K_m = Const]; br NEXT / ret K_m             (the member's value)
//   A_m:  n' = nP - 1; b' = bH - 1; cbr (bH == 0) ? T : L   (budget check)
//   L:    selL = phi[c_t][c_f]; nL = phi[n'][n']; bL = phi[b'][b']; br H
//   T:    1 / 0                                        (the depth TRAP)
//
// The closed form is exact: N = (u64)n0 iterations (n hits 0 iff it ever
// does -- else the budget fires first), sel_N = toggle^N(sel0) with the
// toggle closed form (1-2p)*sel0 + p, p = N&1 (EXACT for every sel0, and
// sel stays in {0,1} when sel0 is, which is when the final select is
// sound), and the guard `(u64)n0 > (u64)bInit` jumps to the SAME trap the
// budget would have fired (n0 < 0 wraps to a huge unsigned count and n0
// over budget both trap -- exactly where the original recursion cap did).
// Additive accumulators x' = x + g + b*nP fold via x_N = x0 + gN +
// b(N*n0 - N(N-1)/2), all mod-2^64 exact. Binary-search-shaped bodies
// (args not affine in the phis) fail the resolver and keep their loop.

// ---- the affine form over H-phi outputs ----------------------------------
struct CdAff {                                 // sum(coeff_i * phi_i) + k
  std::vector<std::pair<uint32_t, val_t>> t{}; // (H-phi vreg, coeff), sorted
  val_t c{};
};
constexpr bool cd_aff_eq(const CdAff &a, const CdAff &b) {
  if (a.c != b.c || a.t.size() != b.t.size())
    return false;
  for (size_t i = 0; i < a.t.size(); ++i)
    if (a.t[i] != b.t[i])
      return false;
  return true;
}
constexpr void cd_term(CdAff &L, val_t k, uint32_t v) {
  if (k == 0)
    return;
  for (size_t i = 0; i < L.t.size(); ++i)
    if (L.t[i].first == v) {
      L.t[i].second = (val_t)((uint64_t)L.t[i].second + (uint64_t)k);
      if (L.t[i].second == 0)
        L.t.erase(L.t.begin() + (long)i);
      return;
    }
  size_t i = 0;
  while (i < L.t.size() && L.t[i].first < v)
    ++i;
  L.t.insert(L.t.begin() + (long)i, {v, k});
}

// ---- def-site scan --------------------------------------------------------
// the LAST instr defining v, or {NONE, NONE}. Every phase of the former
// inline matcher repeated this scan; one helper replaces ten copies.
// `nb` bounds the scan EXACTLY as each original site did: the resolver
// passes fn.nblocks() (current), the shape matchers pass the pass-entry
// count -- the difference is load-bearing (blocks appended by earlier
// folds in the same pass call are visible to the resolver and invisible
// to the shape matchers, which is what keeps those matches conservative).
struct CdDef {
  uint32_t i{NONE}, b{NONE};
};
constexpr CdDef cd_def_site(const Fn &fn, uint32_t v, uint32_t nb) {
  CdDef d{};
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
      if (fn.code[i].dst == v) {
        d.i = i;
        d.b = b;
      }
  return d;
}

// ---- the resolver: v as an affine form over the H-phis --------------------
// (bounded depth; walks Const/Copy/Bin(+,-,*const)/Phi-with-equal-args)
constexpr bool cd_resolve(const Fn &fn, uint32_t v,
                          const std::vector<uint32_t> &hphis, CdAff &out,
                          int depth);
// v's position in hphis, or -1
constexpr long cd_hphi_index(const std::vector<uint32_t> &hphis, uint32_t v) {
  for (size_t i = 0; i < hphis.size(); ++i)
    if (hphis[i] == v)
      return (long)i;
  return -1;
}
// the phi's argument arriving from pred `key` (phis are keyed by pred id)
constexpr bool cd_phi_arg(const Instr &D, uint32_t key, uint32_t &av) {
  if (D.t == key) {
    av = D.a;
    return D.a != NONE;
  }
  if (D.f == key) {
    av = D.b2;
    return D.b2 != NONE;
  }
  return false;
}
// x * y: affine only when one side is a pure constant -- and the original
// resolver only accepted the VALUE on the left (x) or both-const
constexpr bool cd_aff_mul(const CdAff &x, const CdAff &y, CdAff &out) {
  if (!x.t.empty()) { // x is the value, y the const
    if (!y.t.empty())
      return false; // value * value
    out = x;
    for (auto &pr : out.t)
      pr.second = (val_t)((uint64_t)pr.second * (uint64_t)y.c);
    out.c = (val_t)((uint64_t)out.c * (uint64_t)y.c);
    return true;
  }
  if (!y.t.empty())
    return false; // const * value: rejected, as the original did
  out = CdAff{};
  out.c = (val_t)((uint64_t)x.c * (uint64_t)y.c);
  return true;
}
// x +- y: x UNSCALED, y signed (a - b, not -(a+b))
constexpr bool cd_aff_addsub(const CdAff &x, const CdAff &y, bool add,
                             CdAff &out) {
  const val_t sgn = add ? (val_t)1 : (val_t)-1;
  out = x;
  for (const auto &pr : y.t)
    cd_term(out, (val_t)((uint64_t)pr.second * (uint64_t)sgn), pr.first);
  out.c = (val_t)((uint64_t)x.c +
                  (uint64_t)((val_t)((uint64_t)y.c * (uint64_t)sgn)));
  return true;
}
constexpr bool cd_resolve_bin(const Fn &fn, const Instr &D,
                              const std::vector<uint32_t> &hphis, CdAff &out,
                              int depth) {
  if (D.b != B::Add && D.b != B::Sub && D.b != B::Mul)
    return false;
  CdAff x{}, y{};
  if (!cd_resolve(fn, D.a, hphis, x, depth + 1))
    return false;
  if (!cd_resolve(fn, D.b2, hphis, y, depth + 1))
    return false;
  if (D.b == B::Mul)
    return cd_aff_mul(x, y, out);
  return cd_aff_addsub(x, y, D.b == B::Add, out);
}
// a Phi resolves when every live arg resolves to the SAME affine form
constexpr bool cd_resolve_phi(const Fn &fn, const CdDef &d, const Instr &D,
                              const std::vector<uint32_t> &hphis, CdAff &out,
                              int depth) {
  std::vector<CdAff> as{};
  for (uint32_t p : fn.preds[d.b]) {
    uint32_t av = NONE;
    if (!cd_phi_arg(D, p, av))
      continue; // dead pred
    CdAff a{};
    if (!cd_resolve(fn, av, hphis, a, depth + 1))
      return false;
    as.push_back(a);
  }
  if (as.empty())
    return false;
  for (size_t i = 1; i < as.size(); ++i)
    if (!cd_aff_eq(as[0], as[i]))
      return false;
  out = as[0];
  return true;
}
constexpr bool cd_resolve(const Fn &fn, uint32_t v,
                          const std::vector<uint32_t> &hphis, CdAff &out,
                          int depth) {
  if (depth > 8)
    return false;
  if (v == NONE || v >= fn.nvars + (uint32_t)fn.version_base.size())
    return false;
  if (cd_hphi_index(hphis, v) >= 0) { // an H-phi output: the unit form
    out = CdAff{};
    cd_term(out, 1, v);
    return true;
  }
  const CdDef d = cd_def_site(fn, v, fn.nblocks());
  if (d.i == NONE)
    return false;
  const Instr &D = fn.code[d.i];
  if (D.k == K::Const) {
    out = CdAff{};
    out.c = D.c;
    return true;
  }
  if (D.k == K::Copy && D.a != NONE)
    return cd_resolve(fn, D.a, hphis, out, depth + 1);
  if (D.k == K::Bin)
    return cd_resolve_bin(fn, D, hphis, out, depth);
  if (D.k == K::Phi)
    return cd_resolve_phi(fn, d, D, hphis, out, depth);
  return false;
}

// ---- the matched shape ----------------------------------------------------
struct CdHPhi { // one header phi, with its entry/latch argument vregs
  uint32_t dst, entry, latch;
};
struct CdCls {    // its latch value, classified
  uint32_t phi;   // the H-phi vreg
  uint32_t entry; // entry (pre) value vreg
  CdAff laff;     // latch evolution, affine over hphis
  bool is_sel{};  // selector (latch merges per-arm CONSTS)
};
struct CdAdd { // an additive accumulator: x' = x + g (+ bn*nP)
  uint32_t phi, entry;
  val_t g, bn;
};
struct CdShape {              // everything the matcher proved about one loop
  uint32_t h{}, pre{}, L{};   // header, preheader, latch
  std::vector<char> inL;      // membership, CURRENT block count
  std::vector<CdHPhi> hp;     // the header phis
  std::vector<uint32_t> hids; // their dsts (the resolver domain)
  std::vector<CdCls> cls;     // latch classification
  uint32_t selp{NONE}, selEntry{NONE}; // the selector phi (or NONE)
  uint32_t nP{NONE}, nEntry{NONE};     // the counter: n' = n - 1
  uint32_t bH{NONE}, bEntry{NONE};     // the budget: b' = b - 1
  std::vector<CdAdd> adds;             // the additive accumulators
  std::vector<uint32_t> Xs;            // base-case exit blocks
  uint32_t T{NONE};                    // the budget trap block
  Instr hterm{};                       // the header terminator (a copy)
};

// ---- the matchers (one per phase, the old order) ---------------------------
// membership: the loop body rows + the header. Sized to the CURRENT block
// count: earlier folds in this same pass call appended closed-form blocks
// (ids >= A.n); they are outside this loop, so 0 is the right default for
// them, and the succ scan below may legitimately index them.
constexpr void cd_membership(const Fn &fn, const An &A, uint32_t h,
                             CdShape &S) {
  auto row = A.loops.range()[h];
  S.inL.assign(fn.nblocks(), 0);
  for (ulen x : row)
    S.inL[(uint32_t)x] = 1;
  S.inL[h] = 1;
}
// exactly one outside pred (the preheader) + one latch; anything else
// (multiple latches = multi-site self) keeps the loop. (The latch ends
// with `br h` when a merge chain exists, or with the budget check
// `cbr ? T : h` when the site block IS the latch; the exit classification
// validates the trap edge either way.)
constexpr bool cd_loop_edges(const Fn &fn, CdShape &S) {
  for (uint32_t p : fn.preds[S.h]) {
    if (S.inL[p]) {
      if (S.L != NONE)
        return false; // multiple latches (multi-site self)
      S.L = p;
    } else {
      if (S.pre != NONE)
        return false;
      S.pre = p;
    }
  }
  return S.pre != NONE && S.L != NONE;
}
// the header phis (phis-first invariant: they lead the block), each with
// its entry (preheader) and latch argument; nP + bH at least
constexpr bool cd_collect_hphis(const Fn &fn, CdShape &S) {
  for (uint32_t i = fn.blocks[S.h].begin; i < fn.blocks[S.h].end; ++i) {
    const Instr &I = fn.code[i];
    if (I.k != K::Phi)
      break;
    const uint32_t e = (I.t == S.pre) ? I.a : ((I.f == S.pre) ? I.b2 : NONE);
    const uint32_t l = (I.t == S.L) ? I.a : ((I.f == S.L) ? I.b2 : NONE);
    if (e == NONE || l == NONE)
      return false;
    S.hp.push_back({I.dst, e, l});
  }
  if (S.hp.size() < 2)
    return false;
  for (const CdHPhi &p : S.hp)
    S.hids.push_back(p.dst);
  return true;
}
// classify each latch value: affine over the H-phis (the resolver), or a
// Phi of CONSTS selected by the dispatch (resolve failed => not uniform
// affine -- the structural Phi check here, the toggle proof later)
constexpr bool cd_classify_latches(const Fn &fn, uint32_t nb, CdShape &S) {
  for (const CdHPhi &p : S.hp) {
    CdAff la{};
    if (cd_resolve(fn, p.latch, S.hids, la, 0)) {
      S.cls.push_back({p.dst, p.entry, la, false});
      continue;
    }
    const CdDef d = cd_def_site(fn, p.latch, nb);
    if (d.i == NONE || fn.code[d.i].k != K::Phi)
      return false;
    S.cls.push_back({p.dst, p.entry, CdAff{}, true});
  }
  return true;
}
// at most one selector; remember its entry value for the toggle proof
constexpr bool cd_find_selector(CdShape &S) {
  for (const CdCls &c : S.cls)
    if (c.is_sel) {
      if (S.selp != NONE)
        return false;
      S.selp = c.phi;
    }
  for (const CdHPhi &p : S.hp)
    if (p.dst == S.selp)
      S.selEntry = p.entry;
  return true;
}
// the two countdowns: nP's latch affine = nP - 1; bH's = bH - 1
constexpr bool cd_is_countdown(const CdCls &c) {
  return c.laff.t.size() == 1 && c.laff.t[0].first == c.phi &&
         c.laff.t[0].second == 1 && c.laff.c == (val_t)-1;
}
constexpr bool cd_find_countdown(CdShape &S) {
  for (const CdCls &c : S.cls) {
    if (c.is_sel || !cd_is_countdown(c))
      continue;
    if (S.nP == NONE) {
      S.nP = c.phi;
      S.nEntry = c.entry;
    } else if (S.bH == NONE) {
      S.bH = c.phi;
      S.bEntry = c.entry;
    } else
      return false; // a third pure countdown: unsupported
  }
  return S.nP != NONE && S.bH != NONE;
}
// the remaining phis must be additive over {self, nP}: x' = x + g (+ b*nP);
// a pure function of nP (no self term) is unsupported here
constexpr bool cd_add_of(const CdCls &c, uint32_t nP, CdAdd &out) {
  val_t bn = 0;
  bool hasSelf = false;
  for (const auto &pr : c.laff.t) {
    if (pr.first == c.phi && pr.second == 1)
      hasSelf = true; // self coefficient exactly 1 (additive)
    else if (pr.first == nP)
      bn = pr.second;
    else
      return false;
  }
  if (!hasSelf || c.laff.t.size() > 2)
    return false;
  out = {c.phi, c.entry, c.laff.c, bn}; // g = the constant part
  return true;
}
constexpr bool cd_classify_adds(CdShape &S) {
  for (const CdCls &c : S.cls) {
    if (c.is_sel || c.phi == S.nP || c.phi == S.bH)
      continue;
    CdAdd ad{};
    if (!cd_add_of(c, S.nP, ad))
      return false;
    S.adds.push_back(ad);
  }
  return true;
}

// ---- the exit tests --------------------------------------------------------
// Bin D is Eq(v, Const 0) with v on either side: v, or NONE
constexpr uint32_t cd_eq_zero_vreg(const Fn &fn, uint32_t nb, const Instr &D) {
  if (D.k != K::Bin || D.b != B::Eq)
    return NONE;
  const CdDef da = cd_def_site(fn, D.a, nb);
  const CdDef db = cd_def_site(fn, D.b2, nb);
  const bool a0 =
      da.i != NONE && fn.code[da.i].k == K::Const && fn.code[da.i].c == 0;
  const bool b0 =
      db.i != NONE && fn.code[db.i].k == K::Const && fn.code[db.i].c == 0;
  if (a0 && !b0)
    return D.b2;
  if (b0 && !a0)
    return D.a;
  return NONE;
}
// `cbr cond ? t : f` whose cond is Bin(nP == 0): the base test. Eq is
// symmetric: the TRUE arm is the n==0 (base) arm.
constexpr bool cd_cbr_np_zero(const Fn &fn, uint32_t nb, uint32_t nP,
                              const Instr &C, uint32_t &xt, uint32_t &af) {
  if (C.k != K::Cbr)
    return false;
  const CdDef d = cd_def_site(fn, C.a, nb);
  if (d.i == NONE)
    return false;
  if (cd_eq_zero_vreg(fn, nb, fn.code[d.i]) != nP)
    return false;
  xt = C.t;
  af = C.f;
  return true;
}
// `cbr cond ? t : f` whose cond is Bin(bH == 0): the budget check; the
// ==0 arm traps
constexpr bool cd_cbr_bh_zero_trap(const Fn &fn, uint32_t nb, uint32_t bH,
                                   const Instr &C, uint32_t &tt) {
  if (C.k != K::Cbr)
    return false;
  const CdDef d = cd_def_site(fn, C.a, nb);
  if (d.i == NONE)
    return false;
  if (cd_eq_zero_vreg(fn, nb, fn.code[d.i]) != bH)
    return false;
  tt = C.t;
  return true;
}
// a loop block with any edge leaving inL
constexpr bool cd_block_has_out(const Fn &fn, const std::vector<char> &inL,
                                uint32_t b) {
  for (uint32_t s : {fn.succ0(b), fn.succ1(b)})
    if (s != NONE && !inL[s])
      return true;
  return false;
}
constexpr void cd_dedupe_xs(std::vector<uint32_t> &Xs) {
  std::vector<uint32_t> u{};
  for (uint32_t x : Xs) {
    bool dup = false;
    for (uint32_t y : u)
      dup |= (x == y);
    if (!dup)
      u.push_back(x);
  }
  Xs = u;
}
// every block WITH an outside edge must exit via a base test (nP==0 at a
// member block) or the budget trap (bH==0); the dispatch header and the
// latch have only internal successors and are skipped here
constexpr bool cd_classify_exits(const Fn &fn, uint32_t nb, CdShape &S) {
  for (uint32_t b = 0; b < nb; ++b) {
    if (!S.inL[b] || !cd_block_has_out(fn, S.inL, b))
      continue;
    const Instr &tm = fn.code[fn.blocks[b].end - 1];
    uint32_t xt = NONE, af = NONE, tt = NONE;
    if (cd_cbr_np_zero(fn, nb, S.nP, tm, xt, af)) {
      if (S.inL[xt] || !S.inL[af])
        return false;
      S.Xs.push_back(xt);
    } else if (cd_cbr_bh_zero_trap(fn, nb, S.bH, tm, tt)) {
      if (S.inL[tt])
        return false;
      if (S.T == NONE)
        S.T = tt;
      else if (S.T != tt)
        return false;
    } else
      return false; // some other exit: keep the loop
  }
  if (S.T == NONE)
    return false; // no trap edge: not the TCE shape
  cd_dedupe_xs(S.Xs);
  return !S.Xs.empty() && S.Xs.size() <= 2;
}

// ---- the X blocks (the base-case exits) ------------------------------------
struct CdXInfo {        // cross-block X state
  uint32_t xnext{NONE}; // the common Br target
  bool xret{false};     // some X returns
};
// v is one of the H-phis
constexpr bool cd_uses_hphi(const CdShape &S, uint32_t v) {
  for (uint32_t hid : S.hids)
    if (v == hid)
      return true;
  return false;
}
// a Ret X block: the operand is a Const def here, or a loop phi (an
// accumulator returns a loop-carried value; the outside-use rewrite
// replaces it with the closed form)
constexpr bool cd_x_ret(const Fn &fn, const CdShape &S, uint32_t b) {
  const Instr &tm = fn.code[fn.blocks[b].end - 1];
  if (tm.a == NONE)
    return false;
  uint32_t di = NONE;
  for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
    if (fn.code[i].dst == tm.a)
      di = i;
  if (di != NONE && fn.code[di].k == K::Const)
    return true;
  return cd_uses_hphi(S, tm.a);
}
// a Br X block: one common next target, at most ONE Const (the member's
// base value), nothing else but phis; the member select needs its base
// consts
constexpr bool cd_x_br(const Fn &fn, const CdShape &S, uint32_t b, CdXInfo &X) {
  const Instr &tm = fn.code[fn.blocks[b].end - 1];
  if (X.xret)
    return false;
  if (X.xnext == NONE)
    X.xnext = tm.t;
  else if (X.xnext != tm.t)
    return false;
  uint32_t nconst = 0;
  for (uint32_t i = fn.blocks[b].begin; i + 1 < fn.blocks[b].end; ++i) {
    if (fn.code[i].k == K::Const)
      ++nconst;
    else if (fn.code[i].k != K::Phi)
      return false;
  }
  return nconst <= 1 && (nconst != 0 || S.selp == NONE);
}
// no loop-phi uses inside an X block, except a Ret OPERAND (checked by
// cd_x_ret above: I.b2/I.c2 of a Ret are still scrutinized here)
constexpr bool cd_x_uses_ok(const Fn &fn, const CdShape &S, uint32_t b) {
  const uint32_t end = fn.blocks[b].end;
  for (uint32_t i = fn.blocks[b].begin; i < end; ++i) {
    const Instr &I = fn.code[i];
    if (i + 1 == end && I.k == K::Ret) {
      if (cd_uses_hphi(S, I.b2) || cd_uses_hphi(S, I.c2))
        return false;
      continue; // I.a checked above
    }
    if (cd_uses_hphi(S, I.a) || cd_uses_hphi(S, I.b2) || cd_uses_hphi(S, I.c2))
      return false;
  }
  return true;
}
constexpr bool cd_check_xblocks(const Fn &fn, const CdShape &S, CdXInfo &X) {
  for (uint32_t b : S.Xs) {
    const Instr &tm = fn.code[fn.blocks[b].end - 1];
    if (tm.k == K::Ret) {
      if (!cd_x_ret(fn, S, b))
        return false;
      X.xret = true;
    } else if (tm.k == K::Br) {
      if (!cd_x_br(fn, S, b, X))
        return false;
    } else
      return false;
    if (!cd_x_uses_ok(fn, S, b))
      return false;
  }
  return true;
}

// ---- selector soundness: sel must provably stay in {0,1} -------------------
// entry: a Const in {0,1}, or the fn's sel param under Fn::tce_sel == 2
constexpr bool cd_sel_entry_ok(const Fn &fn, uint32_t nb, const CdShape &S) {
  const CdDef d = cd_def_site(fn, S.selEntry, nb);
  if (d.i != NONE && fn.code[d.i].k == K::Const &&
      (fn.code[d.i].c == 0 || fn.code[d.i].c == 1))
    return true;
  return fn.tce_sel == 2 && S.selEntry == 0;
}
// the sel latch phi's two args: consts in {0,1}, DISTINCT, differing by
// exactly +-1 (A = cb - ca: a toggle or identity)
constexpr bool cd_sel_toggle_ok(const Fn &fn, uint32_t nb, const CdShape &S) {
  uint32_t selLatch = NONE;
  for (const CdHPhi &p : S.hp)
    if (p.dst == S.selp)
      selLatch = p.latch;
  const CdDef si = cd_def_site(fn, selLatch, nb);
  if (si.i == NONE)
    return false;
  const CdDef ia = cd_def_site(fn, fn.code[si.i].a, nb);
  const CdDef ib = cd_def_site(fn, fn.code[si.i].b2, nb);
  if (ia.i == NONE || ib.i == NONE)
    return false;
  if (fn.code[ia.i].k != K::Const || fn.code[ib.i].k != K::Const)
    return false;
  const val_t ca = fn.code[ia.i].c, cb = fn.code[ib.i].c;
  if ((ca != 0 && ca != 1) || (cb != 0 && cb != 1) || ca == cb)
    return false;
  const val_t A = (val_t)((uint64_t)cb - (uint64_t)ca);
  return A == 1 || A == (val_t)-1;
}
constexpr bool cd_sel_sound(const Fn &fn, uint32_t nb, const CdShape &S) {
  if (S.selp == NONE)
    return true;
  return cd_sel_entry_ok(fn, nb, S) && cd_sel_toggle_ok(fn, nb, S);
}

// ---- body purity -----------------------------------------------------------
constexpr bool cd_instr_pure(const Instr &I) {
  switch (I.k) {
  case K::Phi:
  case K::Const:
  case K::Copy:
  case K::Br:
  case K::Cbr:
    return true;
  case K::Bin:
    return (I.b != B::Div && I.b != B::Mod) || I.nt;
  default:
    return false; // Print/Call/Input/Alloc/Load/Store/Len: observable
  }
}
constexpr bool cd_body_pure(const Fn &fn, uint32_t nb, const CdShape &S) {
  for (uint32_t b = 0; b < nb; ++b) {
    if (!S.inL[b])
      continue;
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
      if (!cd_instr_pure(fn.code[i]))
        return false;
  }
  return true;
}

// ---- the match: every phase, the old order ---------------------------------
constexpr bool cd_match(const Fn &fn, const An &A, uint32_t nb, uint32_t h,
                        CdShape &S) {
  S.h = h;
  cd_membership(fn, A, h, S);
  if (!cd_loop_edges(fn, S))
    return false;
  if (!cd_collect_hphis(fn, S))
    return false;
  if (!cd_classify_latches(fn, nb, S))
    return false;
  if (!cd_find_selector(S))
    return false;
  if (!cd_find_countdown(S))
    return false;
  if (!cd_classify_adds(S))
    return false;
  S.hterm = fn.code[fn.blocks[h].end - 1]; // the member dispatch
  if (!cd_classify_exits(fn, nb, S))
    return false;
  CdXInfo X{};
  if (!cd_check_xblocks(fn, S, X))
    return false;
  if (!cd_sel_sound(fn, nb, S))
    return false;
  return cd_body_pure(fn, nb, S);
}

// ---- the emitter ------------------------------------------------------------
// two buffers: `g` splices into the preheader (the guard), `cf` becomes
// the appended closed-form block. nt() allocates the fresh vregs, so the
// CALL ORDER of the emit helpers is the vreg numbering -- byte-identical
// output depends on keeping it exactly as the original sequenced them.
struct CdGen {
  Fn &fn;
  std::vector<Instr> g{};  // preheader guard buffer
  std::vector<Instr> cf{}; // the closed-form block buffer
  constexpr uint32_t nt() { return new_anon_vreg(fn, 0); }
  constexpr uint32_t konst(val_t c) {
    uint32_t v = nt();
    g.push_back({.k = K::Const, .dst = v, .c = c});
    return v;
  }
  constexpr void bin(B b, uint32_t d, uint32_t a, uint32_t b2) {
    g.push_back({.k = K::Bin, .b = b, .dst = d, .a = a, .b2 = b2});
  }
  constexpr uint32_t ckonst(val_t c) {
    uint32_t v = nt();
    cf.push_back({.k = K::Const, .dst = v, .c = c});
    return v;
  }
  constexpr void cbin(B b, uint32_t d, uint32_t a, uint32_t b2) {
    cf.push_back({.k = K::Bin, .b = b, .dst = d, .a = a, .b2 = b2});
  }
};
// the guard (into the preheader): unsigned bInit < N <=> N > bInit, via
// the sign-bit lift -- jumps to the SAME trap the budget would have fired
constexpr uint32_t cd_emit_guard(CdGen &G, const CdShape &S) {
  const uint32_t MINc = G.konst(INT64_MIN);
  const uint32_t xN = G.nt();
  G.bin(B::Xor, xN, S.nEntry, MINc);
  const uint32_t xB = G.nt();
  G.bin(B::Xor, xB, S.bEntry, MINc);
  const uint32_t bad = G.nt();
  G.bin(B::Lt, bad, xB, xN);
  return bad;
}
// p = N & 1 (plus the shared `1` const the toggle reuses)
constexpr uint32_t cd_emit_parity(CdGen &G, const CdShape &S, uint32_t &oneC) {
  oneC = G.ckonst(1);
  const uint32_t pV = G.nt();
  G.cbin(B::And, pV, S.nEntry, oneC);
  return pV;
}
// selN = (1-2p)*selEntry + p (exact for every selEntry; the SELECT below
// additionally needs sel in {0,1}, proven by cd_sel_sound)
constexpr uint32_t cd_emit_toggle(CdGen &G, const CdShape &S, uint32_t oneC,
                                  uint32_t pV) {
  const uint32_t two = G.nt();
  G.cbin(B::Add, two, pV, pV);
  const uint32_t k1 = G.nt();
  G.cbin(B::Sub, k1, oneC, two);
  const uint32_t m = G.nt();
  G.cbin(B::Mul, m, k1, S.selEntry);
  const uint32_t selN = G.nt();
  G.cbin(B::Add, selN, m, pV);
  return selN;
}
// T = (N - (N&1))/2 * (N + (N&1) - 1) [exact, same trick as lcf rt] and
// NN = N*n0 -- the shared sub-terms of every additive closed form
struct CdTri {
  uint32_t Tv{}, NN{};
};
constexpr CdTri cd_emit_triangle(CdGen &G, const CdShape &S, uint32_t pV) {
  const uint32_t e0 = G.ckonst(1);
  const uint32_t two = G.nt(); // 2N (parity of N: N&1 -- have pV)
  G.cbin(B::Add, two, S.nEntry, S.nEntry);
  const uint32_t Nm = G.nt();
  G.cbin(B::Sub, Nm, S.nEntry, pV);
  const uint32_t h2 = G.nt();
  G.cbin(B::LShr, h2, Nm, e0);
  const uint32_t t1 = G.nt();
  G.cbin(B::Add, t1, S.nEntry, pV);
  const uint32_t o1 = G.nt();
  G.cbin(B::Sub, o1, t1, e0);
  CdTri r{};
  r.Tv = G.nt();
  G.cbin(B::Mul, r.Tv, h2, o1);
  r.NN = G.nt();
  G.cbin(B::Mul, r.NN, S.nEntry, S.nEntry);
  return r;
}
// x_N = x0 + g*N + bn*(N*n0 - T) for ONE accumulator
constexpr uint32_t cd_emit_add_one(CdGen &G, const CdAdd &ad, uint32_t nEntry,
                                   const CdTri &tri) {
  uint32_t acc = ad.entry;
  if (ad.g != 0) {
    const uint32_t gn = G.nt();
    G.cbin(B::Mul, gn, G.ckonst(ad.g), nEntry);
    const uint32_t a2 = G.nt();
    G.cbin(B::Add, a2, acc, gn);
    acc = a2;
  }
  if (ad.bn != 0) {
    const uint32_t sn = G.nt();
    G.cbin(B::Sub, sn, tri.NN, tri.Tv);
    const uint32_t bt = G.nt();
    G.cbin(B::Mul, bt, sn, G.ckonst(ad.bn));
    const uint32_t a3 = G.nt();
    G.cbin(B::Add, a3, acc, bt);
    acc = a3;
  }
  return acc;
}
// the closed-form block body: parity, the toggle, the additive forms,
// n_N = 0. Fills (H-phi -> value) for the outside-use rewrite.
constexpr void
cd_emit_closed_forms(CdGen &G, const CdShape &S, uint32_t &selN,
                     std::vector<std::pair<uint32_t, uint32_t>> &xform) {
  uint32_t oneC = NONE;
  const uint32_t pV = cd_emit_parity(G, S, oneC);
  if (S.selp != NONE)
    selN = cd_emit_toggle(G, S, oneC, pV);
  CdTri tri{};
  if (!S.adds.empty())
    tri = cd_emit_triangle(G, S, pV);
  for (const CdAdd &ad : S.adds)
    xform.push_back({ad.phi, cd_emit_add_one(G, ad, S.nEntry, tri)});
  xform.push_back({S.nP, G.ckonst(0)}); // n_N = 0 (b_N = bInit - N: only
  // if used outside -- it never is: the budget trap consumed it)
}
// rewrite one operand: the selector to selN, every H-phi to its form
constexpr void
cd_rewrite_op(const CdShape &S, uint32_t selN,
              const std::vector<std::pair<uint32_t, uint32_t>> &xform,
              uint32_t &v) {
  if (v == NONE)
    return;
  if (v == S.selp && selN != NONE)
    v = selN;
  for (const auto &xf : xform)
    if (v == xf.first)
      v = xf.second;
}
// rewrite every OUTSIDE-the-loop use of the H-phis (X blocks use none; T
// uses none; anywhere else outside inL -- including closed-form blocks
// appended by earlier folds in this same pass call, which never reference
// this loop's phis but are covered here anyway so the rewrite is complete
// by construction). The preheader is skipped: the guard emission below
// already carries its operands.
constexpr void
cd_rewrite_uses(Fn &fn, const CdShape &S, uint32_t selN,
                const std::vector<std::pair<uint32_t, uint32_t>> &xform) {
  for (uint32_t b = 0; b < fn.nblocks(); ++b) {
    if (b >= S.inL.size() || S.inL[b] || b == S.pre)
      continue;
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      Instr &I = fn.code[i];
      cd_rewrite_op(S, selN, xform, I.a);
      cd_rewrite_op(S, selN, xform, I.b2);
      cd_rewrite_op(S, selN, xform, I.c2);
      for (uint32_t &av : I.args)
        cd_rewrite_op(S, selN, xform, av);
    }
  }
}
// pre's terminator `br h` becomes `cbr bad ? T : CF`
constexpr bool cd_patch_pre(Fn &fn, const CdShape &S, uint32_t bad,
                            uint32_t CFb) {
  Instr &pt = fn.code[fn.blocks[S.pre].end - 1];
  if (pt.k != K::Br || pt.t != S.h)
    return false;
  pt.k = K::Cbr;
  pt.a = bad;
  pt.t = S.T;
  pt.f = CFb;
  return true;
}
// the multi-member select: the header dispatched on selH (its cond must
// be Eq(selH, ..)); sel == 1 -> the TRUE arm, whose base test names
// Xtrue; cf selects between the two Xs by selN
constexpr bool cd_cf_term_sel(const Fn &fn, const CdShape &S, uint32_t nb,
                              uint32_t selN, std::vector<Instr> &cf) {
  if (S.Xs.size() != 2)
    return false;
  const CdDef d = cd_def_site(fn, S.hterm.a, nb);
  if (d.i == NONE || fn.code[d.i].k != K::Bin || fn.code[d.i].b != B::Eq ||
      fn.code[d.i].a != S.selp)
    return false;
  const uint32_t Et = S.hterm.t;
  if (Et >= nb)
    return false;
  const Instr &et = fn.code[fn.blocks[Et].end - 1];
  uint32_t xt2 = NONE, af2 = NONE;
  if (!cd_cbr_np_zero(fn, nb, S.nP, et, xt2, af2))
    return false;
  uint32_t Xfalse = NONE;
  for (uint32_t x : S.Xs)
    if (x != xt2)
      Xfalse = x;
  if (Xfalse == NONE)
    return false;
  cf.push_back({.k = K::Cbr, .a = selN, .t = xt2, .f = Xfalse});
  return true;
}
// CF terminator: the select (multi) or br X (single member)
constexpr bool cd_cf_terminator(const Fn &fn, const CdShape &S, uint32_t nb,
                                uint32_t selN, std::vector<Instr> &cf) {
  if (S.selp != NONE)
    return cd_cf_term_sel(fn, S, nb, selN, cf);
  if (S.Xs.size() != 1)
    return false;
  cf.push_back({.k = K::Br, .t = S.Xs[0]});
  return true;
}
// splice the guard instrs into pre (before its terminator) and shift
// EVERY current block range -- including CF blocks appended by earlier
// folds in this same pass call, whose instrs sit at the end of the code
// vector and move when g is spliced in (a stale entry-time bound here
// left their ranges pointing into unrelated code)
constexpr void cd_splice_pre(Fn &fn, const CdShape &S,
                             const std::vector<Instr> &g) {
  const uint32_t pe = fn.blocks[S.pre].end - 1;
  fn.code.insert(fn.code.begin() + (long)pe, g.begin(), g.end());
  const uint32_t add = (uint32_t)g.size();
  for (uint32_t b = 0; b < fn.nblocks(); ++b) {
    if (b == S.pre) {
      fn.blocks[b].end += add;
      continue;
    }
    if (fn.blocks[b].begin >= pe + 1)
      fn.blocks[b].begin += add;
    if (fn.blocks[b].end > pe)
      fn.blocks[b].end += add;
  }
}
// append the closed-form block and wire the edges: pre -> T / CF, and
// CF -> its terminator target(s)
constexpr void cd_append_cf(Fn &fn, const CdShape &S, uint32_t CFb,
                            const std::vector<Instr> &cf) {
  fn.code.insert(fn.code.end(), cf.begin(), cf.end());
  fn.blocks.push_back(
      {uint32_t(fn.code.size() - cf.size()), (uint32_t)fn.code.size()});
  fn.succ.push_back(NONE);
  fn.succ.push_back(NONE);
  fn.succ[2 * S.pre] = S.T;
  fn.succ[2 * S.pre + 1] = CFb;
  const Instr &ct = fn.code[fn.blocks[CFb].end - 1];
  if (ct.k == K::Br)
    fn.succ[2 * CFb] = ct.t;
  else if (ct.k == K::Cbr) {
    fn.succ[2 * CFb] = ct.t;
    fn.succ[2 * CFb + 1] = ct.f;
  }
}
// the loop is now unreachable: succ severed AND the blocks' code ranges
// emptied -- a Br-with-NONE-target left in the code vector would crash
// the bytecode emitter (off[f][NONE]) even though the block is dead;
// empty ranges are inert everywhere
constexpr void cd_sever_loop(Fn &fn, const CdShape &S, uint32_t nb) {
  for (uint32_t b = 0; b < nb; ++b) {
    if (!S.inL[b])
      continue;
    fn.succ[2 * b] = NONE;
    fn.succ[2 * b + 1] = NONE;
    fn.blocks[b].end = fn.blocks[b].begin; // drop every instr
  }
  fn.recompute_preds();
}

// ---- the fold, in the old order ---------------------------------------------
// guard -> closed forms -> outside-use rewrite -> preheader patch -> CF
// terminator -> splice -> append -> sever. The rewrite happens BEFORE the
// two post-rewrite validations, exactly as the original sequenced it (the
// shape match already proved them for every reachable input; a late
// `return false` leaves the same partial state the original left).
constexpr bool cd_apply(Fn &fn, const CdShape &S, uint32_t nb, OptStats &os) {
  CdGen G{fn};
  const uint32_t bad = cd_emit_guard(G, S);
  uint32_t selN = NONE;
  std::vector<std::pair<uint32_t, uint32_t>> xform{};
  cd_emit_closed_forms(G, S, selN, xform);
  cd_rewrite_uses(fn, S, selN, xform);
  const uint32_t CFb = fn.nblocks(); // appended block id -- CURRENT
  // count: an earlier fold in this same pass call already appended its
  // own CF block, and reusing the entry-time count would collide with it
  if (!cd_patch_pre(fn, S, bad, CFb))
    return false;
  if (!cd_cf_terminator(fn, S, nb, selN, G.cf))
    return false;
  cd_splice_pre(fn, S, G.g);
  cd_append_cf(fn, S, CFb, G.cf);
  cd_sever_loop(fn, S, nb);
  os.cdfolds++;
  return true;
}

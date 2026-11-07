#pragma once
// === cdfold.hpp ========================================
// countdown fold: replace tailrec'd exact-budget loops by closed
// forms.

#include "minijz/base/pass_cfg.hpp"
#include "minijz/ir/instr.hpp"
#include "minijz/opt/analysis/an.hpp"
#include "minijz/opt/linear/linform.hpp"

// ============================================================================
// 9e5. COUNTDOWN LOOP CLOSED FORM (cdfold)
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
// does — else the budget fires first), sel_N = toggle^N(sel0) with the
// toggle closed form (1-2p)*sel0 + p, p = N&1 (EXACT for every sel0, and
// sel stays in {0,1} when sel0 is, which is when the final select is
// sound), and the guard `(u64)n0 > (u64)bInit` jumps to the SAME trap the
// budget would have fired (n0 < 0 wraps to a huge unsigned count and n0
// over budget both trap — exactly where the original recursion cap did).
// Additive accumulators x' = x + g + b*nP fold via x_N = x0 + gN +
// b(N*n0 - N(N-1)/2), all mod-2^64 exact. Binary-search-shaped bodies
// (args not affine in the phis) fail the resolver and keep their loop.
struct CdAff { // affine over H-phi outputs: sum(coeff_i * phi_i) + k
  std::vector<std::pair<uint32_t, val_t>> t{}; // (H-phi vreg, coeff), sorted
  val_t c{};
};
constexpr bool cd_aff_eq(const CdAff &a, const CdAff &b) {
  if (a.c != b.c || a.t.size() != b.t.size()) return false;
  for (size_t i = 0; i < a.t.size(); ++i)
    if (a.t[i] != b.t[i]) return false;
  return true;
}
constexpr void cd_term(CdAff &L, val_t k, uint32_t v) {
  if (k == 0) return;
  for (size_t i = 0; i < L.t.size(); ++i)
    if (L.t[i].first == v) {
      L.t[i].second = (val_t)((uint64_t)L.t[i].second + (uint64_t)k);
      if (L.t[i].second == 0) L.t.erase(L.t.begin() + (long)i);
      return;
    }
  size_t i = 0;
  while (i < L.t.size() && L.t[i].first < v) ++i;
  L.t.insert(L.t.begin() + (long)i, {v, k});
}
// resolve v to an affine form over `hphis` (bounded depth; walks
// Const/Copy/Bin(+,-,*const)/Phi-with-equal-affine-args)
constexpr bool cd_resolve(const Fn &fn, uint32_t v,
                          const std::vector<uint32_t> &hphis, CdAff &out,
                          int depth) {
  if (depth > 8) return false;
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (v == NONE || v >= nv) return false;
  for (uint32_t h : hphis) {
    if (v == h) {
      out = CdAff{};
      cd_term(out, 1, v);
      return true;
    }
  }
  // def site
  uint32_t di = NONE, db = NONE;
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
      if (fn.code[i].dst == v) {
        di = i;
        db = b;
      }
  if (di == NONE) return false;
  const Instr &D = fn.code[di];
  if (D.k == K::Const) {
    out = CdAff{};
    out.c = D.c;
    return true;
  }
  if (D.k == K::Copy && D.a != NONE)
    return cd_resolve(fn, D.a, hphis, out, depth + 1);
  if (D.k == K::Bin && (D.b == B::Add || D.b == B::Sub || D.b == B::Mul)) {
    CdAff x{}, y{};
    if (!cd_resolve(fn, D.a, hphis, x, depth + 1)) return false;
    if (!cd_resolve(fn, D.b2, hphis, y, depth + 1)) return false;
    if (D.b == B::Mul) { // one side must be a pure constant
      if (!x.t.empty()) { // x is the value, y the const
        if (!y.t.empty()) return false;
        out = x;
        for (auto &pr : out.t)
          pr.second = (val_t)((uint64_t)pr.second * (uint64_t)y.c);
        out.c = (val_t)((uint64_t)out.c * (uint64_t)y.c);
        return true;
      }
      if (!y.t.empty()) return false; // both const: shouldn't happen
      out = CdAff{};
      out.c = D.c; // placeholder, replaced below
      out = x;
      out.c = (val_t)0;
      // const*const: fold via c
      CdAff cx{};
      cd_resolve(fn, D.a, hphis, cx, depth + 1); // known const
      out.c = (val_t)((uint64_t)cx.c * (uint64_t)y.c);
      return true;
    }
    val_t sgn = (D.b == B::Add) ? 1 : (val_t)-1;
    out = x; // x - y = x + sgn*y: x UNSCALED, y scaled (a - b, not -(a+b))
    for (auto &pr : y.t)
      cd_term(out, (val_t)((uint64_t)pr.second * (uint64_t)sgn), pr.first);
    out.c = (val_t)((uint64_t)x.c + (uint64_t)((val_t)((uint64_t)y.c * (uint64_t)sgn)));
    return true;
  }
  if (D.k == K::Phi) { // equal affines on all live args
    // resolve every arg; all must agree
    std::vector<CdAff> as{};
    auto arg_of = [&](uint32_t key, uint32_t &av) -> bool {
      if (D.t == key) { av = D.a; return D.a != NONE; }
      if (D.f == key) { av = D.b2; return D.b2 != NONE; }
      return false;
    };
    for (uint32_t p : fn.preds[db]) {
      uint32_t av = NONE;
      if (!arg_of(p, av)) continue; // dead pred
      CdAff a{};
      if (!cd_resolve(fn, av, hphis, a, depth + 1)) return false;
      as.push_back(a);
    }
    if (as.empty()) return false;
    for (size_t i = 1; i < as.size(); ++i)
      if (!cd_aff_eq(as[0], as[i])) return false;
    out = as[0];
    return true;
  }
  return false;
}

constexpr bool opt_cdfold(Fn &fn, const An &A, OptStats &os) {
  if (A.n != fn.nblocks() || !fn.nblocks()) return false;
  const uint32_t nb = fn.nblocks();
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nv) return false;
  bool changed = false;
  for (uint32_t h = 0; h < A.n; ++h) {
    if (A.inner[h] != h) continue; // not a loop header
    bool folded = [&]() -> bool {
    auto row = A.loops.range()[h];
    // sized to the CURRENT block count: earlier folds in this same pass
    // call appended closed-form blocks (ids >= A.n); they are outside this
    // loop, so 0 is the right default for them, and the succ scan below may
    // legitimately index them
    std::vector<char> inL(fn.nblocks(), 0);
    for (ulen x : row) inL[(uint32_t)x] = 1;
    inL[h] = 1;
    // exactly one outside pred (preheader) + one latch
    uint32_t pre = NONE, L = NONE;
    for (uint32_t p : fn.preds[h]) {
      if (inL[p]) {
        if (L != NONE) return false; // multiple latches (multi-site self)
        L = p;
      } else {
        if (pre != NONE) return false;
        pre = p;
      }
    }
    if (pre == NONE || L == NONE) return false;
    // (the latch ends with `br h` when a merge chain exists, or with the
    // budget check `cbr ? T : h` when the site block IS the latch; the
    // exit classification below validates the trap edge either way)
    // ---- collect + classify the header phis ---------------------------------
    struct HP {
      uint32_t dst, entry, latch;
    };
    std::vector<HP> hp;
    for (uint32_t i = fn.blocks[h].begin; i < fn.blocks[h].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k != K::Phi) break;
      uint32_t e = (I.t == pre) ? I.a : ((I.f == pre) ? I.b2 : NONE);
      uint32_t l = (I.t == L) ? I.a : ((I.f == L) ? I.b2 : NONE);
      if (e == NONE || l == NONE) return false;
      hp.push_back({I.dst, e, l});
    }
    if (hp.size() < 2) return false; // nP + bH at least
    std::vector<uint32_t> hids;
    for (auto &p : hp) hids.push_back(p.dst);
    // resolve each latch value; classify
    struct Cls {
      uint32_t phi;      // the H-phi vreg
      uint32_t entry;    // entry (pre) value vreg
      CdAff laff;        // latch evolution, affine over hphis
      bool is_sel{};     // selector (latch merges per-arm CONSTS)
    };
    std::vector<Cls> cls;
    for (auto &p : hp) {
      CdAff la{};
      if (cd_resolve(fn, p.latch, hids, la, 0)) {
        cls.push_back({p.dst, p.entry, la, false});
        continue;
      }
      // selector? the latch is a Phi of CONSTS selected by the dispatch
      // (resolve failed => not uniform affine) -- verify structurally:
      uint32_t di = NONE, db = NONE;
      for (uint32_t b = 0; b < nb; ++b)
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
          if (fn.code[i].dst == p.latch) {
            di = i;
            db = b;
          }
      if (di == NONE || fn.code[di].k != K::Phi) return false;
      cls.push_back({p.dst, p.entry, CdAff{}, true});
      (void)db;
    }
    // exactly one selector at most
    uint32_t selp = NONE;
    for (auto &c : cls)
      if (c.is_sel) {
        if (selp != NONE) return false;
        selp = c.phi;
      }
    // find nP and bH: nP's latch affine = nP - 1; bH's = bH - 1
    uint32_t nP = NONE, bH = NONE, nEntry = NONE, bEntry = NONE;
    for (auto &c : cls) {
      if (c.is_sel) continue;
      if (c.laff.t.size() == 1 && c.laff.t[0].first == c.phi &&
          c.laff.t[0].second == 1 && c.laff.c == (val_t)-1) {
        if (nP == NONE) {
          nP = c.phi;
          nEntry = c.entry;
        } else if (bH == NONE) {
          bH = c.phi;
          bEntry = c.entry;
        } else
          return false; // a third pure countdown: unsupported
      }
    }
    if (nP == NONE || bH == NONE) return false;
    // any remaining non-selector phis must be additive/identity over
    // {self, nP}: x' = x + g (+ b*nP)
    struct Add {
      uint32_t phi, entry;
      val_t g, bn;
    };
    std::vector<Add> adds;
    for (auto &c : cls) {
      if (c.is_sel || c.phi == nP || c.phi == bH) continue;
      val_t g = 0, bn = 0;
      bool ok = true;
      for (auto &pr : c.laff.t) {
        if (pr.first == c.phi) {
          if (pr.second != 1) ok = false;
        } else if (pr.first == nP) {
          bn = pr.second;
        } else
          ok = false;
      }
      if (!ok || c.laff.t.size() > 2) return false;
      for (auto &pr : c.laff.t)
        if (pr.first == c.phi && pr.second != 1) return false;
      // require self coefficient exactly 1 (additive) — it is in t only once
      bool hasSelf = false;
      for (auto &pr : c.laff.t)
        if (pr.first == c.phi) hasSelf = true;
      if (!hasSelf) return false; // pure function of nP: unsupported here
      adds.push_back({c.phi, c.entry, g, bn});
      // g = the constant part
      adds.back().g = c.laff.c;
    }
    // ---- the dispatch / base / trap structure --------------------------------
    // dispatch: H's terminator (single member: the base test itself)
    const Instr hterm = fn.code[fn.blocks[h].end - 1];
    uint32_t Xt = NONE, Xf = NONE; // value exits (true/false dispatch arms)
    uint32_t T = NONE;             // the common trap exit
    auto is_np_zero = [&](const Instr &C, uint32_t &xt, uint32_t &af) -> bool {
      // C: cbr cond ? a : b where cond = Bin(nP == 0): the ==0 arm exits
      if (C.k != K::Cbr) return false;
      uint32_t di = NONE;
      for (uint32_t b = 0; b < nb; ++b)
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
          if (fn.code[i].dst == C.a) di = i;
      if (di == NONE) return false;
      const Instr &D = fn.code[di];
      if (D.k != K::Bin || D.b != B::Eq) return false;
      bool aIsN = (D.a == nP);
      bool bIsN = (D.b2 == nP);
      uint32_t other = aIsN ? D.b2 : D.b2;
      other = aIsN ? D.b2 : (bIsN ? D.a : NONE);
      if (other == NONE) return false;
      // the other operand must be const 0
      uint32_t oi = NONE;
      for (uint32_t b = 0; b < nb; ++b)
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
          if (fn.code[i].dst == other) oi = i;
      if (oi == NONE || fn.code[oi].k != K::Const || fn.code[oi].c != 0)
        return false;
      // which arm is the n==0 (base) arm? Eq is symmetric: TRUE arm = equal
      xt = C.t;
      af = C.f;
      return true;
    };
    auto is_bh_zero_trap = [&](const Instr &C, uint32_t &tt) -> bool {
      if (C.k != K::Cbr) return false;
      uint32_t di = NONE;
      for (uint32_t b = 0; b < nb; ++b)
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
          if (fn.code[i].dst == C.a) di = i;
      if (di == NONE) return false;
      const Instr &D = fn.code[di];
      if (D.k != K::Bin || D.b != B::Eq) return false;
      uint32_t other = (D.a == bH) ? D.b2
                                    : ((D.b2 == bH) ? D.a : NONE);
      if (other == NONE) return false;
      uint32_t oi = NONE;
      for (uint32_t b = 0; b < nb; ++b)
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
          if (fn.code[i].dst == other) oi = i;
      if (oi == NONE || fn.code[oi].k != K::Const || fn.code[oi].c != 0)
        return false;
      tt = C.t; // the ==0 arm traps
      return true;
    };
    // every block WITH an outside edge must exit via a base test (nP==0
    // at a member block) or the budget trap (bH==0); the dispatch header
    // and the latch have only internal successors and are skipped here
    std::vector<uint32_t> Xs;
    for (uint32_t b = 0; b < nb; ++b) {
      if (!inL[b]) continue;
      bool has_out = false;
      for (uint32_t s : {fn.succ0(b), fn.succ1(b)})
        if (s != NONE && !inL[s]) has_out = true;
      if (!has_out) continue;
      const Instr &tm = fn.code[fn.blocks[b].end - 1];
      uint32_t xt = NONE, af = NONE, tt = NONE;
      if (is_np_zero(tm, xt, af)) {
        if (inL[xt] || !inL[af]) return false;
        Xs.push_back(xt);
        continue;
      }
      if (is_bh_zero_trap(tm, tt)) {
        if (inL[tt]) return false;
        if (T == NONE) T = tt;
        else if (T != tt) return false;
        continue;
      }
      return false; // some other exit: keep the loop
    }
    if (T == NONE) return false; // no trap edge: not the TCE shape
    // Xs: dedupe
    {
      std::vector<uint32_t> u;
      for (uint32_t x : Xs) {
        bool dup = false;
        for (uint32_t y : u) dup |= (x == y);
        if (!dup) u.push_back(x);
      }
      Xs = u;
    }
    if (Xs.empty() || Xs.size() > 2) return false;
    // X blocks (the base-case exits): a Br to a common next (or a Ret),
    // at most ONE Const (the member's base value when there is one), and
    // nothing that uses a loop phi except the RET OPERAND (an accumulator
    // returns a loop-carried value; the outside-use rewrite below replaces
    // it with the closed form).
    std::vector<uint32_t> kval(Xs.size(), NONE);
    uint32_t xnext = NONE;
    bool xret = false;
    for (size_t xi = 0; xi < Xs.size(); ++xi) {
      uint32_t b = Xs[xi];
      const Instr &tm = fn.code[fn.blocks[b].end - 1];
      if (tm.k == K::Ret) {
        if (tm.a == NONE) return false;
        xret = true;
        // the ret operand: a Const def here, or a loop phi (accumulator)
        uint32_t di = NONE;
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
          if (fn.code[i].dst == tm.a) di = i;
        bool isLphi = false;
        for (uint32_t hid : hids) isLphi |= (tm.a == hid);
        if (di != NONE && fn.code[di].k == K::Const) kval[xi] = fn.code[di].c;
        else if (!isLphi) return false;
      } else if (tm.k == K::Br) {
        if (xret) return false;
        if (xnext == NONE) xnext = tm.t;
        else if (xnext != tm.t) return false;
        uint32_t nconst = 0;
        for (uint32_t i = fn.blocks[b].begin; i + 1 < fn.blocks[b].end; ++i) {
          const Instr &I = fn.code[i];
          if (I.k == K::Const) {
            ++nconst;
            kval[xi] = I.c;
          } else if (I.k == K::Phi)
            continue;
          else
            return false;
        }
        if (nconst > 1) return false;
        if (nconst == 0 && selp != NONE)
          return false; // the member select needs its base consts
      } else
        return false;
      // no loop-phi uses inside X (except a Ret operand, handled above)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        auto usesL = [&](uint32_t v) {
          for (uint32_t hid : hids)
            if (v == hid) return true;
          return false;
        };
        if (i + 1 == fn.blocks[b].end && I.k == K::Ret) {
          if (usesL(I.b2) || usesL(I.c2)) return false;
          continue; // I.a checked above
        }
        if (usesL(I.a) || usesL(I.b2) || usesL(I.c2)) return false;
      }
    }
    // ---- selector soundness: sel in {0,1} ------------------------------------
    // entry: a Const in {0,1}, or the fn's sel param under Fn::tce_sel == 2
    // toggle consts in {0,1}: checked from the latch phi args
    uint32_t selEntry = NONE;
    bool selOk = false;
    if (selp != NONE) {
      for (auto &p : hp)
        if (p.dst == selp) selEntry = p.entry;
      uint32_t di = NONE;
      for (uint32_t b = 0; b < nb; ++b)
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
          if (fn.code[i].dst == selEntry) di = i;
      if (di != NONE && fn.code[di].k == K::Const &&
          (fn.code[di].c == 0 || fn.code[di].c == 1))
        selOk = true;
      if (!selOk && fn.tce_sel == 2 && selEntry == 0) selOk = true;
      if (!selOk) return false;
      // find the sel latch phi (the L-phi feeding selp) and check its args
      uint32_t selLatch = NONE;
      for (auto &p : hp)
        if (p.dst == selp) selLatch = p.latch;
      uint32_t si = NONE;
      for (uint32_t b = 0; b < nb; ++b)
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
          if (fn.code[i].dst == selLatch) si = i;
      if (si == NONE) return false;
      // its args must be consts in {0,1}, and DISTINCT
      {
        uint32_t a1 = fn.code[si].a, b1 = fn.code[si].b2;
        uint32_t ia = NONE, ib = NONE;
        for (uint32_t b = 0; b < nb; ++b)
          for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
            if (fn.code[i].dst == a1) ia = i;
            if (fn.code[i].dst == b1) ib = i;
          }
        if (ia == NONE || ib == NONE) return false;
        if (fn.code[ia].k != K::Const || fn.code[ib].k != K::Const)
          return false;
        val_t ca = fn.code[ia].c, cb = fn.code[ib].c;
        if ((ca != 0 && ca != 1) || (cb != 0 && cb != 1) || ca == cb)
          return false;
        // A = cb - ca must be -1 or +1 (a toggle or identity)
        val_t A = (val_t)((uint64_t)cb - (uint64_t)ca);
        if (A != 1 && A != (val_t)-1) return false;
      }
    }
    // ---- body purity: only Phi/Const/Copy/Bin(nt)/Br/Cbr in the loop -------
    for (uint32_t b = 0; b < nb; ++b) {
      if (!inL[b]) continue;
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        switch (I.k) {
        case K::Phi:
        case K::Const:
        case K::Copy:
        case K::Br:
        case K::Cbr:
          break;
        case K::Bin:
          if ((I.b == B::Div || I.b == B::Mod) && !I.nt) return false;
          break;
        default:
          return false; // Print/Call/Input/Alloc/Load/Store/Len: observable
        }
      }
    }
    // ---- fold -----------------------------------------------------------------
    // guard (unsigned n0 > bInit) -> T ; closed forms -> select / X
    {
      std::vector<Instr> g{}; // appended to the preheader
      auto konst = [&](val_t c) {
        uint32_t v = new_anon_vreg(fn, 0);
        g.push_back({.k = K::Const, .dst = v, .c = c});
        return v;
      };
      auto bin = [&](B b, uint32_t d, uint32_t a, uint32_t b2) {
        g.push_back({.k = K::Bin, .b = b, .dst = d, .a = a, .b2 = b2});
      };
      auto nt = [&]() { return new_anon_vreg(fn, 0); };
      const uint32_t MINc = konst(INT64_MIN);
      const uint32_t xN = nt();
      bin(B::Xor, xN, nEntry, MINc);
      const uint32_t xB = nt();
      bin(B::Xor, xB, bEntry, MINc);
      const uint32_t bad = nt();
      bin(B::Lt, bad, xB, xN); // unsigned bInit < N  <=>  N > bInit
      // CF block: closed forms + the select
      std::vector<Instr> cf{};
      auto ckonst = [&](val_t c) {
        uint32_t v = new_anon_vreg(fn, 0);
        cf.push_back({.k = K::Const, .dst = v, .c = c});
        return v;
      };
      auto cbin = [&](B b, uint32_t d, uint32_t a, uint32_t b2) {
        cf.push_back({.k = K::Bin, .b = b, .dst = d, .a = a, .b2 = b2});
      };
      // p = n0 & 1
      const uint32_t oneC = ckonst(1);
      const uint32_t pV = nt();
      cbin(B::And, pV, nEntry, oneC);
      uint32_t selN = NONE;
      if (selp != NONE) {
        // selN = (1-2p)*selEntry + p   (exact for every selEntry; the
        // SELECT below additionally needs sel in {0,1}, proven above)
        const uint32_t two = nt();
        cbin(B::Add, two, pV, pV);
        const uint32_t k1 = nt();
        cbin(B::Sub, k1, oneC, two);
        const uint32_t m = nt();
        cbin(B::Mul, m, k1, selEntry);
        selN = nt();
        cbin(B::Add, selN, m, pV);
      }
      // additive closed forms: x_N = x0 + g*N + bn*(N*n0 - T), T = N(N-1)/2
      std::vector<std::pair<uint32_t, uint32_t>> xform; // (H-phi -> value)
      if (!adds.empty()) {
        const uint32_t e0 = ckonst(1); // reuse oneC
        const uint32_t two = nt();
        cbin(B::Add, two, nEntry, nEntry); // 2N (parity of N: N&1 — have pV)
        // T = (N - (N&1))/2 * (N + (N&1) - 1)  [exact, same trick as lcf rt]
        const uint32_t Nm = nt();
        cbin(B::Sub, Nm, nEntry, pV);
        const uint32_t h2 = nt();
        cbin(B::LShr, h2, Nm, e0);
        const uint32_t t1 = nt();
        cbin(B::Add, t1, nEntry, pV);
        const uint32_t o1 = nt();
        cbin(B::Sub, o1, t1, e0);
        const uint32_t Tv = nt();
        cbin(B::Mul, Tv, h2, o1);
        const uint32_t NN = nt(); // N*n0
        cbin(B::Mul, NN, nEntry, nEntry);
        for (auto &ad : adds) {
          // x_N = x0 + g*N + bn*(N*n0 - T)
          uint32_t acc = ad.entry;
          if (ad.g != 0) {
            const uint32_t gn = nt();
            cbin(B::Mul, gn, ckonst(ad.g), nEntry);
            const uint32_t a2 = nt();
            cbin(B::Add, a2, acc, gn);
            acc = a2;
          }
          if (ad.bn != 0) {
            const uint32_t sn = nt();
            cbin(B::Sub, sn, NN, Tv);
            const uint32_t bt = nt();
            cbin(B::Mul, bt, sn, ckonst(ad.bn));
            const uint32_t a3 = nt();
            cbin(B::Add, a3, acc, bt);
            acc = a3;
          }
          xform.push_back({ad.phi, acc});
        }
      }
      // n_N = 0; b_N = bInit - N (only if used outside)
      const uint32_t zeroC = ckonst(0);
      xform.push_back({nP, zeroC});
      // ---- rewrite outside uses of the H-phis --------------------------------
      // (X blocks use none; T uses none; anywhere else outside inL —
      // including closed-form blocks appended by earlier folds in this same
      // pass call, which never reference this loop's phis but are covered
      // here anyway so the rewrite is complete by construction)
      for (uint32_t b = 0; b < fn.nblocks(); ++b) {
        if (b >= inL.size() || inL[b]) continue;
        if (b == pre) continue; // handled below (guard emission)
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
          Instr &I = fn.code[i];
          auto rwx = [&](uint32_t &v) {
            if (v == NONE) return;
            if (v == selp && selN != NONE) v = selN;
            for (auto &xf : xform)
              if (v == xf.first) v = xf.second;
          };
          rwx(I.a);
          rwx(I.b2);
          rwx(I.c2);
          for (uint32_t &av : I.args) rwx(av);
        }
      }
      // ---- emit: pre gets the guard; CF becomes a new block -----------------
      const uint32_t CFb = fn.nblocks(); // appended block id — CURRENT
      // count: an earlier fold in this same pass call already appended its
      // own CF block, and reusing the entry-time count would collide with it
      // pre's terminator: br h -> cbr bad ? T : CF
      {
        Instr &pt = fn.code[fn.blocks[pre].end - 1];
        if (pt.k != K::Br || pt.t != h) return false;
        pt.k = K::Cbr;
        pt.a = bad;
        pt.t = T;
        pt.f = CFb;
      }
      // CF terminator: the select (multi) or br X (single member)
      if (selp != NONE) {
        if (Xs.size() != 2) return false;
        // the header dispatches on sel: hterm's cond must be Eq(selH, ..)
        uint32_t di = NONE;
        for (uint32_t b = 0; b < nb; ++b)
          for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
            if (fn.code[i].dst == hterm.a) di = i;
        if (di == NONE || fn.code[di].k != K::Bin ||
            fn.code[di].b != B::Eq || fn.code[di].a != selp)
          return false;
        // sel == 1 -> TRUE arm; walk E_true to its X
        uint32_t Et = hterm.t;
        if (Et >= nb) return false;
        const Instr &et = fn.code[fn.blocks[Et].end - 1];
        uint32_t xt2 = NONE, af2 = NONE;
        if (!is_np_zero(et, xt2, af2)) return false;
        uint32_t Xtrue = xt2, Xfalse = NONE;
        for (uint32_t x : Xs)
          if (x != Xtrue) Xfalse = x;
        if (Xfalse == NONE) return false;
        cf.push_back({.k = K::Cbr, .a = selN, .t = Xtrue, .f = Xfalse});
      } else {
        if (Xs.size() != 1) return false;
        cf.push_back({.k = K::Br, .t = Xs[0]});
      }
      // splice g into pre (before its terminator) and append CF
      {
        uint32_t pe = fn.blocks[pre].end - 1;
        fn.code.insert(fn.code.begin() + (long)pe, g.begin(), g.end());
        const uint32_t add = (uint32_t)g.size();
        // shift EVERY current block range — including CF blocks appended by
        // earlier folds in this same pass call, whose instrs sit at the end
        // of the code vector and move when g is spliced in (a stale entry-
        // time bound here left their ranges pointing into unrelated code)
        for (uint32_t b = 0; b < fn.nblocks(); ++b) {
          if (b == pre) {
            fn.blocks[b].end += add;
            continue;
          }
          if (fn.blocks[b].begin >= pe + 1) fn.blocks[b].begin += add;
          if (fn.blocks[b].end > pe) fn.blocks[b].end += add;
        }
      }
      fn.code.insert(fn.code.end(), cf.begin(), cf.end());
      fn.blocks.push_back({uint32_t(fn.code.size() - cf.size()),
                           (uint32_t)fn.code.size()});
      fn.succ.push_back(NONE);
      fn.succ.push_back(NONE);
      // pre -> T / CF ; CF -> its terminator target
      fn.succ[2 * pre] = T;
      fn.succ[2 * pre + 1] = CFb;
      {
        const Instr &ct = fn.code[fn.blocks[CFb].end - 1];
        if (ct.k == K::Br) fn.succ[2 * CFb] = ct.t;
        else if (ct.k == K::Cbr) {
          fn.succ[2 * CFb] = ct.t;
          fn.succ[2 * CFb + 1] = ct.f;
        }
      }
      // the loop is now unreachable: succ severed AND the blocks' code
      // ranges emptied — a Br-with-NONE-target left in the code vector
      // would crash the bytecode emitter (off[f][NONE]) even though the
      // block is dead; empty ranges are inert everywhere
      for (uint32_t b = 0; b < nb; ++b) {
        if (!inL[b]) continue;
        fn.succ[2 * b] = NONE;
        fn.succ[2 * b + 1] = NONE;
        fn.blocks[b].end = fn.blocks[b].begin; // drop every instr
      }
      fn.recompute_preds();
      os.cdfolds++;
      changed = true;
    }
  return true;
  }();
  }
  return changed;
}

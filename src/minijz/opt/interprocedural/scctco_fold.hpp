#pragma once
// === scctco_fold.hpp ========================================
// the scctco residue fold (countdown-by-c -> n & (c-1) etc.).

#include "minijz/base/pass_cfg.hpp"
#include "minijz/ir/instr.hpp"
#include "minijz/opt/interprocedural/callgraph.hpp"

// (the collapse helpers live in scctco.hpp; forward-declared so
// this header can also be included standalone)
constexpr void scctco_retarget(Fn &fn, uint32_t b, uint32_t from, uint32_t to);

// ---- the residue fold (runs on the pristine collapsed shape) ---------------

// affine value in terms of V: V + delta, or a constant
struct ScctcoAff {
  bool ok{}, is_const{};
  int64_t delta{}, konst{};
};

constexpr ScctcoAff scctco_affine(const Fn &fn,
                                  const std::vector<uint32_t> &def_of,
                                  uint32_t v, uint32_t V) {
  if (v == V)
    return {true, false, 0, 0};
  if (v == NONE || v >= def_of.size() || def_of[v] == NONE)
    return {false, false, 0, 0};
  const Instr &D = fn.code[def_of[v]];
  if (D.k == K::Const)
    return {true, true, 0, D.c};
  if (D.k == K::Copy)
    return scctco_affine(fn, def_of, D.a, V);
  if (D.k == K::Phi) { // a merge chain phi: both args must agree
    ScctcoAff x = scctco_affine(fn, def_of, D.a, V);
    ScctcoAff y = scctco_affine(fn, def_of, D.b2, V);
    if (x.ok && y.ok && !x.is_const && !y.is_const && x.delta == y.delta)
      return {true, false, x.delta, 0};
    return {false, false, 0, 0};
  }
  if (D.k == K::Bin && D.b2 != NONE && D.b2 < def_of.size() &&
      def_of[D.b2] != NONE && fn.code[def_of[D.b2]].k == K::Const) {
    const int64_t k = fn.code[def_of[D.b2]].c;
    if (D.b == B::Add || D.b == B::Sub) {
      ScctcoAff r = scctco_affine(fn, def_of, D.a, V);
      if (!r.ok || r.is_const)
        return {false, false, 0, 0};
      return {true, false, r.delta + (D.b == B::Add ? k : -k), 0};
    }
  }
  return {false, false, 0, 0};
}

// Recognize the count-down residue loop on the collapsed member and fold
// it to  cap-guard + V0 & (c-1).  H is the loop header (block 1).
constexpr void scctco_fold(Fn &f, uint32_t H, uint32_t nparams, OptStats &os,
                           uint32_t trapb, uint8_t dbg) {
#define SCCTCO_BAIL(why)                                                       \
  do {                                                                         \
    if (dbg >= 1)                                                              \
      std::printf("  [scctco] fold bail: %s\n", why);                          \
    return;                                                                    \
  } while (0)
  const uint32_t nb = f.nblocks();
  // ---- collect H's phis: [V_0..V_{n-1}, trip] (insertion order) ----------
  std::vector<uint32_t> vphis;
  uint32_t trip_phi = NONE;
  for (uint32_t i = f.blocks[H].begin; i < f.blocks[H].end; ++i) {
    const Instr &P = f.code[i];
    if (P.k != K::Phi)
      break;
    if (i + 1 < f.blocks[H].end && f.code[i + 1].k != K::Phi)
      trip_phi = P.dst; // the last phi is the trip counter
    else
      vphis.push_back(P.dst);
  }
  if (trip_phi == NONE || vphis.size() != 1)
    SCCTCO_BAIL("phi shape");
  const uint32_t V = vphis[0];
  if (nparams != 1)
    SCCTCO_BAIL("nparams != 1");

  // ---- the latch (the pred that is not the entry edge D) -----------------
  if (f.preds[H].size() != 2)
    SCCTCO_BAIL("preds != 2");
  uint32_t root = NONE, latch = NONE;
  for (uint32_t p : f.preds[H]) {
    if (p == 0)
      root = p;
    else
      latch = p;
  }
  if (root != 0 || latch == NONE)
    SCCTCO_BAIL("root/latch");

  uint32_t V0 = NONE, Vl = NONE;
  for (uint32_t i = f.blocks[H].begin; i < f.blocks[H].end; ++i) {
    const Instr &P = f.code[i];
    if (P.k != K::Phi)
      break;
    if (P.dst != V)
      continue;
    if (P.t == root)
      V0 = P.a;
    if (P.f == root)
      V0 = P.b2;
    if (P.t == latch)
      Vl = P.a;
    if (P.f == latch)
      Vl = P.b2;
  }
  if (V0 == NONE || Vl == NONE)
    SCCTCO_BAIL("V0/Vl");

  // ---- def map (for affine resolution) -----------------------------------
  std::vector<uint32_t> def_of(f.nvars + (uint32_t)f.version_base.size(), NONE);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = f.blocks[b].begin; i < f.blocks[b].end; ++i)
      if (f.code[i].dst != NONE && f.code[i].dst < def_of.size())
        def_of[f.code[i].dst] = i;

  // step: Vl must be V - c  (0 < c <= 8, a power of two; the <=8 bound
  // keeps the threshold simulation under 512*8+8 runs)
  ScctcoAff al = scctco_affine(f, def_of, Vl, V);
  if (!al.ok || al.is_const || al.delta >= 0 || al.delta < -8)
    SCCTCO_BAIL("step");
  const uint64_t c = (uint64_t)(-al.delta);
  if ((c & (c - 1)) != 0)
    SCCTCO_BAIL("c not pow2");

  // ---- loop region: reachable from H; the loop proper reaches H back -----
  std::vector<char> reach(nb, 0), back(nb, 0);
  {
    std::vector<uint32_t> wl{H};
    reach[H] = 1;
    while (!wl.empty()) {
      uint32_t b = wl.back();
      wl.pop_back();
      for (uint32_t s : {f.succ0(b), f.succ1(b)}) {
        if (s == NONE || s >= nb || reach[s])
          continue;
        reach[s] = 1;
        wl.push_back(s);
      }
    }
    std::vector<uint32_t> rw{H};
    back[H] = 1;
    while (!rw.empty()) {
      uint32_t b = rw.back();
      rw.pop_back();
      for (uint32_t p : f.preds[b]) {
        if (p >= nb || back[p] || !reach[p])
          continue;
        back[p] = 1;
        rw.push_back(p);
      }
    }
  }
  // purity of the loop proper
  for (uint32_t b = 0; b < nb; ++b) {
    if (!back[b])
      continue;
    for (uint32_t i = f.blocks[b].begin; i < f.blocks[b].end; ++i) {
      const Instr &I = f.code[i];
      if (I.k == K::Phi || I.k == K::Const || I.k == K::Copy || I.k == K::Br ||
          I.k == K::Cbr)
        continue;
      if (I.k == K::Bin && I.b != B::Div && I.b != B::Mod)
        continue;
      SCCTCO_BAIL("impure loop body");
    }
  }
  // ---- exits: cbr arms into non-back reachable blocks --------------------
  std::vector<int64_t> exit_val(c, -1); // residue r -> returned constant
  for (uint32_t b = 0; b < nb; ++b) {
    if (!back[b])
      continue;
    const uint32_t e = f.blocks[b].end;
    if (!e || f.code[e - 1].k != K::Cbr)
      continue;
    const Instr &CB = f.code[e - 1];
    for (uint32_t arm : {CB.t, CB.f}) {
      if (arm == NONE || arm >= nb || back[arm])
        continue;
      if (arm == trapb)
        continue; // the cap guard, not a semantic exit
      // `arm` must be [consts.., ret const]
      uint32_t rv = NONE;
      bool pure = true;
      for (uint32_t i = f.blocks[arm].begin; i < f.blocks[arm].end; ++i) {
        const Instr &I = f.code[i];
        if (I.k == K::Const)
          continue;
        if (I.k == K::Ret && i + 1 == f.blocks[arm].end) {
          rv = I.a;
          continue;
        }
        pure = false;
        break;
      }
      if (!pure || rv == NONE || rv >= def_of.size() || def_of[rv] == NONE ||
          f.code[def_of[rv]].k != K::Const)
        SCCTCO_BAIL("exit not const-ret");
      const int64_t vret = f.code[def_of[rv]].c;
      // the condition: Eq(x, y), one side affine in V, other side const
      if (CB.a == NONE || CB.a >= def_of.size() || def_of[CB.a] == NONE)
        return;
      const Instr &CM = f.code[def_of[CB.a]];
      if (CM.k != K::Bin || CM.b != B::Eq)
        SCCTCO_BAIL("cond not Eq");
      ScctcoAff x = scctco_affine(f, def_of, CM.a, V);
      ScctcoAff y = scctco_affine(f, def_of, CM.b2, V);
      if (!x.ok || !y.ok)
        SCCTCO_BAIL("cond not affine");
      int64_t r = -1;
      if (!x.is_const && y.is_const)
        r = y.konst - x.delta;
      else if (x.is_const && !y.is_const)
        r = x.konst - y.delta;
      else
        SCCTCO_BAIL("cond both non-const");
      if (r < 0 || (uint64_t)r >= c)
        SCCTCO_BAIL("residue range");
      if (exit_val[(uint32_t)r] != -1)
        SCCTCO_BAIL("dup residue");
      exit_val[(uint32_t)r] = vret;
    }
  }
  for (uint64_t r = 0; r < c; ++r)
    if (exit_val[(uint32_t)r] != (int64_t)r)
      SCCTCO_BAIL("v_r != r");

  // ---- exact trap threshold by SIMULATION -------------------------------
  // The static 512*c guess is wrong for multi-edge cycles (mutual recursion
  // makes n+1 calls, not n/2+1: each SCC edge is one frame). The loop is
  // pure and every run trips the guard within 512 edges, so simulating it
  // for n = 0, 1, 2, ... finds the exact first-trapping input. Monotone in
  // n (edges = E*floor(n/c) + o(n mod c), all offsets < E), and edges(n) >=
  // floor(n/c), so a trap is always found by n = 512*c.
  const uint32_t nv = f.nvars + (uint32_t)f.version_base.size();
  auto simulate = [&](int64_t n, bool &trapped, int64_t &out) -> bool {
    std::vector<val_t> val(nv, 0);
    std::vector<char> setv(nv, 0);
    auto read = [&](uint32_t v) -> bool {
      if (v == NONE || v >= nv)
        return false;
      if (!setv[v]) {
        if (v >= def_of.size() || def_of[v] == NONE)
          return false;
        const Instr &D2 = f.code[def_of[v]];
        if (D2.k != K::Const)
          return false; // unknown outer value
        val[v] = D2.c;
        setv[v] = 1;
      }
      return true;
    };
    val[V0] = n;
    setv[V0] = 1;
    uint32_t cur = 0, from = NONE; // enter D
    for (uint32_t steps = 0; steps < 100000; ++steps) {
      if (cur >= nb)
        return false;
      for (uint32_t i = f.blocks[cur].begin; i < f.blocks[cur].end; ++i) {
        const Instr &I = f.code[i];
        if (I.k == K::Phi) {
          uint32_t arg = (I.t == from) ? I.a : ((I.f == from) ? I.b2 : NONE);
          if (arg == NONE || !read(arg))
            return false;
          if (I.dst < nv) {
            val[I.dst] = val[arg];
            setv[I.dst] = 1;
          }
          continue;
        }
        if (I.k == K::Const) {
          if (I.dst < nv) {
            val[I.dst] = I.c;
            setv[I.dst] = 1;
          }
          continue;
        }
        if (I.k == K::Copy) {
          if (!read(I.a))
            return false;
          if (I.dst < nv) {
            val[I.dst] = val[I.a];
            setv[I.dst] = 1;
          }
          continue;
        }
        if (I.k == K::Bin) {
          if (!read(I.a) || !read(I.b2))
            return false;
          Rv r2 = w_bin(I.b, val[I.a], val[I.b2]);
          if (r2.trap) { // Div/Mod cannot appear (purity), paranoia
            trapped = true;
            return true;
          }
          if (I.dst < nv) {
            val[I.dst] = r2.v;
            setv[I.dst] = 1;
          }
          continue;
        }
        if (I.k == K::Br || I.k == K::Cbr || I.k == K::Ret)
          break;      // terminator: handled below the loop
        return false; // anything else: unexpected shape
      }
      const Instr &T2 = f.code[f.blocks[cur].end - 1];
      if (T2.k == K::Ret) {
        if (!read(T2.a))
          return false;
        out = val[T2.a];
        trapped = false;
        return true;
      }
      uint32_t nxt = NONE;
      if (T2.k == K::Br)
        nxt = T2.t;
      else if (T2.k == K::Cbr) {
        if (!read(T2.a))
          return false;
        nxt = (val[T2.a] != 0) ? T2.t : T2.f;
      } else
        return false;
      if (nxt == trapb) {
        trapped = true; // the guard: never execute the trap block
        return true;
      }
      from = cur;
      cur = nxt;
    }
    return false; // diverged past the step cap: should not happen
  };
  uint64_t T = 0;
  bool found = false;
  // The value shape is already proven statically (v_r == r); what the
  // simulation adds is the EXACT first-trapping input T. Trapping is
  // monotone in n (edges = E*floor(n/c) + o(n mod c)), so binary search
  // finds T in ~log2(512*c) runs instead of a linear scan -- which also
  // keeps the constexpr (static_assert) build's evaluation state small.
  {
    bool tr = false;
    int64_t out = 0;
    if (!simulate(0, tr, out))
      SCCTCO_BAIL("sim failed");
    if (tr)
      SCCTCO_BAIL("traps at n=0 (empty budget?)");
    if (out != 0)
      SCCTCO_BAIL("sim value mismatch");
    uint64_t lo = 0, hi = 512 * c + 8; // traps somewhere in (lo, hi]
    for (uint32_t guard = 0; guard < 32 && hi - lo > 1; ++guard) {
      uint64_t mid = lo + (hi - lo) / 2;
      if (!simulate((int64_t)mid, tr, out))
        SCCTCO_BAIL("sim failed");
      if (tr)
        hi = mid;
      else {
        if (out != (int64_t)(mid & (c - 1)))
          SCCTCO_BAIL("sim value mismatch");
        lo = mid;
      }
    }
    if (hi - lo != 1)
      SCCTCO_BAIL("no trap found");
    T = hi;
    found = true;
  }

  // ---- fold: guard + strength-reduced residue -----------------------------
  if (trapb == NONE)
    SCCTCO_BAIL("no trap block");
  const uint32_t base0 = f.base_of(V0);
  const uint32_t z0 = new_anon_vreg(f, base0);
  const uint32_t KC = new_anon_vreg(f, base0);
  const uint32_t g1 = new_anon_vreg(f, base0);
  const uint32_t g2 = new_anon_vreg(f, base0);
  const uint32_t gc = new_anon_vreg(f, base0);
  const uint32_t FOLD = f.nblocks();
  { // guard block: V0 < 0 || V0 >= T -> trap, else fold
    std::vector<Instr> gb;
    gb.push_back(Instr{.k = K::Const, .dst = z0, .c = 0});
    gb.push_back(Instr{.k = K::Const, .dst = KC, .c = (int64_t)T});
    gb.push_back(Instr{.k = K::Bin, .b = B::Lt, .dst = g1, .a = V0, .b2 = z0});
    gb.push_back(Instr{.k = K::Bin, .b = B::Ge, .dst = g2, .a = V0, .b2 = KC});
    gb.push_back(Instr{.k = K::Bin, .b = B::Or, .dst = gc, .a = g1, .b2 = g2});
    gb.push_back(Instr{.k = K::Cbr, .a = gc, .t = trapb, .f = FOLD + 1});
    uint32_t base = (uint32_t)f.code.size();
    f.code.insert(f.code.end(), gb.begin(), gb.end());
    f.blocks.push_back({base, (uint32_t)f.code.size()});
    f.succ.push_back(trapb);
    f.succ.push_back(FOLD + 1);
  }
  { // fold block: ret V0 & (c-1)   (c == 1: ret 0)
    std::vector<Instr> fb;
    if (c > 1) {
      const uint32_t m1 = new_anon_vreg(f, base0);
      const uint32_t res = new_anon_vreg(f, base0);
      fb.push_back(Instr{.k = K::Const, .dst = m1, .c = (int64_t)(c - 1)});
      fb.push_back(
          Instr{.k = K::Bin, .b = B::And, .dst = res, .a = V0, .b2 = m1});
      fb.push_back(Instr{.k = K::Ret, .a = res});
    } else {
      const uint32_t r0 = new_anon_vreg(f, base0);
      fb.push_back(Instr{.k = K::Const, .dst = r0, .c = 0});
      fb.push_back(Instr{.k = K::Ret, .a = r0});
    }
    uint32_t base = (uint32_t)f.code.size();
    f.code.insert(f.code.end(), fb.begin(), fb.end());
    f.blocks.push_back({base, (uint32_t)f.code.size()});
    f.succ.push_back(NONE);
    f.succ.push_back(NONE);
  }
  scctco_retarget(f, 0, H, FOLD); // D: br H -> br FOLD
  os.scctco_folds++;
}

#undef SCCTCO_BAIL

// ---- collapse one SCC member ------------------------------------------------

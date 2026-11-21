#pragma once
// === scctco_fold_impl.hpp ========================================
// the SCC-TCO RESIDUE-FOLD SUPPORT layer, split out of
// scctco_fold.hpp (the code-quality continuation, round nine --
// the cdfold/lcf/unroll/fold/unswitch_bif/taildup/mem2reg/rotate/
// scctco_member treatment): the affine resolver (ScctcoAff), the
// fold context (what the former 362-line scctco_fold held in
// locals), and the phase functions (the phi collection, the latch
// split, the def map, the step gate, the region marks, the purity
// gate, the exit scan, the threshold simulation, the emission).
// The slim driver -- the phase list -- stays in scctco_fold.hpp.
//
// ALGORITHM (the phase list; every order below is the original's):
//   phis    H's phis in insertion order are [V_0..V_{n-1}, trip];
//           exactly one V and nparams == 1 (the scalar count-down).
//   latch   preds(H) == 2, root = D (block 0), latch = the other.
//   vals    V0 = V's root-edge arg, Vl = V's latch-edge arg.
//   defs    def_of: dst -> instr index, the affine input.
//   step    Vl = V - c with 0 < c <= 8 and c a power of two
//           (the <= 8 bound keeps the threshold simulation under
//           512*8+8 runs).
//   regions reach: forward flood from H; back: reverse flood from
//           H gated on reach (the loop proper).
//   pure    the loop proper holds only Phi/Const/Copy/Br/Cbr and
//           non-Div/Mod Bins.
//   exits   every Cbr arm of a back block into a non-back, non-
//           trapb block is [consts.., ret const] under cond
//           Eq(affine(V), const); residue r = konst - delta, in
//           range, unique, and exit_val[r] == r for EVERY r in
//           0..c-1.
//   thresh  simulate(n): interpret the pure loop from D (block 0,
//           from = NONE) with val[V0] = n, consts resolved on
//           demand, the trap block NEVER executed (stepping onto
//           trapb = trapped), a 100000-step cap; monotone in n, so
//           binary search over (0, 512*c + 8] finds the exact
//           first-trapping input T (must not trap at n = 0, must
//           deliver mid & (c-1) on every non-trapping probe).
//   emit    five mints z0/KC/g1/g2/gc (all base_of(V0)), the guard
//           block [0; T; V0<0; V0>=T; or; cbr trapb/fold], then
//           the fold block's m1/res (c > 1) or r0 (c == 1) AFTER
//           the guard append, ret V0 & (c-1) (c == 1: ret 0), and
//           D's br H retargeted at the guard.
//
// THE BAIL-PRINT LEDGER (dbg-gated; the message strings are
// observable under --debug, so every one below is the original's
// exact string, in the original's order): "phi shape",
// "nparams != 1", "preds != 2", "root/latch", "V0/Vl", "step",
// "c not pow2", "impure loop body", "exit not const-ret",
// "cond not Eq", "cond not affine", "cond both non-const",
// "residue range", "dup residue", "v_r != r", "sim failed",
// "traps at n=0 (empty budget?)", "sim value mismatch",
// "no trap found", "no trap block". ONE silent bail exists -- the
// exits scan's condition-def lookup miss returns without a print
// (the original's bare `return;` there) -- sf_scan_exits keeps it
// silent. The original's dead `found` flag (assigned, never read)
// is retired here.
//
// CONTRACT (byte-identical differential, the ckpt 23-28 protocol):
// the mint orders above; the arm iteration order {CB.t, CB.f}; the
// block scan ascending; simulate entering at D with from = NONE
// (D's phis resolve against the root edge); nothing between
// sf_threshold and sf_emit mints (nv is recomputed per simulate
// call and provably constant across the whole fold -- no vreg is
// minted until sf_emit).
#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "scctco_impl.hpp" // scctco_retarget (the D edge retarget)

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

// ---- the fold context -------------------------------------------------------

struct SfCtx {
  Fn &f;
  uint32_t H{}, nparams{}, trapb{};
  OptStats &os;
  uint8_t dbg{};
  uint32_t V{NONE}, trip_phi{NONE};  // the carried value + the trip counter
  uint32_t root{NONE}, latch{NONE};  // D (block 0) + the back-edge pred
  uint32_t V0{NONE}, Vl{NONE};       // V's root-edge / latch-edge args
  std::vector<uint32_t> def_of{};    // dst -> instr index
  uint64_t c{0};                     // the count-down step
  std::vector<char> reach{}, back{}; // from H / the loop proper
  uint64_t T{0};                     // the exact first-trapping input
  constexpr SfCtx(Fn &f_, uint32_t H_, uint32_t nparams_, uint32_t trapb_,
                  OptStats &os_, uint8_t dbg_)
      : f(f_), H(H_), nparams(nparams_), trapb(trapb_), os(os_), dbg(dbg_) {}
};

// the bail print (dbg-gated, the driver's old SCCTCO_BAIL); the
// caller follows with `return false'
constexpr bool sf_bail(const SfCtx &C, const char *why) {
  if (C.dbg >= 1)
    std::printf("  [scctco] fold bail: %s\n", why);
  return false;
}

// ---- the gates --------------------------------------------------------------

// collect H's phis: [V_0..V_{n-1}, trip] (insertion order)
constexpr bool sf_collect_phis(Fn &f, SfCtx &C) {
  std::vector<uint32_t> vphis;
  C.trip_phi = NONE;
  for (uint32_t i = f.blocks[C.H].begin; i < f.blocks[C.H].end; ++i) {
    const Instr &P = f.code[i];
    if (P.k != K::Phi)
      break;
    if (i + 1 < f.blocks[C.H].end && f.code[i + 1].k != K::Phi)
      C.trip_phi = P.dst; // the last phi is the trip counter
    else
      vphis.push_back(P.dst);
  }
  if (C.trip_phi == NONE || vphis.size() != 1)
    return sf_bail(C, "phi shape");
  C.V = vphis[0];
  if (C.nparams != 1)
    return sf_bail(C, "nparams != 1");
  return true;
}

// the latch (the pred that is not the entry edge D)
constexpr bool sf_latch_ok(Fn &f, SfCtx &C) {
  if (f.preds[C.H].size() != 2)
    return sf_bail(C, "preds != 2");
  C.root = NONE;
  C.latch = NONE;
  for (uint32_t p : f.preds[C.H]) {
    if (p == 0)
      C.root = p;
    else
      C.latch = p;
  }
  if (C.root != 0 || C.latch == NONE)
    return sf_bail(C, "root/latch");
  return true;
}

// V0 = V's root-edge arg, Vl = V's latch-edge arg
constexpr bool sf_carried_vals(Fn &f, SfCtx &C) {
  C.V0 = NONE;
  C.Vl = NONE;
  for (uint32_t i = f.blocks[C.H].begin; i < f.blocks[C.H].end; ++i) {
    const Instr &P = f.code[i];
    if (P.k != K::Phi)
      break;
    if (P.dst != C.V)
      continue;
    if (P.t == C.root)
      C.V0 = P.a;
    if (P.f == C.root)
      C.V0 = P.b2;
    if (P.t == C.latch)
      C.Vl = P.a;
    if (P.f == C.latch)
      C.Vl = P.b2;
  }
  if (C.V0 == NONE || C.Vl == NONE)
    return sf_bail(C, "V0/Vl");
  return true;
}

// ---- the analyses -----------------------------------------------------------

// the def map (for affine resolution)
constexpr void sf_build_def_of(Fn &f, SfCtx &C) {
  const uint32_t nb = f.nblocks();
  C.def_of.assign(f.nvars + (uint32_t)f.version_base.size(), NONE);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = f.blocks[b].begin; i < f.blocks[b].end; ++i)
      if (f.code[i].dst != NONE && f.code[i].dst < C.def_of.size())
        C.def_of[f.code[i].dst] = i;
}

// step: Vl must be V - c (0 < c <= 8, a power of two)
constexpr bool sf_step_ok(Fn &f, SfCtx &C) {
  ScctcoAff al = scctco_affine(f, C.def_of, C.Vl, C.V);
  if (!al.ok || al.is_const || al.delta >= 0 || al.delta < -8)
    return sf_bail(C, "step");
  C.c = (uint64_t)(-al.delta);
  if ((C.c & (C.c - 1)) != 0)
    return sf_bail(C, "c not pow2");
  return true;
}

// loop region: reachable from H; the loop proper reaches H back
constexpr void sf_mark_regions(Fn &f, SfCtx &C) {
  const uint32_t nb = f.nblocks();
  C.reach.assign(nb, 0);
  C.back.assign(nb, 0);
  {
    std::vector<uint32_t> wl{C.H};
    C.reach[C.H] = 1;
    while (!wl.empty()) {
      uint32_t b = wl.back();
      wl.pop_back();
      for (uint32_t s : {f.succ0(b), f.succ1(b)}) {
        if (s == NONE || s >= nb || C.reach[s])
          continue;
        C.reach[s] = 1;
        wl.push_back(s);
      }
    }
    std::vector<uint32_t> rw{C.H};
    C.back[C.H] = 1;
    while (!rw.empty()) {
      uint32_t b = rw.back();
      rw.pop_back();
      for (uint32_t p : f.preds[b]) {
        if (p >= nb || C.back[p] || !C.reach[p])
          continue;
        C.back[p] = 1;
        rw.push_back(p);
      }
    }
  }
}

// purity of the loop proper
constexpr bool sf_loop_pure(Fn &f, SfCtx &C) {
  const uint32_t nb = f.nblocks();
  for (uint32_t b = 0; b < nb; ++b) {
    if (!C.back[b])
      continue;
    for (uint32_t i = f.blocks[b].begin; i < f.blocks[b].end; ++i) {
      const Instr &I = f.code[i];
      if (I.k == K::Phi || I.k == K::Const || I.k == K::Copy || I.k == K::Br ||
          I.k == K::Cbr)
        continue;
      if (I.k == K::Bin && I.b != B::Div && I.b != B::Mod)
        continue;
      return sf_bail(C, "impure loop body");
    }
  }
  return true;
}

// exits: cbr arms into non-back reachable blocks; every arm must be
// [consts.., ret const] under cond Eq(affine(V), const), residue r =
// konst - delta unique in range, and exit_val[r] == r for EVERY r
constexpr bool sf_scan_exits(Fn &f, SfCtx &C) {
  const uint32_t nb = f.nblocks();
  std::vector<int64_t> exit_val(C.c, -1); // residue r -> returned constant
  for (uint32_t b = 0; b < nb; ++b) {
    if (!C.back[b])
      continue;
    const uint32_t e = f.blocks[b].end;
    if (!e || f.code[e - 1].k != K::Cbr)
      continue;
    const Instr &CB = f.code[e - 1];
    for (uint32_t arm : {CB.t, CB.f}) {
      if (arm == NONE || arm >= nb || C.back[arm])
        continue;
      if (arm == C.trapb)
        continue; // the cap guard, not a semantic exit
      // `arm' must be [consts.., ret const]
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
      if (!pure || rv == NONE || rv >= C.def_of.size() ||
          C.def_of[rv] == NONE || f.code[C.def_of[rv]].k != K::Const)
        return sf_bail(C, "exit not const-ret");
      const int64_t vret = f.code[C.def_of[rv]].c;
      // the condition: Eq(x, y), one side affine in V, other side const
      if (CB.a == NONE || CB.a >= C.def_of.size() || C.def_of[CB.a] == NONE)
        return false; // SILENT (the original's bare `return;')
      const Instr &CM = f.code[C.def_of[CB.a]];
      if (CM.k != K::Bin || CM.b != B::Eq)
        return sf_bail(C, "cond not Eq");
      ScctcoAff x = scctco_affine(f, C.def_of, CM.a, C.V);
      ScctcoAff y = scctco_affine(f, C.def_of, CM.b2, C.V);
      if (!x.ok || !y.ok)
        return sf_bail(C, "cond not affine");
      int64_t r = -1;
      if (!x.is_const && y.is_const)
        r = y.konst - x.delta;
      else if (x.is_const && !y.is_const)
        r = x.konst - y.delta;
      else
        return sf_bail(C, "cond both non-const");
      if (r < 0 || (uint64_t)r >= C.c)
        return sf_bail(C, "residue range");
      if (exit_val[(uint32_t)r] != -1)
        return sf_bail(C, "dup residue");
      exit_val[(uint32_t)r] = vret;
    }
  }
  for (uint64_t r = 0; r < C.c; ++r)
    if (exit_val[(uint32_t)r] != (int64_t)r)
      return sf_bail(C, "v_r != r");
  return true;
}

// ---- the threshold simulation ----------------------------------------------

// interpret the pure loop from D (block 0, from = NONE) with val[V0]
// = n; consts resolve on demand; the trap block is NEVER executed
// (stepping onto trapb = trapped); 100000-step cap
constexpr bool sf_simulate(Fn &f, const SfCtx &C, int64_t n, bool &trapped,
                           int64_t &out) {
  const uint32_t nb = f.nblocks();
  const uint32_t nv = f.nvars + (uint32_t)f.version_base.size();
  std::vector<val_t> val(nv, 0);
  std::vector<char> setv(nv, 0);
  auto read = [&](uint32_t v) -> bool {
    if (v == NONE || v >= nv)
      return false;
    if (!setv[v]) {
      if (v >= C.def_of.size() || C.def_of[v] == NONE)
        return false;
      const Instr &D2 = f.code[C.def_of[v]];
      if (D2.k != K::Const)
        return false; // unknown outer value
      val[v] = D2.c;
      setv[v] = 1;
    }
    return true;
  };
  val[C.V0] = n;
  setv[C.V0] = 1;
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
    if (nxt == C.trapb) {
      trapped = true; // the guard: never execute the trap block
      return true;
    }
    from = cur;
    cur = nxt;
  }
  return false; // diverged past the step cap: should not happen
}

// the EXACT first-trapping input T by binary search (the static
// 512*c guess is wrong for multi-edge cycles: mutual recursion makes
// n+1 calls, not n/2+1 -- each SCC edge is one frame). The value
// shape is already proven statically (v_r == r); trapping is
// monotone in n (edges = E*floor(n/c) + o(n mod c), all offsets <
// E), so ~log2(512*c) runs find T -- which also keeps the constexpr
// (static_assert) build's evaluation state small.
constexpr bool sf_threshold(Fn &f, SfCtx &C) {
  bool tr = false;
  int64_t out = 0;
  if (!sf_simulate(f, C, 0, tr, out))
    return sf_bail(C, "sim failed");
  if (tr)
    return sf_bail(C, "traps at n=0 (empty budget?)");
  if (out != 0)
    return sf_bail(C, "sim value mismatch");
  uint64_t lo = 0, hi = 512 * C.c + 8; // traps somewhere in (lo, hi]
  for (uint32_t guard = 0; guard < 32 && hi - lo > 1; ++guard) {
    uint64_t mid = lo + (hi - lo) / 2;
    if (!sf_simulate(f, C, (int64_t)mid, tr, out))
      return sf_bail(C, "sim failed");
    if (tr)
      hi = mid;
    else {
      if (out != (int64_t)(mid & (C.c - 1)))
        return sf_bail(C, "sim value mismatch");
      lo = mid;
    }
  }
  if (hi - lo != 1)
    return sf_bail(C, "no trap found");
  C.T = hi;
  return true;
}

// ---- the emission -----------------------------------------------------------

// the guard block (V0 < 0 || V0 >= T -> trap, else fold) + the fold
// block (ret V0 & (c-1); c == 1: ret 0) + D's retarget; the stat
constexpr bool sf_emit(Fn &f, SfCtx &C) {
  if (C.trapb == NONE)
    return sf_bail(C, "no trap block");
  const uint32_t base0 = f.base_of(C.V0);
  const uint32_t z0 = new_anon_vreg(f, base0);
  const uint32_t KC = new_anon_vreg(f, base0);
  const uint32_t g1 = new_anon_vreg(f, base0);
  const uint32_t g2 = new_anon_vreg(f, base0);
  const uint32_t gc = new_anon_vreg(f, base0);
  const uint32_t FOLD = f.nblocks();
  { // guard block: V0 < 0 || V0 >= T -> trap, else fold
    std::vector<Instr> gb;
    gb.push_back(Instr{.k = K::Const, .dst = z0, .c = 0});
    gb.push_back(Instr{.k = K::Const, .dst = KC, .c = (int64_t)C.T});
    gb.push_back(
        Instr{.k = K::Bin, .b = B::Lt, .dst = g1, .a = C.V0, .b2 = z0});
    gb.push_back(
        Instr{.k = K::Bin, .b = B::Ge, .dst = g2, .a = C.V0, .b2 = KC});
    gb.push_back(Instr{.k = K::Bin, .b = B::Or, .dst = gc, .a = g1, .b2 = g2});
    gb.push_back(Instr{.k = K::Cbr, .a = gc, .t = C.trapb, .f = FOLD + 1});
    uint32_t base = (uint32_t)f.code.size();
    f.code.insert(f.code.end(), gb.begin(), gb.end());
    f.blocks.push_back({base, (uint32_t)f.code.size()});
    f.succ.push_back(C.trapb);
    f.succ.push_back(FOLD + 1);
  }
  { // fold block: ret V0 & (c-1)   (c == 1: ret 0)
    std::vector<Instr> fb;
    if (C.c > 1) {
      const uint32_t m1 = new_anon_vreg(f, base0);
      const uint32_t res = new_anon_vreg(f, base0);
      fb.push_back(Instr{.k = K::Const, .dst = m1, .c = (int64_t)(C.c - 1)});
      fb.push_back(
          Instr{.k = K::Bin, .b = B::And, .dst = res, .a = C.V0, .b2 = m1});
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
  scctco_retarget(f, 0, C.H, FOLD); // D: br H -> br FOLD
  C.os.scctco_folds++;
  return true;
}

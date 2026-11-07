#pragma once

// ============================================================================
// 9d3. HANDLE ALIAS ANALYSIS (allocation-identity + value ranges)
// ============================================================================
// The arena is one flat int64 array; a handle is the index of an
// allocation's first slot. alias_analyze computes, for every vreg, whether
// it provably holds the handle of EXACTLY ONE allocation site:
//    single[v] = id   for the Alloc dst, Copies of it, and Phis whose live
//                     args are all that same id
//    single[v] = NONE otherwise ("obscure": constants, arithmetic results,
//                     loads, params, input(), call results, mixed phis --
//                     the value may name ANY array, or no array at all).
// Two program-wide gates are derived from this (see prog_handle_gates):
//   lift_ok   -- no Load/Store anywhere reads through an obscure handle.
//                Otherwise a runtime value could observe the stale arena
//                contents of a promoted (mem2reg'd) array, so promotion is
//                disabled globally. (Promotion KEEPS the allocation in the
//                arena, so handle VALUES and lengths never change.)
//   stkarr_ok -- additionally no handle value is observable outside handle
//                position and no Len reads an obscure handle. Moving an
//                array OUT of the arena (the C backend's stack arrays)
//                shifts every LATER handle value; that must be provably
//                unobservable. [This also fixes a soundness bug: printing
//                the second of two arrays used to print a shifted handle
//                in the C backend when the first was stack-allocated.]
struct AliasInfo {
  std::vector<uint32_t> single{}; // vreg -> alloc id, NONE = obscure
  uint32_t nallocs{};
};

constexpr AliasInfo alias_analyze(const Fn &fn) {
  AliasInfo AI{};
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  AI.single.assign(nv, NONE);
  if (!nv) return AI;
  for (const Instr &I : fn.code)
    if (I.k == K::Alloc && I.dst != NONE && I.dst < nv)
      AI.single[I.dst] = AI.nallocs++;
  // Copies and Phis reference each other (loop phis), so iterate to a
  // fixpoint. Each vreg has ONE def, so single[v] is a pure function of its
  // def instr; the loop just resolves the reference chains. Transitions are
  // monotone NONE->id (an id, once assigned, is stable), so this terminates.
  bool grew = true;
  for (uint32_t guard = 0; grew && guard < 64; ++guard) {
    grew = false;
    for (const Instr &I : fn.code) {
      if (I.dst == NONE || I.dst >= nv) continue;
      if (I.k == K::Copy) {
        if (I.a != NONE && I.a < nv &&
            AI.single[I.dst] != AI.single[I.a]) {
          AI.single[I.dst] = AI.single[I.a];
          grew = true;
        }
      } else if (I.k == K::Phi) {
        // single iff BOTH args exist, are that same single, and are not
        // obscure; an undef (NONE) arg is treated as obscure (conservative)
        bool ok = I.a != NONE && I.a < nv && AI.single[I.a] != NONE &&
                  I.b2 != NONE && I.b2 < nv &&
                  AI.single[I.b2] == AI.single[I.a];
        uint32_t want = ok ? AI.single[I.a] : NONE;
        if (AI.single[I.dst] != want) {
          AI.single[I.dst] = want;
          grew = true;
        }
      }
    }
  }
  return AI;
}

// Program-wide gate computation over every function (pre-optimization):
// the optimizer only ever removes or duplicates instructions, and copies
// propagate single-ness, so a gate that holds now holds for the whole run.
constexpr void prog_handle_gates(const std::vector<Fn> &fns, bool &lift_ok,
                                 bool &stkarr_ok) {
  lift_ok = true;
  stkarr_ok = true;
  for (const Fn &fn : fns) {
    if (!fn.nblocks()) continue;
    AliasInfo AI = alias_analyze(fn);
    const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
    auto sgl = [&](uint32_t v) -> bool {
      return v != NONE && v < nv && AI.single[v] != NONE;
    };
    for (uint32_t b = 0; b < fn.nblocks(); ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k == K::Load || I.k == K::Store) {
          if (!sgl(I.a)) lift_ok = false; // could hit ANY array's contents
        } else if (I.k == K::Len) {
          if (!sgl(I.a)) stkarr_ok = false; // length table would shift
        }
        if (!stkarr_ok) continue;
        // observable uses of a handle VALUE (numbers that shift when an
        // earlier allocation leaves the arena)
        switch (I.k) {
        case K::Bin:   // arithmetic/comparison on a handle
        case K::Cbr:   // branching on a handle value
        case K::Print: // printing a handle
        case K::Ret:   // returning a handle to the caller
        case K::Alloc: // a handle as an allocation size
          if (sgl(I.a) || sgl(I.b2)) stkarr_ok = false;
          break;
        case K::Load:  // handle used as an INDEX
          if (sgl(I.b2)) stkarr_ok = false;
          break;
        case K::Store: // handle used as an index or stored as a value
          if (sgl(I.b2) || sgl(I.c2)) stkarr_ok = false;
          break;
        case K::Call:  // handle passed to a function
          for (uint32_t v : I.args)
            if (sgl(v)) stkarr_ok = false;
          break;
        default: break; // Copy/Phi: plumbing; Len handle position: fine
        }
      }
  }
}

// The array arena: identical bump-allocator semantics in EVERY backend
// (interpreters, bytecode VM, emitted C, emitted x86-64). A handle is the
// index of the array's first element; allocation is monotonic (no free), so
// handle values, aliasing, and allocation order are bit-identical everywhere.
// CAP must match MJZ_ARENA_CAP emitted into the C/asm backends.
inline constexpr val_t MJZ_ARENA_CAP = 2097152; // 2^21 int64 slots = 16 MiB

// ============================================================================
// 9d3b. VALUE RANGE ANALYSIS (signed intervals + branch refinement)
// ============================================================================
// Per-block dense interval states over SSA vregs; every vreg has exactly one
// def, so a vreg's "value" is a single runtime integer and its interval is a
// property of the reaching paths, refined per block entry:
//   * interval transfer for +,-,* (saturating; any overflow -> top)
//   * sign-aware Div/Mod transfers: x>=0,y>=1 => x%y in [0, min(x.hi,y.hi-1)]
//     (this is what keeps Euclid's loop provably non-negative)
//   * branch refinement on each out-edge of a Cbr whose condition is a
//     comparison: on the taken edge the comparison HOLDS as a fact about the
//     operand values (SSA: the same values, not recomputations), so the
//     operands' intervals can be tightened -- `x != 0` chops 0 off an
//     endpoint, `x < y` caps x.hi, `x == c` pins both sides.
//   * plain iteration first, growing intervals UP from concrete seeds
//     (loop counters start as [0,0] and widen only when they keep growing:
//     `while (i < 1000)` converges to i in [0,999] at the body entry);
//     then widening at blocks that still grow; then narrowing rounds.
//     If no fixpoint is reached at all, every state falls back to TOP
//     (sound over-approximation, no proofs). The result is always a sound
//     over-approximation of the reachable values.
struct Iv {
  val_t lo{INT64_MIN}, hi{INT64_MAX};
  bool bot{}; // empty interval: unreachable program point
};
constexpr bool iv_eq(const Iv &a, const Iv &b) {
  return a.bot == b.bot && a.lo == b.lo && a.hi == b.hi;
}
constexpr Iv iv_hull(const Iv &a, const Iv &b) { // join (may-analysis)
  if (a.bot) return b;
  if (b.bot) return a;
  return {a.lo < b.lo ? a.lo : b.lo, a.hi > b.hi ? a.hi : b.hi, false};
}
constexpr uint64_t u64_abs(val_t v) { // |v| saturating at 2^63
  uint64_t u = (uint64_t)v;
  return (u >> 63) ? (uint64_t)0 - u : u;
}
// one interval per operand of a Bin; all results are sound supersets
constexpr Iv iv_bin(B b, const Iv &x, const Iv &y) {
  if (x.bot || y.bot) return {0, 0, true};
  switch (b) {
  case B::Add: {
    __int128 lo = (__int128)x.lo + y.lo, hi = (__int128)x.hi + y.hi;
    if (lo < (__int128)INT64_MIN || hi > (__int128)INT64_MAX)
      return {INT64_MIN, INT64_MAX, false}; // wraps: anything is possible
    return {(val_t)lo, (val_t)hi, false};
  }
  case B::Sub: {
    __int128 lo = (__int128)x.lo - y.hi, hi = (__int128)x.hi - y.lo;
    if (lo < (__int128)INT64_MIN || hi > (__int128)INT64_MAX)
      return {INT64_MIN, INT64_MAX, false};
    return {(val_t)lo, (val_t)hi, false};
  }
  case B::Mul: {
    __int128 c[4] = {(__int128)x.lo * y.lo, (__int128)x.lo * y.hi,
                     (__int128)x.hi * y.lo, (__int128)x.hi * y.hi};
    __int128 lo = c[0], hi = c[0];
    for (int k = 1; k < 4; ++k) {
      if (c[k] < lo) lo = c[k];
      if (c[k] > hi) hi = c[k];
    }
    if (lo < (__int128)INT64_MIN || hi > (__int128)INT64_MAX)
      return {INT64_MIN, INT64_MAX, false};
    return {(val_t)lo, (val_t)hi, false};
  }
  case B::Div: {
    if (!(y.lo >= 1 || y.hi <= -1)) return {INT64_MIN, INT64_MAX, false};
    // single-sign divisor: the extrema of x/y are at the four corners
    val_t cl = INT64_MAX, ch = INT64_MIN; // careful with init directions
    cl = INT64_MIN; ch = INT64_MAX;
    val_t lo = INT64_MAX, hi = INT64_MIN;
    for (int k = 0; k < 4; ++k) {
      val_t a = (k & 1) ? x.hi : x.lo, d = (k & 2) ? y.hi : y.lo;
      Rv r = w_div(a, d); // corner may trap numerically: then top
      if (r.trap) return {INT64_MIN, INT64_MAX, false};
      if (r.v < lo) lo = r.v;
      if (r.v > hi) hi = r.v;
    }
    (void)cl;
    (void)ch;
    return {lo, hi, false};
  }
  case B::Mod: {
    if (!(y.lo >= 1 || y.hi <= -1)) return {INT64_MIN, INT64_MAX, false};
    // |r| < |y| and sign(r) == sign(x); also |r| <= |x|
    val_t M{};
    if (y.lo >= 1) {
      M = (y.hi == INT64_MAX) ? INT64_MAX : (y.hi - 1);
    } else {
      uint64_t m = u64_abs(y.lo) > u64_abs(y.hi) ? u64_abs(y.lo)
                                                 : u64_abs(y.hi);
      M = (m >= (uint64_t)INT64_MAX) ? INT64_MAX : (val_t)(m - 1);
    }
    if (x.lo >= 0) return {0, x.hi < M ? x.hi : M, false};
    if (x.hi <= 0) return {x.lo < -M ? -M : x.lo, 0, false};
    return {x.lo < -M ? -M : x.lo, x.hi < M ? x.hi : M, false};
  }
  case B::Lt: case B::Le: case B::Gt: case B::Ge: case B::Eq: case B::Ne:
    return {0, 1, false}; // comparisons yield 0/1
  default:
    return {INT64_MIN, INT64_MAX, false}; // bit ops / shifts / mulhi: top
  }
}
// tighten (x,y) given that `x cmp y` is known to be `taken`
constexpr void iv_refine(Iv &x, Iv &y, B cmp, bool taken) {
  if (x.bot || y.bot) return;
  if (cmp == B::Gt || cmp == B::Ge) { // normalize to the Lt/Le direction
    Iv t = x;
    x = y;
    y = t;
    cmp = (cmp == B::Gt) ? B::Lt : B::Le;
  }
  auto exclude = [](Iv &i, val_t c) { // remove the point c if it is an endpoint
    if (i.lo == c && i.hi == c) { i.bot = true; return; }
    if (i.lo == c && c != INT64_MAX) i.lo = c + 1;
    else if (i.hi == c && c != INT64_MIN) i.hi = c - 1;
  };
  if (cmp == B::Lt) {
    if (taken) {
      if (y.hi != INT64_MIN && x.hi > y.hi - 1) x.hi = y.hi - 1;
      if (x.lo != INT64_MAX && y.lo < x.lo + 1) y.lo = x.lo + 1;
    } else { // x >= y
      if (x.lo < y.lo) x.lo = y.lo;
      if (y.hi > x.hi) y.hi = x.hi;
    }
  } else if (cmp == B::Le) {
    if (taken) {
      if (x.hi > y.hi) x.hi = y.hi;
      if (y.lo < x.lo) y.lo = x.lo;
    } else { // x > y
      if (y.lo != INT64_MAX && x.lo < y.lo + 1) x.lo = y.lo + 1;
      if (x.hi != INT64_MIN && y.hi > x.hi - 1) y.hi = x.hi - 1;
    }
  } else if (cmp == B::Eq) {
    if (taken) {
      val_t lo = x.lo > y.lo ? x.lo : y.lo;
      val_t hi = x.hi < y.hi ? x.hi : y.hi;
      if (lo > hi) { x.bot = y.bot = true; } // contradictory: dead edge
      else { x = {lo, hi, false}; y = {lo, hi, false}; }
    } else {
      if (y.lo == y.hi) exclude(x, y.lo);
      else if (x.lo == x.hi) exclude(y, x.lo);
    }
  } else { // B::Ne
    if (taken) {
      if (y.lo == y.hi) exclude(x, y.lo);
      else if (x.lo == x.hi) exclude(y, x.lo);
    } else {
      val_t lo = x.lo > y.lo ? x.lo : y.lo;
      val_t hi = x.hi < y.hi ? x.hi : y.hi;
      if (lo > hi) { x.bot = y.bot = true; }
      else { x = {lo, hi, false}; y = {lo, hi, false}; }
    }
  }
}

struct Ranges {
  bool ok{};
  uint32_t nb{}, nv{};
  std::vector<Iv> entry{}; // per block: nv intervals at block entry
};

// Apply instr i's definition to state st (phis are pre-substituted by the
// caller via the block-entry phi rule; they are skipped here).
constexpr void iv_transfer(const Fn &fn, uint32_t i, uint32_t nv,
                           std::vector<Iv> &st) {
  const Instr &I = fn.code[i];
  if (I.dst == NONE || I.dst >= nv) return;
  switch (I.k) {
  case K::Const:
    st[I.dst] = {I.c, I.c, false};
    return;
  case K::Copy:
    if (I.a != NONE && I.a < nv) st[I.dst] = st[I.a];
    else st[I.dst] = {INT64_MIN, INT64_MAX, false};
    return;
  case K::Bin:
    st[I.dst] = iv_bin(I.b, st[I.a], st[I.b2]);
    return;
  case K::Phi:
    return; // handled by the block-entry rule
  case K::Alloc:
    st[I.dst] = {INT64_MIN, INT64_MAX, false}; // handle value: unknown
    return;
  case K::Len:
    st[I.dst] = {0, MJZ_ARENA_CAP, false};
    return;
  default:
    st[I.dst] = {INT64_MIN, INT64_MAX, false}; // input/call/load: top
    return;
  }
}

constexpr Ranges range_analyze(const Fn &fn, const An &A) {
  Ranges RA{};
  RA.nb = fn.nblocks();
  RA.nv = fn.nvars + (uint32_t)fn.version_base.size();
  RA.ok = RA.nb && RA.nv &&
          (uint64_t)RA.nb * RA.nv <= 4u * 1024u * 1024u; // memory cap
  if (!RA.ok) return RA;
  const uint32_t nb = RA.nb, nv = RA.nv;
  // comparison def behind each Cbr (same predicate the emission plan uses
  // for branch fusion: single-use, single-def comparison)
  std::vector<uint32_t> raw(nv, 0), ndefs(nv, 0), defi(nv, NONE);
  for (uint32_t i = 0; i < fn.code.size(); ++i) {
    const Instr &I = fn.code[i];
    if (I.a != NONE && I.a < nv) raw[I.a]++;
    if (I.b2 != NONE && I.b2 < nv) raw[I.b2]++;
    if (I.c2 != NONE && I.c2 < nv) raw[I.c2]++;
    if (I.dst != NONE && I.dst < nv) {
      ++ndefs[I.dst];
      defi[I.dst] = i;
    }
  }
  auto is_cmp_def = [&](uint32_t c) -> uint32_t {
    if (c == NONE || c >= nv) return NONE;
    if (raw[c] != 1 || ndefs[c] != 1) return NONE;
    uint32_t d = defi[c];
    if (d == NONE) return NONE;
    const Instr &D = fn.code[d];
    if (D.k != K::Bin) return NONE;
    if (D.b == B::Lt || D.b == B::Le || D.b == B::Gt || D.b == B::Ge ||
        D.b == B::Eq || D.b == B::Ne)
      return d;
    return NONE;
  };
  std::vector<Iv> entry((size_t)nb * nv, Iv{0, 0, true});  // bottom: grow up
  std::vector<Iv> exit_((size_t)nb * nv, Iv{0, 0, true});
  std::vector<char> touched(nb, 0);
  std::vector<Iv> st(nv), edge(nv);
  // Patience before header-widening kicks in, and after it. Both capped:
  // phi args read the UNREFINED pred exits, so a loop-carried counter that
  // creeps +-1 per round (e.g. the scctco frame guard, or any i = i-1
  // chain not bounded by an entry-side hull) converges only via widening,
  // and a non-converging (oscillating) CFG runs to the give-up bound --
  // on a big inlined caller, patience x (4*nb+16) x nb*nv was minutes.
  // Giving up falls back to sound TOP (no nt proofs), so the caps only
  // trade precision for time on pathological shapes.
  const uint32_t plain = (4 * nb + 16) > 64 ? 64 : (4 * nb + 16);
  const uint32_t widened_cap = (4 * nb + 16) > 64 ? 64 : (4 * nb + 16);
  bool widening = false;
  uint32_t round = 0;
  bool converged = false;
  for (;;) {
    bool changed = false;
    for (uint32_t b = 0; b < nb; ++b) {
      // ---- entry = hull over incoming (refined) pred exits + phi rule
      for (uint32_t v = 0; v < nv; ++v) st[v] = {0, 0, true};
      bool any_pred = false;
      for (uint32_t p : fn.preds[b]) {
        if (p >= nb || !touched[p]) continue;
        any_pred = true;
        // refined exit of p along the edge p -> b
        for (uint32_t v = 0; v < nv; ++v) edge[v] = exit_[(size_t)p * nv + v];
        const Instr &term = fn.code[fn.blocks[p].end - 1];
        if (term.k == K::Cbr) {
          uint32_t d = is_cmp_def(term.a);
          if (d != NONE) {
            const Instr &D = fn.code[d];
            Iv x = (D.a != NONE && D.a < nv) ? edge[D.a] : Iv{INT64_MIN, INT64_MAX, false};
            Iv y = (D.b2 != NONE && D.b2 < nv) ? edge[D.b2] : Iv{INT64_MIN, INT64_MAX, false};
            bool taken = (term.t == b);
            iv_refine(x, y, D.b, taken);
            if (D.a != NONE && D.a < nv) edge[D.a] = x;
            if (D.b2 != NONE && D.b2 < nv) edge[D.b2] = y;
          }
        }
        for (uint32_t v = 0; v < nv; ++v) st[v] = iv_hull(st[v], edge[v]);
      }
      if (!any_pred && b != 0) { // unreachable: keep bottom
        for (uint32_t v = 0; v < nv; ++v) st[v] = {0, 0, true};
      }
      if (b == 0) { // entry: everything top (params included)
        for (uint32_t v = 0; v < nv; ++v) st[v] = {INT64_MIN, INT64_MAX, false};
      } else {
        // phi rule: the phi's value is one of its args, per incoming edge
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
          const Instr &I = fn.code[i];
          if (I.k != K::Phi) break;
          Iv acc{0, 0, true};
          for (uint32_t p : fn.preds[b]) {
            if (p >= nb || !touched[p]) continue;
            uint32_t arg = NONE;
            if (I.t == p && I.a != NONE) arg = I.a;
            else if (I.f == p && I.b2 != NONE) arg = I.b2;
            if (arg != NONE && arg < nv)
              acc = iv_hull(acc, exit_[(size_t)p * nv + arg]);
          }
          st[I.dst] = acc;
        }
      }
      // ---- convergence bookkeeping
      {
        bool diff = false;
        for (uint32_t v = 0; v < nv; ++v)
          if (!iv_eq(st[v], entry[(size_t)b * nv + v])) { diff = true; break; }
        if (diff) {
          // WIDEN AT LOOP HEADERS ONLY (Cousot discipline). Widening at
          // every block re-inflates refined body states (e.g. a body entry
          // [0,999] "grew" from [0,31] only because the header widened),
          // and the re-widened [0,MAX] feeds i+1 -> overflow -> TOP into
          // the phi, poisoning the lo side to MIN forever (+1 per round,
          // never converging). Non-header blocks keep the joined value and
          // descend freely.
          if (widening && b < A.n && A.inner[b] == b) { // widen growing sides
            for (uint32_t v = 0; v < nv; ++v) {
              Iv &old = entry[(size_t)b * nv + v];
              if (st[v].bot) continue;
              if (st[v].lo < old.lo) st[v].lo = INT64_MIN;
              if (st[v].hi > old.hi) st[v].hi = INT64_MAX;
              if (st[v].lo > st[v].hi) st[v] = old; // paranoia
            }
          }
          for (uint32_t v = 0; v < nv; ++v)
            entry[(size_t)b * nv + v] = st[v];
          changed = true;
        }
      }
      // ---- transfer through the block
      for (uint32_t v = 0; v < nv; ++v) st[v] = entry[(size_t)b * nv + v];
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
        iv_transfer(fn, i, nv, st);
      for (uint32_t v = 0; v < nv; ++v) exit_[(size_t)b * nv + v] = st[v];
      touched[b] = 1;
    }
    if (!changed) {
      converged = true;
      break;
    }
    ++round;
    if (!widening && round >= plain) widening = true;
    if (widening && round >= plain + widened_cap) break; // give up (below)
  }
  if (!converged) {
    // no fixpoint (pathological CFG): every claim would rest on an
    // under-approximation, so fall back to the trivially sound TOP
    entry.assign((size_t)nb * nv, Iv{INT64_MIN, INT64_MAX, false});
    RA.entry = std::move(entry);
    return RA;
  }
  // ---- narrowing: re-run transfers and intersect with the widened state
  for (uint32_t n = 0; n < 3; ++n) {
    for (uint32_t b = 0; b < nb; ++b) {
      for (uint32_t v = 0; v < nv; ++v) st[v] = {0, 0, true};
      bool any_pred = false;
      for (uint32_t p : fn.preds[b]) {
        if (p >= nb || !touched[p]) continue;
        any_pred = true;
        for (uint32_t v = 0; v < nv; ++v) edge[v] = exit_[(size_t)p * nv + v];
        const Instr &term = fn.code[fn.blocks[p].end - 1];
        if (term.k == K::Cbr) {
          uint32_t d = is_cmp_def(term.a);
          if (d != NONE) {
            const Instr &D = fn.code[d];
            Iv x = (D.a != NONE && D.a < nv) ? edge[D.a] : Iv{INT64_MIN, INT64_MAX, false};
            Iv y = (D.b2 != NONE && D.b2 < nv) ? edge[D.b2] : Iv{INT64_MIN, INT64_MAX, false};
            iv_refine(x, y, D.b, term.t == b);
            if (D.a != NONE && D.a < nv) edge[D.a] = x;
            if (D.b2 != NONE && D.b2 < nv) edge[D.b2] = y;
          }
        }
        for (uint32_t v = 0; v < nv; ++v) st[v] = iv_hull(st[v], edge[v]);
      }
      (void)any_pred;
      if (b == 0)
        for (uint32_t v = 0; v < nv; ++v) st[v] = {INT64_MIN, INT64_MAX, false};
      else
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
          const Instr &I = fn.code[i];
          if (I.k != K::Phi) break;
          Iv acc{0, 0, true};
          for (uint32_t p : fn.preds[b]) {
            if (p >= nb || !touched[p]) continue;
            uint32_t arg = NONE;
            if (I.t == p && I.a != NONE) arg = I.a;
            else if (I.f == p && I.b2 != NONE) arg = I.b2;
            if (arg != NONE && arg < nv)
              acc = iv_hull(acc, exit_[(size_t)p * nv + arg]);
          }
          st[I.dst] = acc;
        }
      // intersect with the previous (sound) approximation
      for (uint32_t v = 0; v < nv; ++v) {
        Iv &old = entry[(size_t)b * nv + v];
        if (st[v].bot || old.bot) continue;
        val_t lo = st[v].lo > old.lo ? st[v].lo : old.lo;
        val_t hi = st[v].hi < old.hi ? st[v].hi : old.hi;
        if (lo <= hi) old = {lo, hi, false};
      }
      for (uint32_t v = 0; v < nv; ++v) st[v] = entry[(size_t)b * nv + v];
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
        iv_transfer(fn, i, nv, st);
      for (uint32_t v = 0; v < nv; ++v) exit_[(size_t)b * nv + v] = st[v];
    }
  }
  RA.entry = std::move(entry);
  return RA;
}

// --- 9d3c. trap elimination ----------------------------------------------------
// A Div/Mod whose divisor provably excludes 0 (and excludes -1, or whose
// dividend provably excludes INT64_MIN) can never trap: mark it `nt` so
// every backend drops the guard, DCE/LICM may treat it as pure, and the
// closed-form/simulation loop passes may delete the loop computing it.
// A Load/Store whose handle provably names ONE constant-length allocation
// with a constant in-range index can never trap either.
constexpr bool opt_trapelim(Fn &fn, const An &A, OptStats &os) {
  Ranges RA = range_analyze(fn, A);
  if (!RA.ok) return false;
  AliasInfo AI = alias_analyze(fn);
  const uint32_t nb = fn.nblocks();
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nv) return false;
  std::vector<uint32_t> defi(nv, NONE);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
      if (fn.code[i].dst != NONE && fn.code[i].dst < nv)
        defi[fn.code[i].dst] = i;
  // allocation lengths (constant ones only)
  std::vector<val_t> alen(AI.nallocs, val_t(-1));
  for (const Instr &I : fn.code)
    if (I.k == K::Alloc && I.a != NONE && I.a < nv && defi[I.a] != NONE &&
        fn.code[defi[I.a]].k == K::Const && I.dst != NONE && I.dst < nv &&
        AI.single[I.dst] != NONE && fn.code[defi[I.a]].c >= 0)
      alen[AI.single[I.dst]] = fn.code[defi[I.a]].c;
  auto nz = [](const Iv &i) { return i.bot || i.hi < 0 || i.lo > 0; };
  auto nm1 = [](const Iv &i) { return i.bot || i.hi < -1 || i.lo > -1; };
  auto nmin = [](const Iv &i) { return i.bot || i.lo > INT64_MIN; };
  bool changed = false;
  std::vector<Iv> st(nv);
  for (uint32_t b = 0; b < nb; ++b) {
    for (uint32_t v = 0; v < nv; ++v) st[v] = RA.entry[(size_t)b * nv + v];
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k == K::Phi) continue; // entry rule already covered phis
      if (I.k == K::Bin && (I.b == B::Div || I.b == B::Mod) && !I.nt) {
        Iv dv = (I.b2 != NONE && I.b2 < nv) ? st[I.b2] : Iv{};
        Iv dd = (I.a != NONE && I.a < nv) ? st[I.a] : Iv{};
        if (nz(dv) && (nm1(dv) || nmin(dd))) {
          I.nt = true;
          os.traps_killed++;
          changed = true;
        }
      } else if ((I.k == K::Load || I.k == K::Store) && !I.nt) {
        // provably one constant-length allocation + provably in-range index
        // (a constant index, or the range analysis's interval for a runtime
        // index, must fit inside [0, len))
        if (I.a != NONE && I.a < nv && AI.single[I.a] != NONE &&
            alen[AI.single[I.a]] >= 0 && I.b2 != NONE && I.b2 < nv) {
          val_t L = alen[AI.single[I.a]];
          bool safe = false;
          if (defi[I.b2] != NONE && fn.code[defi[I.b2]].k == K::Const) {
            val_t idx = fn.code[defi[I.b2]].c;
            safe = idx >= 0 && idx < L;
          } else {
            const Iv &ix = st[I.b2]; // range at this program point
            safe = !ix.bot && ix.lo >= 0 && ix.hi < L;
          }
          if (safe) {
            I.nt = true;
            os.traps_killed++;
            changed = true;
          }
        }
      }
      iv_transfer(fn, i, nv, st);
    }
  }
  return changed;
}

// ============================================================================
// 9d4. mem2reg: array memory -> SSA registers ("memory to register lifting")
// ============================================================================
// An `array(L)` with constant L whose every access is through a handle that
// provably names ONLY that allocation, with a constant in-range index, does
// not need memory at all: slot j of the array becomes a pseudo-variable,
//     init   : L x `Const 0` right after the Alloc (arena arrays are
//              zero-filled; the Alloc itself is KEPT so the arena bump,
//              handle values, lengths and OOM traps stay bit-identical)
//     store  : a versioned def (a Copy) of the slot's pseudo-variable
//     load   : a use of the currently reaching version
// with phis placed at the iterated dominance frontier of each slot's def
// blocks and renaming by dominator-tree walk -- full SSA, so every later
// pass (constprop, CSE, LICM, closed forms...) sees plain registers.
// An array that is never READ (no Load at all) is dead regardless of index
// shape: all its Stores are deleted ("if the array is not read we just
// delete writes, cuz why not") and Len(h) folds to the constant L.
constexpr bool opt_mem2reg(Fn &fn, const An &A, OptStats &os, bool lift_ok,
                           bool delalloc_ok) {
  const uint32_t nb = fn.nblocks();
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nb || !nv) return false;
  AliasInfo AI = alias_analyze(fn);
  if (!AI.nallocs) return false;
  // ---- dead allocations --------------------------------------------------
  // With every Load/Store/Len of an array gone (lifted here, dead-store
  // deleted here, or never present at all) nothing reads the allocation
  // anymore. Deleting it shifts every LATER handle, which is observable
  // only through handle VALUES (print/compare/pass/stored) or a Len through
  // an obscure handle -- exactly what the program-wide stkarr_ok gate
  // excludes -- so under that gate an allocation whose handle chain is
  // referenced by nothing but its own Copy/Phi plumbing is pure dead weight
  // and dies here (DCE then cleans the plumbing). This is what removes the
  // `int64_t mjz_sN[len]; memset(...)` pair for arrays that folding has
  // already turned into constants.
  auto sweep_dead_allocs = [&]() -> bool {
    if (!delalloc_ok) return false;
    std::vector<char> used(AI.nallocs, 0);
    for (uint32_t b = 0; b < fn.nblocks(); ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        uint32_t dst_id =
            (I.dst != NONE && I.dst < nv) ? AI.single[I.dst] : NONE;
        auto use = [&](uint32_t v) {
          if (v == NONE || v >= nv) return;
          uint32_t id = AI.single[v];
          if (id != NONE && id != dst_id) used[id] = 1;
        };
        use(I.a);
        use(I.b2);
        use(I.c2);
        for (uint32_t v : I.args) use(v);
      }
    bool any = false;
    for (uint32_t id = 0; id < AI.nallocs; ++id) any |= used[id] == 0;
    if (!any) return false;
    std::vector<Instr> nc{};
    nc.reserve(fn.code.size());
    for (uint32_t b = 0; b < fn.nblocks(); ++b) {
      uint32_t old_begin = fn.blocks[b].begin;
      fn.blocks[b].begin = (uint32_t)nc.size();
      for (uint32_t i = old_begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k == K::Alloc && I.dst != NONE && I.dst < nv) {
          uint32_t id = AI.single[I.dst];
          if (id != NONE && !used[id]) {
            os.deadallocs++;
            continue;
          }
        }
        nc.push_back(I);
      }
      fn.blocks[b].end = (uint32_t)nc.size();
    }
    fn.code = std::move(nc);
    return true;
  };
  if (!lift_ok) return sweep_dead_allocs();
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
  constexpr val_t MJZ_MEM2REG_MAX_ELEMS = 256;
  constexpr uint32_t MJZ_MEM2REG_MAX_SLOTS = 2048;
  // ---- classify allocations -----------------------------------------------
  // liftable: constant length, every access single-handle + const index
  //           in range (loads exist)          -> slot machinery
  // dead:     constant length, NO loads, and every store provably unable to
  //           trap (constant in-range index, or `nt` from the range
  //           analysis) -> drop the stores; Len -> L; keep the Alloc (an
  //           earlier arena slot must not shift)
  struct Cls {
    uint32_t id{};
    val_t len{};
    bool lift{};
    bool dead{};
  };
  std::vector<Cls> cls(AI.nallocs, {0, -1, false, false});
  {
    std::vector<char> has_load(AI.nallocs, 0), has_store(AI.nallocs, 0),
        bad(AI.nallocs, 0), bad_dead(AI.nallocs, 0);
    std::vector<uint32_t> alloc_instr(AI.nallocs, NONE);
    for (uint32_t b = 0; b < nb; ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        uint32_t id = (I.a != NONE && I.a < nv) ? AI.single[I.a] : NONE;
        if (I.k == K::Alloc && I.dst != NONE && I.dst < nv &&
            AI.single[I.dst] != NONE) {
          if (id == NONE) id = AI.single[I.dst];
          if (I.a != NONE && I.a < nv && kc[I.a] && kv[I.a] >= 0) {
            cls[id].len = kv[I.a];
            alloc_instr[id] = i;
          } else {
            cls[id].len = -1; // runtime length: not classifiable
          }
          continue;
        }
        if (id == NONE) continue;
        if (I.k == K::Load || I.k == K::Store) {
          bool idx_ok = I.b2 != NONE && I.b2 < nv && kc[I.b2] &&
                        kv[I.b2] >= 0 && kv[I.b2] < cls[id].len;
          if (!idx_ok) bad[id] = 1; // slot machinery needs compile-time elems
          if (I.k == K::Load) {
            has_load[id] = 1;
          } else {
            has_store[id] = 1;
            if (!idx_ok && !I.nt)
              bad_dead[id] = 1; // a may-trap store must stay (trap parity)
          }
        }
      }
    uint32_t budget = MJZ_MEM2REG_MAX_SLOTS;
    for (uint32_t id = 0; id < AI.nallocs; ++id) {
      if (cls[id].len < 0 || alloc_instr[id] == NONE) continue;
      if (has_load[id] && !bad[id] && cls[id].len <= MJZ_MEM2REG_MAX_ELEMS &&
          budget >= (uint32_t)cls[id].len) {
        cls[id].lift = true;
        budget -= (uint32_t)cls[id].len;
      } else if (!has_load[id] && !bad_dead[id]) {
        cls[id].dead = true; // never read: stores are pure dead weight
      }
    }
  }
  bool any_lift = false, any_dead = false;
  for (const Cls &c : cls) {
    any_lift |= c.lift;
    any_dead |= c.dead;
  }
  if (!any_lift && !any_dead) return sweep_dead_allocs();
  // ---- defensive pre-check (before ANY mutation): every access of a ------
  // lifted array must be dominated by the allocation's block, and ordered
  // after the allocation when both share a block. SSA's "def dominates use"
  // makes this a theorem for the handle's uses; verify it anyway so the
  // renaming walk below can never encounter an empty slot stack.
  {
    std::vector<uint32_t> alloc_idx(AI.nallocs, NONE),
        alloc_blk(AI.nallocs, NONE);
    for (uint32_t b = 0; b < nb; ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k == K::Alloc && I.dst != NONE && I.dst < nv &&
            AI.single[I.dst] != NONE) {
          alloc_idx[AI.single[I.dst]] = i;
          alloc_blk[AI.single[I.dst]] = b;
        }
      }
    for (uint32_t b = 0; b < nb; ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k != K::Load && I.k != K::Store) continue;
        uint32_t h = (I.a != NONE && I.a < nv) ? AI.single[I.a] : NONE;
        if (h == NONE || !cls[h].lift) continue;
        uint32_t ab = alloc_blk[h];
        if (ab == NONE || !A.dominates(ab, b) ||
            (ab == b && i < alloc_idx[h]))
          cls[h].lift = false;
      }
    any_lift = false;
    for (const Cls &c : cls) any_lift |= c.lift;
    if (!any_lift && !any_dead) return sweep_dead_allocs();
  }
  // ---- lifted arrays: slot bookkeeping --------------------------------------
  uint32_t total_slots = 0;
  for (const Cls &c : cls)
    if (c.lift) total_slots += (uint32_t)c.len;
  // slot id = slot_base[id] + element index
  std::vector<uint32_t> slot_base(AI.nallocs, NONE);
  {
    uint32_t next = 0;
    for (uint32_t id = 0; id < AI.nallocs; ++id)
      if (cls[id].lift) {
        slot_base[id] = next;
        next += (uint32_t)cls[id].len;
      }
  }
  // def blocks per slot: the Alloc block (init) + each Store's block
  std::vector<std::vector<ulen>> sdefs(total_slots);
  {
    std::vector<uint32_t> alloc_block(AI.nallocs, NONE);
    for (uint32_t b = 0; b < nb; ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k == K::Alloc && I.dst < nv && AI.single[I.dst] != NONE)
          alloc_block[AI.single[I.dst]] = b;
      }
    for (uint32_t id = 0; id < AI.nallocs; ++id)
      if (cls[id].lift)
        for (val_t j = 0; j < cls[id].len; ++j)
          sdefs[slot_base[id] + (uint32_t)j].push_back(alloc_block[id]);
    for (uint32_t b = 0; b < nb; ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k == K::Store && I.a < nv && AI.single[I.a] != NONE &&
            cls[AI.single[I.a]].lift)
          sdefs[slot_base[AI.single[I.a]] + (uint32_t)kv[I.b2]].push_back(b);
      }
  }
  // ---- phi placement at each slot's IDF -------------------------------------
  std::vector<std::vector<uint32_t>> phis_at(nb);
  for (uint32_t s = 0; s < total_slots; ++s) {
    if (sdefs[s].size() < 2) continue;
    std::vector<ulen> idf =
        gns::calculate_iterated_dominance_frontiers_given_dominance_frontiers<
            V>(A.df, sdefs[s]);
    for (ulen blk : idf) {
      uint32_t b = (uint32_t)blk;
      if (b >= nb || fn.preds[b].size() < 2) continue;
      bool dup = false;
      for (uint32_t x : phis_at[b]) dup |= (x == s);
      if (!dup) phis_at[b].push_back(s);
    }
  }
  // ---- rebuild: slot phis at block starts, init consts after Allocs,
  //      drop dead-array stores -----------------------------------------------
  std::vector<uint32_t> init_vreg(total_slots, NONE);
  std::vector<char> drop(fn.code.size(), 0);
  {
    std::vector<Instr> nc{};
    nc.reserve(fn.code.size() + total_slots + 16);
    for (uint32_t b = 0; b < nb; ++b) {
      uint32_t old_begin = fn.blocks[b].begin;
      fn.blocks[b].begin = (uint32_t)nc.size();
      for (uint32_t s : phis_at[b]) // slot phis go FIRST (before var phis)
        nc.push_back({.k = K::Phi, .c = (int64_t)s});
      for (uint32_t i = old_begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k == K::Store && I.a < nv && AI.single[I.a] != NONE &&
            cls[AI.single[I.a]].dead) {
          drop[i] = 1;
          os.mem2reg_dses++;
          continue;
        }
        nc.push_back(I);
        if (I.k == K::Alloc && I.dst < nv && AI.single[I.dst] != NONE &&
            cls[AI.single[I.dst]].lift) {
          uint32_t base = slot_base[AI.single[I.dst]];
          for (val_t j = 0; j < cls[AI.single[I.dst]].len; ++j) {
            uint32_t v = new_anon_vreg(fn, 0);
            init_vreg[base + (uint32_t)j] = v;
            nc.push_back({.k = K::Const, .dst = v, .c = 0});
          }
          os.mem2reg_lifts++;
        }
      }
      fn.blocks[b].end = (uint32_t)nc.size();
    }
    fn.code = std::move(nc);
  }
  // ---- renaming: dominator-tree walk with per-slot version stacks ----------
  // (SSA guarantees every access of a lifted array is dominated by the
  // allocation's block -- the handle's def dominates its uses -- so the
  // init version is always on the stack when an access is rewritten.)
  // init_vreg -> slot lookup. NOTE: the init vregs were created by the
  // rebuild (new_anon_vreg), so their ids are >= nv; the map is sized to
  // cover them. The init is pushed onto the version stack WHEN THE WALK
  // REACHES IT (at the allocation site), NOT at function entry -- an array
  // allocated inside a loop is re-zeroed every iteration, and reads before
  // the allocation never happen (the dominance pre-check guarantees it).
  // (Entry-pushing instead lets a loop-carried slot phi SHADOW the init and
  // leak the previous iteration's values -- the stale-loop-array bug.)
  std::vector<uint32_t> init_slot_of(nv + total_slots, NONE);
  for (uint32_t s = 0; s < total_slots; ++s)
    if (init_vreg[s] != NONE && init_vreg[s] < nv + total_slots)
      init_slot_of[init_vreg[s]] = s;
  {
    std::vector<std::vector<uint32_t>> stacks(total_slots);
    struct Frame {
      uint32_t block, child_idx;
      std::vector<uint32_t> pushed; // slots this block pushed (pop on exit)
    };
    std::vector<Frame> wstack;
    // visit: rewrite this block's accesses; seed slot-phi args in successors
    auto visit = [&](uint32_t b, std::vector<uint32_t> &pushed) {
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        Instr &I = fn.code[i];
        if (I.k == K::Phi) {
          if (I.dst == NONE) { // slot-phi placeholder: assign its result
            uint32_t s = (uint32_t)I.c;
            I.dst = new_anon_vreg(fn, 0);
            stacks[s].push_back(I.dst);
            pushed.push_back(s);
          }
          continue; // variable phis: args are plain vreg references
        }
        if (I.k == K::Const && I.dst != NONE &&
            I.dst < init_slot_of.size() && init_slot_of[I.dst] != NONE) {
          // the promotion's zero-init
          uint32_t s2 = init_slot_of[I.dst];
          stacks[s2].push_back(I.dst);
          pushed.push_back(s2);
          continue;
        }
        uint32_t h = (I.a != NONE && I.a < nv) ? AI.single[I.a] : NONE;
        if (I.k == K::Load && h != NONE && cls[h].lift) {
          uint32_t s = slot_base[h] + (uint32_t)kv[I.b2];
          if (stacks[s].empty()) return false; // cannot happen (dominance)
          I.k = K::Copy;
          I.a = stacks[s].back();
          I.b2 = NONE;
        } else if (I.k == K::Store && h != NONE && cls[h].lift) {
          uint32_t s = slot_base[h] + (uint32_t)kv[I.b2];
          uint32_t v = new_anon_vreg(fn, 0);
          I.k = K::Copy;
          I.dst = v;
          I.a = I.c2; // the stored value
          I.b2 = NONE;
          I.c2 = NONE;
          stacks[s].push_back(v);
          pushed.push_back(s);
        } else if (I.k == K::Len && h != NONE &&
                   (cls[h].lift || cls[h].dead)) {
          I.k = K::Const;
          I.c = cls[h].len;
          I.a = NONE;
        }
      }
      // seed slot-phi args in successors: the value at the END of this
      // block reaches the successor along this edge. Slot phis are exactly
      // the first phis_at[s].size() phi instrs of s (insertion order).
      for (uint32_t s2 : {fn.succ0(b), fn.succ1(b)}) {
        if (s2 == NONE || s2 >= nb) continue;
        uint32_t k = 0;
        for (uint32_t i = fn.blocks[s2].begin; i < fn.blocks[s2].end; ++i) {
          Instr &I = fn.code[i];
          if (I.k != K::Phi) break;
          if (k < phis_at[s2].size()) {
            uint32_t s = phis_at[s2][k];
            uint32_t slot = NONE; // b's position among s2's preds
            for (uint32_t j = 0; j < fn.preds[s2].size(); ++j)
              if (fn.preds[s2][j] == b) slot = j;
            if (slot == NONE) return false;
            uint32_t arg = stacks[s].empty() ? NONE : stacks[s].back();
            if (slot == 0) { I.a = arg; I.t = b; }
            else           { I.b2 = arg; I.f = b; }
          }
          ++k;
        }
      }
      return true;
    };
    std::vector<uint32_t> pu{};
    if (!visit(0, pu)) return false; // defensive: refuse (nothing mutated
                                     // by visit at that point on failure)
    wstack.push_back({0, 0, std::move(pu)});
    while (!wstack.empty()) {
      Frame &fr = wstack.back();
      auto ch = A.dom_tree.range()[fr.block];
      if (fr.child_idx < (uint32_t)ch.size()) {
        uint32_t next = (uint32_t)ch[fr.child_idx++];
        std::vector<uint32_t> pu2{};
        visit(next, pu2);
        wstack.push_back({next, 0, std::move(pu2)});
      } else {
        for (auto it = fr.pushed.rbegin(); it != fr.pushed.rend(); ++it)
          stacks[*it].pop_back();
        wstack.pop_back();
      }
    }
  }
  (void)sweep_dead_allocs(); // post-rewrite: lifted handles are now unused
  return true;
}

#pragma once
// === intervals.hpp ========================================
// signed-interval value range analysis with branch refinement.

#include "../../../minijz/ir/instr.hpp"
#include "../../../minijz/opt/analysis/alias.hpp"
#include "../../../minijz/opt/analysis/an.hpp"

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
  if (a.bot)
    return b;
  if (b.bot)
    return a;
  return {a.lo < b.lo ? a.lo : b.lo, a.hi > b.hi ? a.hi : b.hi, false};
}
constexpr uint64_t u64_abs(val_t v) { // |v| saturating at 2^63
  uint64_t u = (uint64_t)v;
  return (u >> 63) ? (uint64_t)0 - u : u;
}
// one interval per operand of a Bin; all results are sound supersets
constexpr Iv iv_bin(B b, const Iv &x, const Iv &y) {
  if (x.bot || y.bot)
    return {0, 0, true};
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
      if (c[k] < lo)
        lo = c[k];
      if (c[k] > hi)
        hi = c[k];
    }
    if (lo < (__int128)INT64_MIN || hi > (__int128)INT64_MAX)
      return {INT64_MIN, INT64_MAX, false};
    return {(val_t)lo, (val_t)hi, false};
  }
  case B::Div: {
    if (!(y.lo >= 1 || y.hi <= -1))
      return {INT64_MIN, INT64_MAX, false};
    // single-sign divisor: the extrema of x/y are at the four corners
    val_t cl = INT64_MAX, ch = INT64_MIN; // careful with init directions
    cl = INT64_MIN;
    ch = INT64_MAX;
    val_t lo = INT64_MAX, hi = INT64_MIN;
    for (int k = 0; k < 4; ++k) {
      val_t a = (k & 1) ? x.hi : x.lo, d = (k & 2) ? y.hi : y.lo;
      Rv r = w_div(a, d); // corner may trap numerically: then top
      if (r.trap)
        return {INT64_MIN, INT64_MAX, false};
      if (r.v < lo)
        lo = r.v;
      if (r.v > hi)
        hi = r.v;
    }
    (void)cl;
    (void)ch;
    return {lo, hi, false};
  }
  case B::Mod: {
    if (!(y.lo >= 1 || y.hi <= -1))
      return {INT64_MIN, INT64_MAX, false};
    // |r| < |y| and sign(r) == sign(x); also |r| <= |x|
    val_t M{};
    if (y.lo >= 1) {
      M = (y.hi == INT64_MAX) ? INT64_MAX : (y.hi - 1);
    } else {
      uint64_t m =
          u64_abs(y.lo) > u64_abs(y.hi) ? u64_abs(y.lo) : u64_abs(y.hi);
      M = (m >= (uint64_t)INT64_MAX) ? INT64_MAX : (val_t)(m - 1);
    }
    if (x.lo >= 0)
      return {0, x.hi < M ? x.hi : M, false};
    if (x.hi <= 0)
      return {x.lo < -M ? -M : x.lo, 0, false};
    return {x.lo < -M ? -M : x.lo, x.hi < M ? x.hi : M, false};
  }
  case B::Lt:
  case B::Le:
  case B::Gt:
  case B::Ge:
  case B::Eq:
  case B::Ne:
    return {0, 1, false}; // comparisons yield 0/1
  case B::And:
    // x & 1 in {0,1} whatever x is (the parity bit of the countdown fold
    // and of divmagic's %2 lowerings); everything else stays top
    if (!x.bot && !y.bot && y.lo == y.hi && y.lo == 1)
      return {0, 1, false};
    return {INT64_MIN, INT64_MAX, false};
  default:
    return {INT64_MIN, INT64_MAX, false}; // bit ops / shifts / mulhi: top
  }
}
// tighten (x,y) given that `x cmp y` is known to be `taken`
constexpr void iv_refine(Iv &x, Iv &y, B cmp, bool taken) {
  if (x.bot || y.bot)
    return;
  bool swapped = false;
  if (cmp == B::Gt || cmp == B::Ge) { // normalize to the Lt/Le direction
    Iv t = x;
    x = y;
    y = t;
    cmp = (cmp == B::Gt) ? B::Lt : B::Le;
    swapped = true; // refine in the mirrored space, restore at the end:
  }                 // x/y are CALLER-BINDINGS (x <-> D.a), so the swap must
    // not leak out -- the old leak exchanged the operands'
    // intervals on every Gt/Ge edge, e.g. `while (c > 0)`
    // pinned c to [0,0] in the body (fuzz cfg seed 4412).
  auto exclude = [](Iv &i, val_t c) { // remove the point c if it is an endpoint
    if (i.lo == c && i.hi == c) {
      i.bot = true;
      return;
    }
    if (i.lo == c && c != INT64_MAX)
      i.lo = c + 1;
    else if (i.hi == c && c != INT64_MIN)
      i.hi = c - 1;
  };
  if (cmp == B::Lt) {
    if (taken) {
      if (y.hi != INT64_MIN && x.hi > y.hi - 1)
        x.hi = y.hi - 1;
      if (x.lo != INT64_MAX && y.lo < x.lo + 1)
        y.lo = x.lo + 1;
    } else { // x >= y
      if (x.lo < y.lo)
        x.lo = y.lo;
      if (y.hi > x.hi)
        y.hi = x.hi;
    }
  } else if (cmp == B::Le) {
    if (taken) {
      if (x.hi > y.hi)
        x.hi = y.hi;
      if (y.lo < x.lo)
        y.lo = x.lo;
    } else { // x > y
      if (y.lo != INT64_MAX && x.lo < y.lo + 1)
        x.lo = y.lo + 1;
      if (x.hi != INT64_MIN && y.hi > x.hi - 1)
        y.hi = x.hi - 1;
    }
  } else if (cmp == B::Eq) {
    if (taken) {
      val_t lo = x.lo > y.lo ? x.lo : y.lo;
      val_t hi = x.hi < y.hi ? x.hi : y.hi;
      if (lo > hi) {
        x.bot = y.bot = true;
      } // contradictory: dead edge
      else {
        x = {lo, hi, false};
        y = {lo, hi, false};
      }
    } else {
      if (y.lo == y.hi)
        exclude(x, y.lo);
      else if (x.lo == x.hi)
        exclude(y, x.lo);
    }
  } else { // B::Ne
    if (taken) {
      if (y.lo == y.hi)
        exclude(x, y.lo);
      else if (x.lo == x.hi)
        exclude(y, x.lo);
    } else {
      val_t lo = x.lo > y.lo ? x.lo : y.lo;
      val_t hi = x.hi < y.hi ? x.hi : y.hi;
      if (lo > hi) {
        x.bot = y.bot = true;
      } else {
        x = {lo, hi, false};
        y = {lo, hi, false};
      }
    }
  }
  // contradictory endpoint crossings (possible when a fact contradicts an
  // endpoint, e.g. x=[5,5] refined by `x<3`): the empty interval, not a
  // garbage [5,2] that hull() would smear across the state
  if (!x.bot && x.lo > x.hi)
    x.bot = true;
  if (!y.bot && y.lo > y.hi)
    y.bot = true;
  if (swapped) { // restore the caller's bindings
    Iv t = x;
    x = y;
    y = t;
  }
}

// decide a comparison from intervals alone: writes 0/1 into `out` and
// returns true when the outcome is provable (both singletons: the exact
// evaluator; provably disjoint/ordered intervals: the six directions).
// Shared by trapelim's hull-based compare folding and jthread's
// edge-sensitive branch resolution.
constexpr bool iv_decide(B b, const Iv &x, const Iv &y, val_t &out) {
  if (x.bot || y.bot)
    return false;
  if (x.lo == x.hi && y.lo == y.hi) { // both singletons: exact
    Rv r = w_bin(b, x.lo, y.lo);
    if (r.trap)
      return false; // unreachable for comparisons; be safe
    out = r.v;
    return true;
  }
  if (x.hi < y.lo || x.lo > y.hi) { // provably disjoint / ordered
    switch (b) {
    case B::Lt:
    case B::Le:
      out = (x.hi < y.lo) ? 1 : 0;
      return true;
    case B::Gt:
    case B::Ge:
      out = (x.hi < y.lo) ? 0 : 1;
      return true;
    case B::Eq:
      out = 0;
      return true;
    case B::Ne:
      out = 1;
      return true;
    default:
      return false;
    }
  }
  return false;
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
  if (I.dst == NONE || I.dst >= nv)
    return;
  switch (I.k) {
  case K::Const:
    st[I.dst] = {I.c, I.c, false};
    return;
  case K::Copy:
    if (I.a != NONE && I.a < nv)
      st[I.dst] = st[I.a];
    else
      st[I.dst] = {INT64_MIN, INT64_MAX, false};
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
  if (!RA.ok)
    return RA;
  const uint32_t nb = RA.nb, nv = RA.nv;
  // comparison def behind each Cbr (same predicate the emission plan uses
  // for branch fusion: single-use, single-def comparison)
  std::vector<uint32_t> raw(nv, 0), ndefs(nv, 0), defi(nv, NONE);
  for (uint32_t i = 0; i < fn.code.size(); ++i) {
    const Instr &I = fn.code[i];
    if (I.a != NONE && I.a < nv)
      raw[I.a]++;
    if (I.b2 != NONE && I.b2 < nv)
      raw[I.b2]++;
    if (I.c2 != NONE && I.c2 < nv)
      raw[I.c2]++;
    if (I.dst != NONE && I.dst < nv) {
      ++ndefs[I.dst];
      defi[I.dst] = i;
    }
  }
  auto is_cmp_def = [&](uint32_t c) -> uint32_t {
    if (c == NONE || c >= nv)
      return NONE;
    if (raw[c] != 1 || ndefs[c] != 1)
      return NONE;
    uint32_t d = defi[c];
    if (d == NONE)
      return NONE;
    const Instr &D = fn.code[d];
    if (D.k != K::Bin)
      return NONE;
    if (D.b == B::Lt || D.b == B::Le || D.b == B::Gt || D.b == B::Ge ||
        D.b == B::Eq || D.b == B::Ne)
      return d;
    return NONE;
  };
  std::vector<Iv> entry((size_t)nb * nv, Iv{0, 0, true}); // bottom: grow up
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
      for (uint32_t v = 0; v < nv; ++v)
        st[v] = {0, 0, true};
      bool any_pred = false;
      for (uint32_t p : fn.preds[b]) {
        if (p >= nb || !touched[p])
          continue;
        any_pred = true;
        // refined exit of p along the edge p -> b
        for (uint32_t v = 0; v < nv; ++v)
          edge[v] = exit_[(size_t)p * nv + v];
        const Instr &term = fn.code[fn.blocks[p].end - 1];
        if (term.k == K::Cbr) {
          uint32_t d = is_cmp_def(term.a);
          if (d != NONE) {
            const Instr &D = fn.code[d];
            Iv x = (D.a != NONE && D.a < nv) ? edge[D.a]
                                             : Iv{INT64_MIN, INT64_MAX, false};
            Iv y = (D.b2 != NONE && D.b2 < nv)
                       ? edge[D.b2]
                       : Iv{INT64_MIN, INT64_MAX, false};
            bool taken = (term.t == b);
            iv_refine(x, y, D.b, taken);
            if (D.a != NONE && D.a < nv)
              edge[D.a] = x;
            if (D.b2 != NONE && D.b2 < nv)
              edge[D.b2] = y;
          }
        }
        for (uint32_t v = 0; v < nv; ++v)
          st[v] = iv_hull(st[v], edge[v]);
      }
      if (!any_pred && b != 0) { // unreachable: keep bottom
        for (uint32_t v = 0; v < nv; ++v)
          st[v] = {0, 0, true};
      }
      if (b == 0) { // entry: everything top (params included)
        for (uint32_t v = 0; v < nv; ++v)
          st[v] = {INT64_MIN, INT64_MAX, false};
      } else {
        // phi rule: the phi's value is one of its args, per incoming edge
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
          const Instr &I = fn.code[i];
          if (I.k != K::Phi)
            break;
          Iv acc{0, 0, true};
          for (uint32_t p : fn.preds[b]) {
            if (p >= nb || !touched[p])
              continue;
            uint32_t arg = NONE;
            if (I.t == p && I.a != NONE)
              arg = I.a;
            else if (I.f == p && I.b2 != NONE)
              arg = I.b2;
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
          if (!iv_eq(st[v], entry[(size_t)b * nv + v])) {
            diff = true;
            break;
          }
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
              if (st[v].bot)
                continue;
              if (st[v].lo < old.lo)
                st[v].lo = INT64_MIN;
              if (st[v].hi > old.hi)
                st[v].hi = INT64_MAX;
              if (st[v].lo > st[v].hi)
                st[v] = old; // paranoia
            }
          }
          for (uint32_t v = 0; v < nv; ++v)
            entry[(size_t)b * nv + v] = st[v];
          changed = true;
        }
      }
      // ---- transfer through the block
      for (uint32_t v = 0; v < nv; ++v)
        st[v] = entry[(size_t)b * nv + v];
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
        iv_transfer(fn, i, nv, st);
      for (uint32_t v = 0; v < nv; ++v)
        exit_[(size_t)b * nv + v] = st[v];
      touched[b] = 1;
    }
    if (!changed) {
      converged = true;
      break;
    }
    ++round;
    if (!widening && round >= plain)
      widening = true;
    if (widening && round >= plain + widened_cap)
      break; // give up (below)
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
      for (uint32_t v = 0; v < nv; ++v)
        st[v] = {0, 0, true};
      bool any_pred = false;
      for (uint32_t p : fn.preds[b]) {
        if (p >= nb || !touched[p])
          continue;
        any_pred = true;
        for (uint32_t v = 0; v < nv; ++v)
          edge[v] = exit_[(size_t)p * nv + v];
        const Instr &term = fn.code[fn.blocks[p].end - 1];
        if (term.k == K::Cbr) {
          uint32_t d = is_cmp_def(term.a);
          if (d != NONE) {
            const Instr &D = fn.code[d];
            Iv x = (D.a != NONE && D.a < nv) ? edge[D.a]
                                             : Iv{INT64_MIN, INT64_MAX, false};
            Iv y = (D.b2 != NONE && D.b2 < nv)
                       ? edge[D.b2]
                       : Iv{INT64_MIN, INT64_MAX, false};
            iv_refine(x, y, D.b, term.t == b);
            if (D.a != NONE && D.a < nv)
              edge[D.a] = x;
            if (D.b2 != NONE && D.b2 < nv)
              edge[D.b2] = y;
          }
        }
        for (uint32_t v = 0; v < nv; ++v)
          st[v] = iv_hull(st[v], edge[v]);
      }
      (void)any_pred;
      if (b == 0)
        for (uint32_t v = 0; v < nv; ++v)
          st[v] = {INT64_MIN, INT64_MAX, false};
      else
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
          const Instr &I = fn.code[i];
          if (I.k != K::Phi)
            break;
          Iv acc{0, 0, true};
          for (uint32_t p : fn.preds[b]) {
            if (p >= nb || !touched[p])
              continue;
            uint32_t arg = NONE;
            if (I.t == p && I.a != NONE)
              arg = I.a;
            else if (I.f == p && I.b2 != NONE)
              arg = I.b2;
            if (arg != NONE && arg < nv)
              acc = iv_hull(acc, exit_[(size_t)p * nv + arg]);
          }
          st[I.dst] = acc;
        }
      // intersect with the previous (sound) approximation
      for (uint32_t v = 0; v < nv; ++v) {
        Iv &old = entry[(size_t)b * nv + v];
        if (st[v].bot || old.bot)
          continue;
        val_t lo = st[v].lo > old.lo ? st[v].lo : old.lo;
        val_t hi = st[v].hi < old.hi ? st[v].hi : old.hi;
        if (lo <= hi)
          old = {lo, hi, false};
      }
      for (uint32_t v = 0; v < nv; ++v)
        st[v] = entry[(size_t)b * nv + v];
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
        iv_transfer(fn, i, nv, st);
      for (uint32_t v = 0; v < nv; ++v)
        exit_[(size_t)b * nv + v] = st[v];
    }
  }
  RA.entry = std::move(entry);
  return RA;
}

// --- 9d3c. trap elimination
// ----------------------------------------------------

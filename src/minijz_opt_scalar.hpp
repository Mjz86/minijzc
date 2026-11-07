// ============================================================================
// MiniJZ compiler — §9 scalar optimizer passes, alias + range analysis, mem2reg
// ----------------------------------------------------------------------------
// Ordered-include header: this file is a slice of the original single
// translation unit and relies on being included after its predecessors
// (see src/minijz.hpp for the canonical order). Do not include it
// directly in an arbitrary order.
// ============================================================================
#pragma once

// ============================================================================
// 9. optimizer (all on SSA; analyses from the lib are recomputed on demand)
// ============================================================================
// fresh anonymous vreg (used by destruction temps, unroll copies, inlining)
constexpr uint32_t new_anon_vreg(Fn &fn, uint32_t base) {
  uint32_t v = fn.nvars + (uint32_t)fn.version_base.size();
  fn.version_base.push_back(base);
  fn.version_no.push_back(0x10000 + (uint32_t)fn.version_base.size());
  return v;
}

struct OptStats {
  uint32_t folds{};      // instrs turned into Const
  uint32_t copies{};     // instrs turned into Copy
  uint32_t brs_folded{}; // Cbr turned into Br
  uint32_t blocks_gone{};// unreachable blocks removed
  uint32_t dce{};        // dead instrs removed
  uint32_t hoisted{};    // LICM hoists
  uint32_t copyprops{};  // copies propagated away (uses rewritten to source)
  uint32_t cses{};       // recomputations replaced by dominating defs
  uint32_t splices{};    // single-pred/succ block pairs merged
  uint32_t cbr_same{};   // Cbr with identical arms turned into Br
  uint32_t phi_degens{}; // phis reduced to Copy/Const (dead pred / single pred)
  uint32_t canon_swaps{}; // commutative ops put in canonical operand order
  uint32_t canon_cmps{};  // >/>= comparisons rewritten to the </<= direction
  uint32_t inlines{};    // call sites inlined (callee body spliced into caller)
  uint32_t unrolls{};    // loops fully unrolled (constant trip count)
  uint32_t iters{};      // fixpoint iterations of the pass pipeline
  uint32_t reassocs{};   // +/-/*c chains reassociated to canonical forms
  uint32_t reassoc_gone{}; // instructions removed by reassociation
  uint32_t lcf_loops{};  // loops replaced by closed-form series evaluation
  uint32_t lcf_sims{};   // loops replaced by compile-time simulation
  uint32_t lcf_rt{};     // loops replaced by RUNTIME-trip-count closed forms
  uint32_t divmagics{};  // div/mod-by-const -> mulhi/shift/bitops lowerings
  uint32_t notcombs{};   // !(cmp) / (cmp)==0 / (cmp)!=1 compare negations folded
  uint32_t traps_killed{}; // div/mod/load/store traps PROVEN impossible (nt)
  uint32_t mem2reg_lifts{}; // arrays lifted to SSA vregs (registers)
  uint32_t mem2reg_dses{};  // stores deleted for never-read (dead) arrays
  uint32_t lcf_gensims{};  // loops folded by the general (any-condition) simulator
  uint32_t constcalls{};  // pure constant-argument calls evaluated at compile time
  uint32_t deadallocs{};  // unused Allocs deleted (arrays read by no one)
  uint32_t tces{};        // tail-recursion eliminated (recursion -> loops)
  uint32_t tce_merges{};  // pure-tail SCCs merged into one sel-dispatched fn
  uint32_t cdfolds{};     // countdown loops replaced by exact closed forms
  uint32_t ifconvs{};     // cbrs with provably-binary conds if-converted
};

// --- 9a. constant folding / propagation / branch folding / identities -------
// known: vreg -> const value. iterates to fixpoint.
constexpr bool opt_constprop(Fn &fn, OptStats &os, bool is_main = false) {
  bool changed_any = false;
  for (int round = 0; round < 12; ++round) {
    bool changed = false;
    std::vector<char> known;
    std::vector<val_t> kval;
    auto is_known = [&](uint32_t v, val_t &out) -> bool {
      if (v == NONE) return false;
      if (v < known.size() && known[v]) { out = kval[v]; return true; }
      return false;
    };
    // collect consts
    known.assign(fn.nvars + fn.version_base.size(), 0);
    kval.assign(fn.nvars + fn.version_base.size(), 0);
    for (const Instr &I : fn.code)
      if (I.k == K::Const) { known[I.dst] = 1; kval[I.dst] = I.c; }
    for (uint32_t b = 0; b < fn.nblocks(); ++b) {
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        Instr &I = fn.code[i];
        val_t av{}, bv{};
        bool ka = is_known(I.a, av);
        bool kb = is_known(I.b2, bv);
        switch (I.k) {
        case K::Const: break;
        case K::Copy:
          if (ka) { I.k = K::Const; I.c = av; os.folds++; changed = true; }
          break;
        case K::Bin: {
          // algebraic identities (never introduce traps: the operand-defining
          // instrs still execute; we only recompute THIS instr's value)
          if (I.b == B::Add || I.b == B::Sub) {
            if (kb && bv == 0) { // x+0 = x ; x-0 = x
              I.k = K::Copy; os.copies++; changed = true; break;
            }
            if (ka && av == 0 && I.b == B::Add) { // 0+x = x
              I.k = K::Copy; I.a = I.b2; os.copies++; changed = true; break;
            }
          }
          if (I.b == B::Mul) {
            if ((ka && av == 0) || (kb && bv == 0)) {
              I.k = K::Const; I.c = 0; os.folds++; changed = true; break;
            }
            if (ka && av == 1) { I.k = K::Copy; I.a = I.b2; os.copies++; changed = true; break; }
            if (kb && bv == 1) { I.k = K::Copy; os.copies++; changed = true; break; }
          }
          if (I.b == B::Div || I.b == B::Mod) {
            if (kb && bv == 1) { // x/1 = x ; x%1 = 0 (divisor known: no trap)
              if (I.b == B::Div) { I.k = K::Copy; os.copies++; }
              else { I.k = K::Const; I.c = 0; os.folds++; }
              changed = true; break;
            }
          }
          if (I.b == B::And) { // x & 0 = 0 ; x & -1 = x
            if ((ka && av == 0) || (kb && bv == 0)) {
              I.k = K::Const; I.c = 0; os.folds++; changed = true; break;
            }
            if (ka && av == -1) { I.k = K::Copy; I.a = I.b2; os.copies++; changed = true; break; }
            if (kb && bv == -1) { I.k = K::Copy; os.copies++; changed = true; break; }
          }
          if (I.b == B::Or) { // x | 0 = x ; x | -1 = -1
            if (ka && av == -1) { I.k = K::Const; I.c = -1; os.folds++; changed = true; break; }
            if (kb && bv == -1) { I.k = K::Const; I.c = -1; os.folds++; changed = true; break; }
            if (ka && av == 0) { I.k = K::Copy; I.a = I.b2; os.copies++; changed = true; break; }
            if (kb && bv == 0) { I.k = K::Copy; os.copies++; changed = true; break; }
          }
          if (I.b == B::Xor) { // x ^ 0 = x (x ^ x handled below)
            if (ka && av == 0) { I.k = K::Copy; I.a = I.b2; os.copies++; changed = true; break; }
            if (kb && bv == 0) { I.k = K::Copy; os.copies++; changed = true; break; }
          }
          if (I.b == B::Shl || I.b == B::LShr || I.b == B::AShr) {
            if (kb && (bv & 63) == 0) { // shift by 0: identity
              I.k = K::Copy; os.copies++; changed = true; break;
            }
          }
          if (I.b == B::MulHi) {
            if ((ka && av == 0) || (kb && bv == 0)) {
              I.k = K::Const; I.c = 0; os.folds++; changed = true; break;
            }
          }
          if (I.a != NONE && I.a == I.b2) { // same vreg both sides (no traps)
            int which = -1; // -1 keep, 0 -> Const 0, 1 -> Const 1
            switch (I.b) {
            case B::Sub: case B::Lt: case B::Gt: which = 0; break;
            case B::Eq: case B::Le: case B::Ge: which = 1; break;
            case B::Xor: which = 0; break; // x ^ x = 0
            default: break; // Add/Mul/Div/Mod/Ne handled elsewhere or unsafe
            }
            if (I.b == B::Ne) which = 0; // x != x is always false
            if (which >= 0) {
              I.k = K::Const; I.c = which;
              os.folds++; changed = true; break;
            }
          }
          if (ka && kb) {
            Rv r{};
            switch (I.b) {
            case B::Add: r = w_add(av, bv); break;
            case B::Sub: r = w_sub(av, bv); break;
            case B::Mul: r = w_mul(av, bv); break;
            case B::Div: r = w_div(av, bv); break;
            case B::Mod: r = w_mod(av, bv); break;
            case B::Lt: r = {av < bv, false}; break;
            case B::Le: r = {av <= bv, false}; break;
            case B::Gt: r = {av > bv, false}; break;
            case B::Ge: r = {av >= bv, false}; break;
            case B::Eq: r = {av == bv, false}; break;
            case B::Ne: r = {av != bv, false}; break;
            case B::And: r = w_and(av, bv); break;
            case B::Or: r = w_or(av, bv); break;
            case B::Xor: r = w_xor(av, bv); break;
            case B::Shl: r = w_shl(av, bv); break;
            case B::LShr: r = w_lshr(av, bv); break;
            case B::AShr: r = w_ashr(av, bv); break;
            case B::MulHi: r = w_mulhi(av, bv); break;
            }
            if (!r.trap) { // trapping folds stay unfolded: runtime trap preserved
              I.k = K::Const; I.c = r.v; os.folds++; changed = true;
            }
          }
          break;
        }
        case K::Phi: {
          // all args the same const -> const ; all args identical -> copy
          uint32_t x = I.a, y = I.b2;
          if (x != NONE && y != NONE) {
            if (x == y) { // both same vreg: copy
              I.k = K::Copy; I.a = x; os.copies++; changed = true; break;
            }
            val_t xv{}, yv{};
            if (is_known(x, xv) && is_known(y, yv) && xv == yv) {
              I.k = K::Const; I.c = xv; os.folds++; changed = true; break;
            }
          }
          break;
        }
        case K::Depth: { // main's frame count is provably 1 everywhere
          if (is_main) {
            I.k = K::Const;
            I.c = 1;
            os.folds++;
            changed = true;
          }
          break;
        }
        case K::Cbr: {
          if (I.t == I.f && I.t != NONE) { // both arms identical: unconditional
            I.k = K::Br;
            I.f = NONE;
            fn.succ[2 * b + 1] = NONE;
            os.cbr_same++;
            changed = true;
            break;
          }
          if (ka) { // fold branch
            I.k = K::Br;
            I.t = (av != 0) ? I.t : I.f;
            I.f = NONE;
            fn.succ[2 * b] = I.t;
            fn.succ[2 * b + 1] = NONE;
            os.brs_folded++; changed = true;
          }
          break;
        }
        default: break;
        }
      }
    }
    if (!changed) break;
    changed_any = true;
  }
  return changed_any;
}

// --- 9a2. copy propagation ----------------------------------------------------
// `d = s` (Copy): every use of d becomes s and the copy dies. SSA gives each
// vreg exactly one def, and the copy's source dominates the copy, hence the
// source dominates every replaced use — always sound. This is the pass that
// cleans up phi-to-copy degenerations and lets CSE see canonical names.
constexpr bool opt_copyprop(Fn &fn, OptStats &os) {
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  std::vector<uint32_t> ren(nv, NONE);
  auto resolve = [&](uint32_t v) -> uint32_t {
    while (v != NONE && v < nv && ren[v] != NONE) v = ren[v];
    return v;
  };
  bool had = false;
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k == K::Copy && I.dst != NONE && I.a != NONE && I.a != I.dst) {
        ren[I.dst] = resolve(I.a); // sources resolve transitively: chains die
        had = true;
      }
    }
  if (!had) return false;
  bool changed = false;
  std::vector<char> drop(fn.code.size(), 0);
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k == K::Copy && I.dst != NONE &&
          (I.a == I.dst || ren[I.dst] != NONE)) { // self copy or propagated
        drop[i] = 1;
        if (I.a != I.dst) os.copyprops++;
        changed = true;
        continue;
      }
      if (I.a != NONE) {
        uint32_t r = resolve(I.a);
        changed |= (r != I.a);
        I.a = r;
      }
      if (I.b2 != NONE) {
        uint32_t r = resolve(I.b2);
        changed |= (r != I.b2);
        I.b2 = r;
      }
      if (I.c2 != NONE) { // Store: the stored value
        uint32_t r = resolve(I.c2);
        changed |= (r != I.c2);
        I.c2 = r;
      }
      for (uint32_t &av : I.args) { // Call arguments
        uint32_t r = resolve(av);
        changed |= (r != av);
        av = r;
      }
    }
  if (changed) { // sweep dropped copies, preserving block ranges
    std::vector<Instr> nc{};
    nc.reserve(fn.code.size());
    for (uint32_t b = 0; b < fn.nblocks(); ++b) {
      uint32_t ob = fn.blocks[b].begin, oe = fn.blocks[b].end;
      fn.blocks[b].begin = (uint32_t)nc.size();
      for (uint32_t i = ob; i < oe; ++i)
        if (!drop[i]) nc.push_back(fn.code[i]);
      fn.blocks[b].end = (uint32_t)nc.size();
    }
    fn.code = std::move(nc);
  }
  return changed;
}

// --- 9a4. canonicalization ----------------------------------------------------
// Puts every pure Bin into ONE canonical shape so equivalent computations
// become identical (which is what makes CSE hit) and identities match:
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
    if (v == NONE) return 0;
    if (v < nv && kc[v]) return ((uint64_t)1 << 63) ^ (uint64_t)kv[v];
    return (uint64_t)v;
  };
  bool changed = false;
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k != K::Bin) continue;
      if (I.b == B::Gt || I.b == B::Ge) { // canonical comparison direction
        std::swap(I.a, I.b2);
        I.b = (I.b == B::Gt) ? B::Lt : B::Le;
        os.canon_cmps++;
        changed = true;
      } else if (I.b == B::Add || I.b == B::Mul || I.b == B::Eq ||
                 I.b == B::Ne || I.b == B::And || I.b == B::Or ||
                 I.b == B::Xor || I.b == B::MulHi) { // commutative canonical order
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
        if (v != NONE && v < nv) ++uses[v];
      };
      bump(I.a);
      bump(I.b2);
      bump(I.c2);
      for (uint32_t v : I.args) bump(v);
      if (I.dst != NONE && I.dst < nv) {
        ++ndefs[I.dst];
        defi[I.dst] = i;
      }
    }
    auto inv = [](B b) -> B {
      switch (b) {
      case B::Lt: return B::Ge;
      case B::Le: return B::Gt;
      case B::Gt: return B::Lt;
      case B::Ge: return B::Le;
      case B::Eq: return B::Ne;
      case B::Ne: return B::Eq;
      default: return b;
      }
    };
    for (uint32_t b = 0; b < fn.nblocks(); ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        Instr &I = fn.code[i];
        if (I.k != K::Bin || (I.b != B::Eq && I.b != B::Ne)) continue;
        if (I.b2 == NONE || I.b2 >= nv || !kc[I.b2]) continue; // const right
        val_t cv = kv[I.b2];
        if (cv != 0 && cv != 1) continue;
        uint32_t t1 = I.a;
        if (t1 == NONE || t1 >= nv) continue;
        uint32_t d = defi[t1];
        if (d == NONE || ndefs[t1] != 1 || uses[t1] != 1) continue;
        const Instr &D = fn.code[d];
        if (D.k != K::Bin) continue;
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

// --- 9a3. dominator-scoped common subexpression elimination -------------------
// A pure, non-trapping Bin recomputed under a *dominating* identical Bin is
// replaced by the dominating def. Visibility is exactly "def dominates use",
// checked with the lib's dominance intervals: we walk the dominator tree (from
// the lib) in preorder, keeping a scoped table — entries pushed on block entry
// are truncated on exit. The renaming map is applied in a SECOND pass over
// the whole function after the walk completes: a Phi's argument is a use on
// an INCOMING EDGE, so the block defining the replaced vreg can be scanned
// AFTER the phi's own block in preorder (a merge block does not sit below the
// predecessor in the dominator tree) — rewriting during the walk would leave
// the phi pointing at a dropped def (a ghost vreg reading as 0).
constexpr bool opt_cse(Fn &fn, const An &A, OptStats &os) {
  if (A.n != fn.nblocks() || !fn.nblocks()) return false;
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  std::vector<uint32_t> ren(nv, NONE);
  auto resolve = [&](uint32_t v) -> uint32_t {
    while (v != NONE && v < nv && ren[v] != NONE) v = ren[v];
    return v;
  };
  // known constants: two temps holding 7 are the SAME operand. CSE keys
  // compare constants by VALUE, not by vreg, so i*7 hits i*7.
  std::vector<char> kconst(nv, 0);
  std::vector<val_t> kval(nv, 0);
  for (const Instr &I : fn.code)
    if (I.k == K::Const && I.dst != NONE && I.dst < nv) {
      kconst[I.dst] = 1;
      kval[I.dst] = I.c;
    }
  struct Ent {
    B op;
    uint32_t x, y;   // vreg operands (NONE if the operand is a known const)
    val_t xv, yv;    // const operand values
    uint32_t val;    // the defining vreg (the CSE replacement)
  };
  std::vector<Ent> table;
  struct Fr {
    uint32_t b, ci, mark;
  };
  std::vector<Fr> st;
  std::vector<char> drop(fn.code.size(), 0);
  bool changed = false;
  auto rows = A.dom_tree.range();
  st.push_back({0, 0, (uint32_t)table.size()});
  while (!st.empty()) {
    uint32_t b = st.back().b;
    uint32_t ci = st.back().ci;
    uint32_t mark = st.back().mark;
    if (ci == 0) { // entering the block: scan its instrs once
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        // resolved operands, LOCAL only (for the CSE key); the mutation
        // happens in the second pass, once the map is complete
        uint32_t ra = resolve(I.a), rb = resolve(I.b2);
        if (I.k != K::Bin || I.dst == NONE) continue;
        if (I.b == B::Div || I.b == B::Mod)
          if (!I.nt) continue; // may trap: keep (nt: proven safe, pure)
        Ent e{};
        e.op = I.b;
        e.x = ra;
        e.y = rb;
        if (e.x != NONE && e.x < nv && kconst[e.x]) {
          e.x = NONE;
          e.xv = kval[ra];
        }
        if (e.y != NONE && e.y < nv && kconst[e.y]) {
          e.y = NONE;
          e.yv = kval[rb];
        }
        bool comm = I.b == B::Add || I.b == B::Mul || I.b == B::Eq ||
                    I.b == B::Ne || I.b == B::And || I.b == B::Or ||
                    I.b == B::Xor || I.b == B::MulHi;
        auto lt = [](const Ent &e, int i) -> uint64_t { // orderable tag
          if (i == 0) return e.x == NONE ? ((uint64_t)1 << 63) ^
                                              (uint64_t)e.xv
                                         : (uint64_t)e.x;
          return e.y == NONE ? ((uint64_t)1 << 63) ^ (uint64_t)e.yv
                             : (uint64_t)e.y;
        };
        if (comm && lt(e, 1) < lt(e, 0)) {
          std::swap(e.x, e.y);
          std::swap(e.xv, e.yv);
        }
        e.val = I.dst;
        bool found = false;
        for (uint32_t t = 0; t < table.size(); ++t) // tables stay tiny
          if (table[t].op == e.op && table[t].x == e.x && table[t].y == e.y &&
              (e.x != NONE || table[t].xv == e.xv) &&
              (e.y != NONE || table[t].yv == e.yv)) {
            ren[I.dst] = table[t].val;
            found = true;
            break;
          }
        if (found) {
          drop[i] = 1;
          os.cses++;
          changed = true;
        } else {
          table.push_back(e);
        }
      }
    }
    auto ch = rows[b]; // children of b in the dominator tree (from the lib)
    if (ci < (uint32_t)ch.size()) {
      st.back().ci = ci + 1;
      uint32_t nb = (uint32_t)ch[ci];
      st.push_back({nb, 0, (uint32_t)table.size()});
      continue;
    }
    table.resize(mark); // leaving b: its entries go out of scope
    st.pop_back();
  }
  if (changed) {
    // second pass: rewrite EVERY operand (Phi args included) with the
    // COMPLETED map -- sound because ren[v] = w implies w's def dominates
    // v's def, which dominates every (edge-)use of v
    for (uint32_t b = 0; b < fn.nblocks(); ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        Instr &I = fn.code[i];
        if (I.a != NONE) I.a = resolve(I.a);
        if (I.b2 != NONE) I.b2 = resolve(I.b2);
        if (I.c2 != NONE) I.c2 = resolve(I.c2);
        for (uint32_t &av : I.args) av = resolve(av);
      }
    // sweep CSE'd instrs, preserving block ranges
    bool any_drop = false;
    for (char c : drop) any_drop |= (c != 0);
    if (any_drop) {
      std::vector<Instr> nc{};
      nc.reserve(fn.code.size());
      for (uint32_t b = 0; b < fn.nblocks(); ++b) {
        uint32_t ob = fn.blocks[b].begin, oe = fn.blocks[b].end;
        fn.blocks[b].begin = (uint32_t)nc.size();
        for (uint32_t i = ob; i < oe; ++i)
          if (!drop[i]) nc.push_back(fn.code[i]);
        fn.blocks[b].end = (uint32_t)nc.size();
      }
      fn.code = std::move(nc);
    }
  }
  return changed;
}

// --- 9b. unreachable block removal + phi arg pruning -------------------------
// Phi instrs carry their pred block ids in t/f (set during SSA construction),
// so pruning is by pred identity — no guesswork.
constexpr bool opt_remove_unreachable(Fn &fn, OptStats &os) {
  std::vector<char> reach(fn.nblocks(), 0);
  std::vector<uint32_t> wl{0};
  reach[0] = 1;
  while (!wl.empty()) {
    uint32_t b = wl.back(); wl.pop_back();
    for (uint32_t s : {fn.succ0(b), fn.succ1(b)})
      if (s != NONE && !reach[s]) { reach[s] = 1; wl.push_back(s); }
  }
  bool any = false;
  for (uint32_t b = 0; b < fn.nblocks(); ++b) {
    if (reach[b]) continue;
    if (b != 0 && fn.blocks[b].begin == fn.blocks[b].end &&
        fn.succ0(b) == NONE && fn.succ1(b) == NONE)
      continue; // isolated by an earlier round: old news, not a change
    any = true;  // genuinely new dead code (or a newly-folded edge)
    break;
  }
  if (any) {
    // compact code of reachable blocks; isolate unreachable ones
    std::vector<Instr> nc{};
    uint32_t gone = 0;
    for (uint32_t b = 0; b < fn.nblocks(); ++b) {
      uint32_t ob = fn.blocks[b].begin, oe = fn.blocks[b].end;
      fn.blocks[b].begin = (uint32_t)nc.size();
      if (reach[b]) {
        for (uint32_t i = ob; i < oe; ++i) nc.push_back(fn.code[i]);
      } else {
        if (oe > ob || fn.succ[2 * b] != NONE || fn.succ[2 * b + 1] != NONE)
          ++gone; // only count blocks that actually died THIS round
        fn.succ[2 * b] = NONE;      // isolate: no stale edges into analyses
        fn.succ[2 * b + 1] = NONE;
      }
      fn.blocks[b].end = (uint32_t)nc.size();
    }
    fn.code = std::move(nc);
    os.blocks_gone += gone;
  }
  // succ may have changed (Cbr folds) even without dead blocks: always
  // refresh preds, then prune phi args by pred identity
  fn.recompute_preds();
  bool degenerated = false;
  for (uint32_t b = 0; b < fn.nblocks(); ++b) {
    if (fn.preds[b].empty()) continue;
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k != K::Phi) break;
      auto is_live_pred = [&](uint32_t p) {
        for (uint32_t q : fn.preds[b])
          if (q == p) return true;
        return false;
      };
      bool l0 = (I.a != NONE) && is_live_pred(I.t);
      bool l1 = (I.b2 != NONE) && is_live_pred(I.f);
      uint32_t keep = l0 ? I.a : (l1 ? I.b2 : NONE);
      if (l0 && l1) continue;         // both preds live: phi unchanged
      if (I.k != K::Phi) continue;    // already degenerated on an earlier visit
      if (keep == NONE) {             // no live pred (cannot happen in a live
        I.k = K::Const; I.c = 0;      // block, but be safe) -> undef, DCE food
        os.folds++;
      } else {                        // one pred left: phi degenerates to copy
        I.k = K::Copy; I.a = keep; I.b2 = NONE;
        os.copies++;
      }
      os.phi_degens++;
      degenerated = true;
      I.t = I.f = NONE;
    }
  }
  return any || degenerated;
}

// --- 9b2. block splicing ------------------------------------------------------
// If a single-succ block b ends in `br s`, and s has exactly one pred (b) and
// no phis, s's code is appended to b (b's Br dropped). This collapses the
// jump chains that branch folding leaves behind. Phi pred ids in s's
// successors are rekeyed s -> b so phi args stay pred-identified.
constexpr bool opt_splice(Fn &fn, OptStats &os) {
  uint32_t nb = fn.nblocks();
  std::vector<uint32_t> absorbed(nb, NONE); // block -> absorber
  std::vector<uint32_t> merges(nb, NONE);   // absorber -> absorbed block
  bool any = false;
  for (uint32_t b = 0; b < nb; ++b) {
    if (absorbed[b] != NONE || merges[b] != NONE)
      continue; // already in a pair this sweep (matchings only: no chains)
    uint32_t s = fn.succ0(b);
    if (fn.succ1(b) != NONE || s == NONE) continue; // dead or 2-succ
    if (s == b || s == 0) continue;         // self-loop / entry: keep
    if (absorbed[s] != NONE || merges[s] != NONE) continue;
    if (fn.preds[s].size() != 1 || fn.preds[s][0] != b) continue;
    bool s_has_phi = false;
    for (uint32_t i = fn.blocks[s].begin; i < fn.blocks[s].end; ++i)
      if (fn.code[i].k == K::Phi) { s_has_phi = true; break; }
    if (s_has_phi) continue;
    // absorb s into b
    absorbed[s] = b;
    merges[b] = s;
    any = true;
    os.splices++;
    // rekey phis in s's successors (pred s -> pred b) and refresh their preds
    for (uint32_t t : {fn.succ0(s), fn.succ1(s)}) {
      if (t == NONE) continue;
      for (uint32_t i = fn.blocks[t].begin; i < fn.blocks[t].end; ++i) {
        Instr &I = fn.code[i];
        if (I.k != K::Phi) break;
        if (I.t == s) I.t = b;
        if (I.f == s) I.f = b;
      }
      for (uint32_t &p : fn.preds[t])
        if (p == s) p = b;
    }
  }
  if (!any) return false;
  // rebuild: absorbed blocks vanish; an absorber emits its own non-terminator
  // instrs, then its absorbed block's instrs, then (that block may itself
  // have absorbed someone in this sweep) the whole chain, ending at the final
  // terminator. Ranges AND succs are read from SNAPSHOTS: the rebuild
  // overwrites fn.blocks/fn.succ while chain ends still need the absorbed
  // blocks' ORIGINAL successors (an absorbed block isolated earlier in this
  // same loop must not poison its absorber with NONE succs).
  std::vector<Block> old_blocks = fn.blocks;
  std::vector<uint32_t> old_succ = fn.succ;
  std::vector<Instr> nc{};
  nc.reserve(fn.code.size());
  for (uint32_t b = 0; b < nb; ++b) {
    if (absorbed[b] != NONE) {
      fn.blocks[b] = {(uint32_t)nc.size(), (uint32_t)nc.size()};
      fn.succ[2 * b] = NONE;
      fn.succ[2 * b + 1] = NONE;
      continue;
    }
    uint32_t begin = (uint32_t)nc.size();
    uint32_t x = b;
    for (;;) {
      uint32_t xb = old_blocks[x].begin, xe = old_blocks[x].end;
      uint32_t m = merges[x];
      if (m == NONE) { // chain end: emit everything incl. the terminator
        for (uint32_t i = xb; i < xe; ++i) nc.push_back(fn.code[i]);
        fn.succ[2 * b] = old_succ[2 * x];
        fn.succ[2 * b + 1] = old_succ[2 * x + 1];
        break;
      }
      // x absorbed m: emit x's instrs but its trailing Br, then follow
      if (xe > xb)
        for (uint32_t i = xb; i + 1 < xe; ++i) nc.push_back(fn.code[i]);
      x = m;
    }
    fn.blocks[b] = {begin, (uint32_t)nc.size()};
  }
  fn.code = std::move(nc);
  fn.recompute_preds();
  return true;
}

// --- 9c. SSA dead-code elimination -------------------------------------------
// roots: Print, Br, Cbr, Exit, and Div/Mod whose divisor is not a known
// nonzero constant (they may trap — removing them would erase the trap).
constexpr bool opt_dce(Fn &fn, OptStats &os) {
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  std::vector<char> known(nv, 0);
  std::vector<val_t> kval(nv, 0);
  for (const Instr &I : fn.code)
    if (I.k == K::Const) { known[I.dst] = 1; kval[I.dst] = I.c; }
  // def index of each vreg
  std::vector<uint32_t> def_idx(nv, NONE);
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
      if (fn.code[i].dst != NONE) def_idx[fn.code[i].dst] = i;
  auto may_trap = [&](const Instr &I) {
    if (I.k != K::Bin || (I.b != B::Div && I.b != B::Mod)) return false;
    if (I.nt) return false; // proven trap-free: pure, removable
    return !(I.b2 < nv && known[I.b2] && kval[I.b2] != 0);
  };
  std::vector<char> live(fn.code.size(), 0);
  std::vector<uint32_t> wl{};
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      // roots: side effects (Print is output, Store mutates the arena,
      // Input consumes the stdin tape), control flow, and anything that may
      // TRAP (Div/Mod with a non-constant divisor, Alloc: n<0 or OOM,
      // Load: out of bounds) — removing them would erase observable behavior.
      if (I.k == K::Print || I.k == K::Br || I.k == K::Cbr || I.k == K::Exit ||
          I.k == K::Store || I.k == K::Input || I.k == K::Alloc ||
          I.k == K::Load || I.k == K::Ret || I.k == K::Call || may_trap(I)) {
        live[i] = 1;
        wl.push_back(i);
      }
    }
  while (!wl.empty()) {
    uint32_t i = wl.back(); wl.pop_back();
    for (uint32_t v : fn.code[i].args) { // Call arguments
      if (v == NONE || v >= nv) continue;
      uint32_t d = def_idx[v];
      if (d != NONE && !live[d]) { live[d] = 1; wl.push_back(d); }
    }
    for (uint32_t v : {fn.code[i].a, fn.code[i].b2, fn.code[i].c2}) {
      if (v == NONE || v >= nv) continue;
      uint32_t d = def_idx[v];
      if (d != NONE && !live[d]) { live[d] = 1; wl.push_back(d); }
    }
  }
  // sweep
  bool changed = false;
  {
    std::vector<Instr> nc{};
    nc.reserve(fn.code.size());
    for (uint32_t b = 0; b < fn.nblocks(); ++b) {
      uint32_t ob = fn.blocks[b].begin, oe = fn.blocks[b].end;
      fn.blocks[b].begin = (uint32_t)nc.size();
      for (uint32_t i = ob; i < oe; ++i) {
        if (live[i]) nc.push_back(fn.code[i]);
        else { changed = true; os.dce++; }
      }
      fn.blocks[b].end = (uint32_t)nc.size();
    }
    fn.code = std::move(nc);
  }
  // trivial-phi collapse (x = phi(a, a) / phi(self, a) -> copy), cascading
  for (int round = 0; round < 10; ++round) {
    bool ch = false;
    for (uint32_t b = 0; b < fn.nblocks(); ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        Instr &I = fn.code[i];
        if (I.k != K::Phi) continue;
        uint32_t x = (I.a != NONE && I.a != I.dst) ? I.a : NONE;
        uint32_t y = (I.b2 != NONE && I.b2 != I.dst) ? I.b2 : NONE;
        if (x == NONE && y == NONE) continue;         // all self/undef
        if (x != NONE && y != NONE && x != y) continue; // genuine phi
        uint32_t pick = (x != NONE) ? x : y;            // phi(a), phi(a,self)
        I.k = K::Copy; I.a = pick; I.b2 = NONE;
        I.t = I.f = NONE;
        os.copies++; ch = true;
      }
    if (!ch) break;
    changed |= ch;
  }
  return changed;
}

// def instr index of vreg v (linear scan; diagnostics only)
constexpr uint32_t def_index_of(const Fn &fn, uint32_t v) {
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
      if (fn.code[i].dst == v) return i;
  return NONE;
}

// --- 9d. loop-invariant code motion -------------------------------------------
// Uses the lib's natural loops + dominance intervals:
//   - loop bodies from calculate_natural_loops_with_header_...
//   - ordering by nesting depth from calculate_nesting_of_natural_loop_headers
//   - "def dominates preheader" via the dominance intervals
// Hoist pure non-trapping instrs whose operands are all defined outside the
// loop (and dominate the preheader) into the preheader. Only loops with a
// unique outside predecessor are eligible (preheader must dominate header).
constexpr bool opt_licm(Fn &fn, const An &A, OptStats &os) {
  if (A.n != fn.nblocks()) return false;
  bool changed_any = false;
  // headers sorted innermost-first (nest count descending)
  std::vector<uint32_t> headers;
  for (uint32_t h = 0; h < A.n; ++h)
    if (A.inner[h] == h) headers.push_back(h);
  for (size_t i = 0; i + 1 < headers.size(); ++i)   // tiny sorts are fine here
    for (size_t j = i + 1; j < headers.size(); ++j)
      if (A.nest[headers[j]] > A.nest[headers[i]])
        std::swap(headers[i], headers[j]);
  for (uint32_t h : headers) {
    auto row = A.loops.range()[h];
    if (row.empty()) continue;
    bool inL_flag_dummy = false;
    (void)inL_flag_dummy;
    std::vector<char> inL(A.n, 0);
    for (ulen x : row) inL[(uint32_t)x] = 1;
    inL[h] = 1;
    // unique outside pred?
    uint32_t pre = NONE;
    uint32_t outside = 0;
    for (uint32_t p : fn.preds[h]) {
      if (!inL[p]) { pre = p; ++outside; }
    }
    if (outside != 1 || pre == NONE) continue;
    // def block of every vreg (recomputed each hoist round)
    for (int round = 0; round < 10; ++round) {
      const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
      std::vector<uint32_t> defb(nv, NONE);
      for (uint32_t b = 0; b < fn.nblocks(); ++b)
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
          if (fn.code[i].dst != NONE) defb[fn.code[i].dst] = b;
      auto invariant = [&](uint32_t v) -> bool {
        if (v == NONE) return true;
        if (v >= nv) return false;
        uint32_t d = defb[v];
        if (d == NONE) return false;
        if (inL[d]) return false;              // defined inside: not invariant
        return A.dominates(d, pre);            // reachable into preheader
      };
      bool changed = false;
      for (ulen xb : row) {
        uint32_t B = (uint32_t)xb;
        if (B == h) continue;                  // header holds the phi+cond
        for (uint32_t i = fn.blocks[B].begin; i < fn.blocks[B].end; ++i) {
          Instr &I = fn.code[i];
          if (I.dst == NONE) continue;                 // control/print: no
          if (I.k == K::Phi || I.k == K::Cbr || I.k == K::Br ||
              I.k == K::Print || I.k == K::Exit)
            continue;
          if (I.k == K::Bin && (I.b == B::Div || I.b == B::Mod) && !I.nt)
            continue; // may trap: no hoist (nt: proven safe, hoistable)
          if (I.k == K::Const) continue;               // nothing to gain
          // arena/stdin ops are NOT loop-invariant: Alloc returns a fresh
          // handle per execution, Load/Store touch mutable memory, Len can
          // change when an Alloc lands in between, Input consumes the tape.
          if (I.k == K::Alloc || I.k == K::Load || I.k == K::Store ||
              I.k == K::Len || I.k == K::Input)
            continue;
          if (I.k == K::Call)
            continue; // callee may print/trap/read stdin: never invariant
          if (!invariant(I.a) || !invariant(I.b2)) continue;
          // hoist: move to the preheader, right before its terminator.
          // The terminator is always the last instr, so this is guaranteed
          // to be after any phis AND after every operand def that lives in
          // the preheader itself.
          uint32_t pe_old = fn.blocks[pre].end - 1;   // terminator, pre-erase
          Instr copy = I;
          fn.code.erase(fn.code.begin() + (long)i);   // remove from B
          // insertion index in the post-erase vector:
          uint32_t pe = (pe_old > i) ? pe_old - 1 : pe_old;
          fn.code.insert(fn.code.begin() + (long)pe, copy);
          // range fixup 1: everything after the erased index shifts down
          for (uint32_t x = 0; x < fn.nblocks(); ++x) {
            if (fn.blocks[x].begin > i) fn.blocks[x].begin--;
            if (fn.blocks[x].end > i) fn.blocks[x].end--;
          }
          // range fixup 2: the hoisted instr now lives inside `pre` (before
          // its terminator); blocks after it shift up
          for (uint32_t x = 0; x < fn.nblocks(); ++x) {
            if (x == pre) { fn.blocks[x].end++; continue; }
            if (fn.blocks[x].begin >= pe + 1) fn.blocks[x].begin++;
            if (fn.blocks[x].end >= pe + 1) fn.blocks[x].end++;
          }
          os.hoisted++;
          changed = true;
          break;                                      // rescan this block
        }
        if (changed) break;
      }
      if (!changed) break;
      changed_any = true;
    }
  }
  return changed_any;
}

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
  const uint32_t plain = 4 * nb + 16;
  const uint32_t widened_cap = 4 * nb + 16;
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
  {
    std::vector<std::vector<uint32_t>> stacks(total_slots);
    for (uint32_t s = 0; s < total_slots; ++s)
      if (init_vreg[s] != NONE) stacks[s].push_back(init_vreg[s]);
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


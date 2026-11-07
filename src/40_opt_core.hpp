#pragma once

// ============================================================================
// 7. analysis driver — all the graph math comes from mjz::graph_ns
// ============================================================================
struct An {
  std::vector<std::vector<ulen>> adjs{}, prs{};
  gns::basic_forest_t<V, std::vector<ulen>> fs{}, fp{};
  std::vector<ulen> idom{}, ib{}, ie{};
  gns::basic_forest_t<V, std::vector<ulen>> dom_tree{};
  gns::basic_forest_t<V, std::vector<ulen>> loops{};
  std::vector<ulen> inner{}, outer{}, nest{};
  gns::basic_forest_t<V, std::vector<ulen>> df{};
  uint32_t n{};

  constexpr bool dominates(uint32_t d, uint32_t s) const {
    if (d >= n || s >= n) return false;
    if (ib[d] == 0 || ib[s] == 0) return false;
    return ib[d] <= ib[s] && ie[d] >= ie[s];
  }
};

constexpr An analyze(const Fn &fn) {
  An A{};
  A.n = fn.nblocks();
  A.adjs.assign(A.n, {});
  A.prs.assign(A.n, {});
  for (uint32_t b = 0; b < A.n; ++b) {
    if (fn.succ0(b) != NONE) A.adjs[b].push_back(fn.succ0(b));
    if (fn.succ1(b) != NONE) A.adjs[b].push_back(fn.succ1(b));
    for (uint32_t p : fn.preds[b]) A.prs[b].push_back(p);
  }
  // -- everything below is mjz_lib doing the heavy lifting --
  A.fs = gns::make_basic_forest<V>(A.adjs);            // CSR successor graph
  A.fp = gns::make_basic_inv_forest<V>(A.adjs);        // CSR predecessor graph
  A.idom = gns::calculate_dominators_from_entry<V>(0, A.fs, A.fp); // Lengauer–Tarjan
  A.dom_tree = gns::make_dominator_forest_given_dominators<V>(A.idom);
  auto iv = gns::calculate_dominance_intervals_given_dominators_and_forest<V>(
      A.dom_tree, A.idom);
  A.ib = iv.first;
  A.ie = iv.second;
  A.loops =
      gns::calculate_natural_loops_with_header_given_dominance_intervals<V>(
          A.ib, A.ie, A.fp, true);
  A.inner.assign(A.n, ulen(-1));
  A.outer.assign(A.n, 0);
  A.nest.assign(A.n, 0);
  gns::calculate_nesting_of_natural_loop_headers<V>(A.loops, A.inner, A.outer,
                                                    A.nest);
  A.df = gns::calculate_dominance_frontiers_given_dominators<V>(A.idom, A.fp);
  return A;
}

// ============================================================================
// 8. SSA construction
//    - phi placement at the iterated dominance frontier of each def set
//      (mjz::graph_ns::calculate_iterated_dominance_frontiers_...)
//    - renaming by dominator-tree walk (dom tree from the lib)
// ============================================================================
struct SsaStats {
  uint32_t phis{};
  uint32_t versions{};
};

constexpr bool ssa_construct(Fn &fn, const An &A, SsaStats &st) {
  st = {};
  const uint32_t nb = fn.nblocks();
  // ---- 8a. def blocks per original vreg
  std::vector<std::vector<ulen>> defs(fn.nvars);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
      if (fn.code[i].dst != NONE && fn.code[i].k != K::Phi)
        defs[fn.code[i].dst].push_back(b);
  // parameters are "defined at entry" (block 0): uses before any assignment
  // see the incoming argument; re-assignments get ordinary phi placement
  for (uint32_t v = 0; v < fn.nparams; ++v) defs[v].push_back(0);
  // ---- 8b. phi insertion at the IDF of each multi-def vreg's def set
  std::vector<std::vector<uint32_t>> phis_at(nb);
  for (uint32_t v = 0; v < fn.nvars; ++v) {
    if (defs[v].size() < 2) continue;
    std::vector<ulen> idf =
        gns::calculate_iterated_dominance_frontiers_given_dominance_frontiers<
            V>(A.df, defs[v]);
    for (ulen blk : idf) {
      uint32_t b = (uint32_t)blk;
      if (b >= nb || fn.preds[b].size() < 2) continue;
      bool dup = false;
      for (uint32_t x : phis_at[b]) dup |= (x == v);
      if (!dup) phis_at[b].push_back(v);
    }
  }
  // ---- 8c. rebuild the code vector with phis at block starts
  {
    std::vector<Instr> nc{};
    nc.reserve(fn.code.size() + 32);
    for (uint32_t b = 0; b < nb; ++b) {
      uint32_t old_begin = fn.blocks[b].begin;
      fn.blocks[b].begin = (uint32_t)nc.size();
      for (uint32_t v : phis_at[b]) {
        nc.push_back({.k = K::Phi, .a = NONE, .b2 = NONE, .c = (int64_t)v});
        st.phis++;
      }
      for (uint32_t i = old_begin; i < fn.blocks[b].end; ++i)
        nc.push_back(fn.code[i]);
      fn.blocks[b].end = (uint32_t)nc.size();
    }
    fn.code = std::move(nc);
  }
  // ---- 8d. renaming: dominator-tree DFS with per-base version stacks
  fn.version_base.clear();
  fn.version_no.clear();
  std::vector<std::vector<uint32_t>> stacks(fn.nvars);
  for (uint32_t v = 0; v < fn.nparams; ++v)
    stacks[v].push_back(v); // version 0 of a param is the param itself
  auto fresh = [&](uint32_t base) -> uint32_t {
    uint32_t v = fn.nvars + (uint32_t)fn.version_base.size();
    fn.version_base.push_back(base);
    fn.version_no.push_back((uint32_t)stacks[base].size());
    st.versions++;
    return v;
  };
  // phi dsts are fresh versions, pushed when visiting their block
  // explicit-stack DFS over the dominator tree (children rows from the lib)
  struct Frame {
    uint32_t block, child_idx, pushed;
  };
  std::vector<Frame> stack;
  // visit block 0
  auto visit_prologue = [&](uint32_t b, uint32_t &pushed) -> bool {
    pushed = 0;
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k == K::Phi) {
        uint32_t base = (uint32_t)I.c;
        I.dst = fresh(base);
        stacks[base].push_back(I.dst);
        ++pushed;
        continue;
      }
      // rewrite uses
      if (I.a != NONE && I.a < fn.nvars) {
        auto &s = stacks[I.a];
        if (s.empty()) return false; // use before def (should not happen)
        I.a = s.back();
      }
      if (I.b2 != NONE && I.b2 < fn.nvars) {
        auto &s = stacks[I.b2];
        if (s.empty()) return false;
        I.b2 = s.back();
      }
      if (I.c2 != NONE && I.c2 < fn.nvars) { // Store: the stored value
        auto &s = stacks[I.c2];
        if (s.empty()) return false;
        I.c2 = s.back();
      }
      for (uint32_t &av : I.args) { // Call arguments
        if (av != NONE && av < fn.nvars) {
          auto &s = stacks[av];
          if (s.empty()) return false;
          av = s.back();
        }
      }
      if (I.dst != NONE) {
        uint32_t base = I.dst;
        I.dst = fresh(base);
        stacks[base].push_back(I.dst);
        ++pushed;
      }
    }
    // seed phi args in successors
    for (uint32_t s : {fn.succ0(b), fn.succ1(b)}) {
      if (s == NONE) continue;
      for (uint32_t i = fn.blocks[s].begin; i < fn.blocks[s].end; ++i) {
        Instr &I = fn.code[i];
        if (I.k != K::Phi) break;
        uint32_t base = (uint32_t)I.c;
        const auto &st_ = stacks[base];
        // find slot of b in preds[s]
        uint32_t slot = NONE;
        for (uint32_t j = 0; j < fn.preds[s].size(); ++j)
          if (fn.preds[s][j] == b) slot = j;
        if (slot == NONE) return false;
        // Minimal (non-pruned) SSA: a phi arg may be UNDEFINED on an edge
        // (variable not live-in there). Scoping guarantees it is never read
        // on that path, so record the pred with arg NONE; DCE drops the phi.
        uint32_t arg = st_.empty() ? NONE : st_.back();
        // record the pred block id with the arg (t/f unused by Phi) so that
        // later CFG edits can prune phi args by pred identity
        if (slot == 0) { I.a = arg; I.t = b; }
        else           { I.b2 = arg; I.f = b; }
      }
    }
    return true;
  };
  auto visit_epilogue = [&](uint32_t b, uint32_t pushed) {
    // pop exactly `pushed` entries (one per def the prologue pushed; the
    // count equals the block's defining instrs — decrement ONLY on pops)
    for (uint32_t i = fn.blocks[b].begin;
         i < fn.blocks[b].end && pushed; ++i) {
      const Instr &I = fn.code[i];
      if (I.k == K::Phi) {
        stacks[(uint32_t)I.c].pop_back();
        --pushed;
      } else if (I.dst != NONE) {
        stacks[fn.base_of(I.dst)].pop_back();
        --pushed;
      }
    }
  };
  {
    uint32_t pushed0 = 0;
    if (!visit_prologue(0, pushed0)) return false;
    stack.push_back({0, 0, pushed0});
    auto children_of = [&](uint32_t b) {
      return A.dom_tree.range()[b];
    };
    while (!stack.empty()) {
      Frame &fr = stack.back();
      auto ch = children_of(fr.block);
      if (fr.child_idx < (uint32_t)ch.size()) {
        uint32_t next = (uint32_t)ch[fr.child_idx++];
        uint32_t pushed = 0;
        if (!visit_prologue(next, pushed)) return false;
        stack.push_back({next, 0, pushed});
      } else {
        visit_epilogue(fr.block, fr.pushed);
        stack.pop_back();
      }
    }
  }
  return true;
}
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
  uint32_t scctco{};      // SCC members whose inner-SCC tail calls collapsed
  uint32_t scctco_folds{}; // collapsed count-down loops folded to & (c-1)
};

// --- 9a. constant folding / propagation / branch folding / identities -------
// known: vreg -> const value. iterates to fixpoint.
constexpr bool opt_constprop(Fn &fn, OptStats &os) {
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

// ============================================================================
// MiniJZ compiler — §9e0 pass manager: tiers, fixpoints, optimize()/optimize_prog()
// ----------------------------------------------------------------------------
// Ordered-include header: this file is a slice of the original single
// translation unit and relies on being included after its predecessors
// (see src/minijz.hpp for the canonical order). Do not include it
// directly in an arbitrary order.
// ============================================================================
#pragma once

// ============================================================================
// 9e0. PASS MANAGER
// ============================================================================
// PassCfg is the single configuration threaded from the CLI through
// compile() -> optimize_prog() -> optimize(). Passes live in three cost
// tiers and the scheduler runs them with a strict discipline:
//   tier 0 "cheap"    : constprop, copyprop, canon, cse, unreachable,
//                       splice, dce -- always run to a FIXPOINT first
//   tier 1 "middle"   : mem2reg (array lifting + dead stores + dead
//                       allocs), trapelim(+range analysis), licm, divmagic,
//                       reassoc -- one round each, followed by a tier-0
//                       fixpoint, repeated until global fixpoint / --fp-limit
//   tier 2 "expensive": unroll, lcf -- CFG restructuring, run ONLY after
//                       tiers 0/1 reached their fixpoint; each round is
//                       followed by full tier-0 (+1) fixpoints again.
//                       const-call evaluation is interprocedural (driven
//                       from optimize_prog, after the inlining rounds):
//                       pure constant-argument calls are executed exactly
//                       at compile time, recursive fns included
// -O levels gate the tiers:
//   -O0: nothing (SSA + destruction only)
//   -O1: tier 0
//   -O2: tiers 0+1
//   -O3: everything incl. interprocedural inlining (default)
// Per-pass overrides (--enable/--disable) are applied on top of the preset,
// so `-O1 --enable=lcf` is legal. --debug=N controls the dump volume:
//   0 silent; 1 one line per pass invocation; 2 + the IR after each pass
//   that changed something; 3 reserved for analysis dumps.
struct PassCfg {
  uint32_t level{3};   // -O0..-O3 preset
  uint32_t unroll{16}; // full-unroll constant-trip loops with N <= this
  uint32_t fp_limit{24}; // fixpoint round cap for every tier loop
  uint8_t debug{0};    // dump level (0..3)
  bool regalloc{true}; // C/bytecode register allocation (coloring)
  // tier 0
  bool p_fold{true}, p_copyprop{true}, p_canon{true}, p_cse{true},
      p_unreach{true}, p_splice{true}, p_dce{true};
  // tier 1
  bool p_mem2reg{true}, p_trapelim{true}, p_licm{true}, p_divmagic{true},
      p_reassoc{true};
  // tier 2
  bool p_unroll{true}, p_lcf{true}, p_constcall{true}, p_tailrec{true},
      p_cdfold{true}, p_ifconv{true};
  // interprocedural
  bool do_inline{true};
  bool do_opt{true}; // false: skip the optimizer entirely
  // this fn is main (fns[0]): its frame count is provably 1, so a K::Depth
  // in it constant-folds to 1
  bool is_main{false};
  // program-wide gates (filled in by compile(); see prog_handle_gates)
  bool lift_ok{true};   // mem2reg array promotion allowed
  bool stkarr_ok{true}; // C-backend stack arrays allowed
  // runtime-only debugging context (constexpr path leaves it empty)
  std::string_view dbg_name{};

  constexpr void set_level(uint32_t lv) {
    level = lv;
    bool t0 = lv >= 1, t1 = lv >= 2, t2 = lv >= 3;
    p_fold = p_copyprop = p_canon = p_cse = p_unreach = p_splice = p_dce = t0;
    p_mem2reg = p_trapelim = p_licm = p_divmagic = p_reassoc = t1;
    p_unroll = p_lcf = p_constcall = p_tailrec = p_cdfold = p_ifconv = t2;
    do_inline = t2;
    do_opt = lv != 0;
  }
};

constexpr bool optimize(Fn &fn, OptStats &os, An &final_an,
                        PassCfg cfg = {}); // (defined below)

// --- 9e2. interprocedural: call graph, SCC (mjz_lib), inlining ---------------
// Nodes are functions (index 0 = the main body); edges are call sites. SCCs
// come from mjz::graph_ns::calculate_strongly_connected_components (Tarjan):
//   * a function in a multi-member SCC, or with a self-edge, is RECURSIVE
//     and is never inlined (inlining it would duplicate unbounded work and
//     break the SSA shape)
//   * Tarjan finalizes an SCC only after every SCC it can reach, so walking
//     SCCs in emission order processes callees before callers: inlined
//     bodies propagate through the rounds

// Inline ONE call site `dst = G(args)` in F at instr index ci. Both sides are
// in SSA. The callee's blocks are copied with fresh vreg/block ids; params
// are SUBSTITUTED by the caller's argument vregs (they dominate the call, so
// they dominate the whole inlined region). The caller's block splits at the
// call: everything after it moves to a fresh continuation block M. Several
// returns merge through a chain of 2-pred phi blocks (this IR's phis hold
// exactly two args); the chain end jumps to M. Uses of the call result are
// rewritten to the merged value.
constexpr bool opt_inline_one(Fn &F, const Fn &G, uint32_t ci) {
  if (ci >= F.code.size()) return false;
  const Instr call = F.code[ci]; // value copy: the vector is rebuilt below
  if (call.k != K::Call || call.dst == NONE) return false;
  if (call.args.size() != G.nparams) return false;
  const uint32_t nbF = F.nblocks(), nbG = G.nblocks();
  const uint32_t nvG = G.nvars + (uint32_t)G.version_base.size();
  // G's entry must be phi-free (entry blocks never have phis; paranoia)
  for (uint32_t i = G.blocks[0].begin; i < G.blocks[0].end; ++i)
    if (G.code[i].k == K::Phi) return false;
  // locate the caller block holding the call
  uint32_t B = NONE;
  for (uint32_t b = 0; b < nbF; ++b)
    if (ci >= F.blocks[b].begin && ci < F.blocks[b].end) {
      B = b;
      break;
    }
  if (B == NONE) return false;
  const uint32_t Bs0 = F.succ[2 * B], Bs1 = F.succ[2 * B + 1];
  // vreg map: params -> caller args; all other callee vregs -> fresh ids
  std::vector<uint32_t> vmap(nvG, NONE);
  for (uint32_t v = 0; v < nvG; ++v) {
    if (v < G.nparams) vmap[v] = call.args[v];
    else vmap[v] = new_anon_vreg(F, G.base_of(v));
  }
  auto mapv = [&](uint32_t v) -> uint32_t {
    if (v == NONE || v >= nvG) return v;
    return vmap[v] != NONE ? vmap[v] : v;
  };
  // block map: fresh ids appended to F
  std::vector<uint32_t> bmap(nbG, NONE);
  for (uint32_t g = 0; g < nbG; ++g) bmap[g] = nbF + g;
  // return sites: (block, value)
  std::vector<std::pair<uint32_t, uint32_t>> rets;
  for (uint32_t g = 0; g < nbG; ++g)
    for (uint32_t i = G.blocks[g].begin; i < G.blocks[g].end; ++i)
      if (G.code[i].k == K::Ret && i + 1 == G.blocks[g].end)
        rets.push_back({g, G.code[i].a});
  if (rets.empty()) return false;
  const uint32_t R = (uint32_t)rets.size();
  const uint32_t M = nbF + nbG + (R > 1 ? R - 1 : 0);
  // ---- rebuild the code vector (append-only ids; ranges recomputed) ----
  std::vector<Instr> nc;
  nc.reserve(F.code.size() + G.code.size() + 32);
  std::vector<Block> nblocks(M + 1);
  std::vector<uint32_t> nsucc(2 * (M + 1), NONE);
  // 1) F's blocks; B is truncated at the call and ends with Br G-entry
  for (uint32_t b = 0; b < nbF; ++b) {
    uint32_t base = (uint32_t)nc.size();
    if (b == B) {
      for (uint32_t i = F.blocks[b].begin; i < ci; ++i) nc.push_back(F.code[i]);
      nc.push_back({.k = K::Br, .t = bmap[0]});
      nblocks[b] = {base, (uint32_t)nc.size()};
      nsucc[2 * b] = bmap[0];
      nsucc[2 * b + 1] = NONE;
    } else {
      for (uint32_t i = F.blocks[b].begin; i < F.blocks[b].end; ++i)
        nc.push_back(F.code[i]);
      nblocks[b] = {base, (uint32_t)nc.size()};
      nsucc[2 * b] = F.succ[2 * b];
      nsucc[2 * b + 1] = F.succ[2 * b + 1];
    }
  }
  // 2) G's blocks, copied; Ret -> Br into the merge chain
  // K::Depth adjustment: the callee's Depth() is its OWN frame count; the
  // inlined copy runs in F's frame, where the same value is F's Depth()+1.
  // dm = Depth(); one = 1; dadj = dm + one is emitted at the inline point;
  // every copied Depth becomes a Copy of dadj (constprop folds the chain
  // when F is main, where Depth provably = 1).
  uint32_t dmV = NONE, dadjV = NONE;
  bool G_has_depth = false;
  std::vector<Instr> depth_pre{};
  for (const Instr &I : G.code)
    if (I.k == K::Depth) G_has_depth = true;
  if (G_has_depth) {
    dmV = new_anon_vreg(F, 0);
    dadjV = new_anon_vreg(F, 0);
    uint32_t oneV = new_anon_vreg(F, 0);
    depth_pre.push_back(Instr{.k = K::Depth, .dst = dmV});
    depth_pre.push_back(Instr{.k = K::Const, .dst = oneV, .c = 1});
    depth_pre.push_back(
        Instr{.k = K::Bin, .b = B::Add, .dst = dadjV, .a = dmV, .b2 = oneV});
  }
  auto ret_target = [&](uint32_t j) -> uint32_t {
    if (R == 1) return M;
    return (j == 0) ? (nbF + nbG) : (nbF + nbG + j - 1);
  };
  for (uint32_t g = 0; g < nbG; ++g) {
    uint32_t base = (uint32_t)nc.size();
    uint32_t ridx = NONE;
    for (uint32_t j = 0; j < R; ++j)
      if (rets[j].first == g) ridx = j;
    if (g == 0 && !depth_pre.empty()) { // the Depth adjustment prefix lives
      for (const Instr &dp : depth_pre) nc.push_back(dp); // INSIDE this block
    }
    for (uint32_t i = G.blocks[g].begin; i < G.blocks[g].end; ++i) {
      const Instr &I = G.code[i];
      if (i + 1 == G.blocks[g].end && I.k == K::Ret) {
        nc.push_back({.k = K::Br, .t = ret_target(ridx)});
        continue;
      }
      Instr c = I;
      if (c.k == K::Depth) { // callee frame -> caller frame + 1
        c.k = K::Copy;
        c.a = dadjV;
        c.b2 = NONE;
      }
      if (c.dst != NONE) c.dst = mapv(c.dst);
      c.a = mapv(c.a);
      c.b2 = mapv(c.b2);
      if (c.c2 != NONE) c.c2 = mapv(c.c2);
      for (uint32_t &av : c.args) av = mapv(av);
      if (c.k == K::Phi) {
        if (c.t != NONE && c.t < nbG) c.t = bmap[c.t];
        if (c.f != NONE && c.f < nbG) c.f = bmap[c.f];
      } else if (c.k == K::Br) {
        if (c.t < nbG) c.t = bmap[c.t];
      } else if (c.k == K::Cbr) {
        if (c.t < nbG) c.t = bmap[c.t];
        if (c.f < nbG) c.f = bmap[c.f];
      }
      nc.push_back(c);
    }
    nblocks[bmap[g]] = {base, (uint32_t)nc.size()};
    if (nc.size() > base) {
      const Instr &term = nc.back();
      if (term.k == K::Br) {
        nsucc[2 * bmap[g]] = term.t;
      } else if (term.k == K::Cbr) {
        nsucc[2 * bmap[g]] = term.t;
        nsucc[2 * bmap[g] + 1] = term.f;
      }
    }
  }
  // 3) merge chain (R > 1): m_j = [phi(prev, ret_{j+1})] + Br next-or-M
  uint32_t result_val = NONE;
  if (R > 1) {
    uint32_t prev_res = mapv(rets[0].second);
    for (uint32_t j = 0; j + 1 < R; ++j) {
      uint32_t mb = nbF + nbG + j;
      uint32_t base = (uint32_t)nc.size();
      uint32_t rv = new_anon_vreg(F, 0);
      uint32_t a_src = prev_res;
      uint32_t b_src = mapv(rets[j + 1].second);
      uint32_t pa = (j == 0) ? bmap[rets[0].first] : (nbF + nbG + j - 1);
      uint32_t pb = bmap[rets[j + 1].first];
      Instr phi{.k = K::Phi, .dst = rv};
      if (pa < pb) { // slot order follows ascending pred ids
        phi.a = a_src;
        phi.t = pa;
        phi.b2 = b_src;
        phi.f = pb;
      } else {
        phi.a = b_src;
        phi.t = pb;
        phi.b2 = a_src;
        phi.f = pa;
      }
      nc.push_back(phi);
      uint32_t tgt = (j + 2 < R) ? (nbF + nbG + j + 1) : M;
      nc.push_back({.k = K::Br, .t = tgt});
      nblocks[mb] = {base, (uint32_t)nc.size()};
      nsucc[2 * mb] = tgt;
      prev_res = rv;
    }
    result_val = prev_res;
  } else {
    result_val = mapv(rets[0].second);
  }
  // 4) M: the caller's continuation (instrs after the call + terminator)
  {
    uint32_t base = (uint32_t)nc.size();
    for (uint32_t i = ci + 1; i < F.blocks[B].end; ++i)
      nc.push_back(F.code[i]);
    nblocks[M] = {base, (uint32_t)nc.size()};
    nsucc[2 * M] = Bs0;
    nsucc[2 * M + 1] = Bs1;
  }
  // 5) phis in B's old successors: their edge now departs from M
  for (uint32_t s : {Bs0, Bs1}) {
    if (s == NONE || s >= nbF) continue;
    for (uint32_t i = nblocks[s].begin; i < nblocks[s].end; ++i) {
      Instr &I = nc[i];
      if (I.k != K::Phi) break;
      if (I.t == B) I.t = M;
      if (I.f == B) I.f = M;
    }
  }
  // 6) commit
  F.code = std::move(nc);
  F.blocks = std::move(nblocks);
  F.succ = std::move(nsucc);
  // 7) uses of the call result -> merged value
  for (Instr &I : F.code) {
    if (I.a == call.dst) I.a = result_val;
    if (I.b2 == call.dst) I.b2 = result_val;
    if (I.c2 == call.dst) I.c2 = result_val;
    for (uint32_t &av : I.args)
      if (av == call.dst) av = result_val;
  }
  F.recompute_preds();
  return true;
}

// drop functions unreachable from main; renumber Call targets
constexpr void eliminate_dead_fns(std::vector<Fn> &fns,
                                  std::vector<std::string_view> &names,
                                  std::vector<OptStats> &ostats,
                                  std::vector<uint32_t> &src_index) {
  for (;;) {
    const uint32_t n = (uint32_t)fns.size();
    std::vector<char> reach(n, 0);
    std::vector<uint32_t> wl{0};
    reach[0] = 1;
    while (!wl.empty()) {
      uint32_t f = wl.back();
      wl.pop_back();
      for (const Instr &I : fns[f].code)
        if (I.k == K::Call) {
          uint32_t c = (uint32_t)I.c;
          if (c < n && !reach[c]) {
            reach[c] = 1;
            wl.push_back(c);
          }
        }
    }
    bool any_dead = false;
    for (uint32_t f = 1; f < n; ++f) any_dead |= !reach[f];
    if (!any_dead) return;
    std::vector<uint32_t> remap(n, NONE);
    std::vector<Fn> nf;
    std::vector<std::string_view> nn;
    std::vector<OptStats> no;
    std::vector<uint32_t> nsrc;
    for (uint32_t f = 0; f < n; ++f) {
      if (!reach[f]) continue;
      remap[f] = (uint32_t)nf.size();
      nf.push_back(std::move(fns[f]));
      nn.push_back(names[f]);
      no.push_back(ostats[f]);
      nsrc.push_back(src_index[f]);
    }
    for (Fn &F : nf)
      for (Instr &I : F.code)
        if (I.k == K::Call) I.c = remap[(uint32_t)I.c];
    fns = std::move(nf);
    names = std::move(nn);
    ostats = std::move(no);
    src_index = std::move(nsrc);
  }
}

// Whole-program optimization: per-function fixpoint (incl. unrolling), then
// SCC-guided inlining rounds (each inlined caller is re-optimized so constant
// arguments fold through the inlined body), dead-function elimination, and a
// final pass (inlining can expose constant trip counts worth unrolling).
constexpr void optimize_prog(std::vector<Fn> &fns,
                             std::vector<std::string_view> &names,
                             std::vector<OptStats> &ostats, PassCfg cfg,
                             std::vector<uint32_t> &src_index) {
  auto main_aware = [&](PassCfg c, uint32_t f) {
    c.is_main = (f == 0); // main = fns[0] (dead-fn elim never removes it)
    if (f < names.size()) c.dbg_name = names[f];
    return c;
  };
  for (uint32_t f = 0; f < fns.size(); ++f) {
    An A{};
    optimize(fns[f], ostats[f], A, main_aware(cfg, f));
  }
  if (fns.size() < 2) return;
  // ---- interprocedural inlining rounds (SCC-aware; recursive never) -------
  auto inline_rounds = [&](int rounds) {
    if (!cfg.do_inline) return;
    for (int round = 0; round < rounds; ++round) {
      CallGraph G = build_callgraph(fns);
      auto recount = [&]() {
        std::vector<uint32_t> cs(fns.size(), 0);
        for (uint32_t f = 0; f < fns.size(); ++f)
          for (const Instr &I : fns[f].code)
            if (I.k == K::Call) {
              uint32_t c = (uint32_t)I.c;
              if (c < cs.size()) cs[c]++;
            }
        return cs;
      };
      std::vector<uint32_t> callsites = recount();
      bool inlined_any = false;
      for (uint32_t s = 0; s < G.nscc; ++s) {
        uint32_t e = (s + 1 < G.nscc) ? G.scc_begin[s + 1]
                                      : (uint32_t)G.members.size();
        for (uint32_t j = G.scc_begin[s]; j < e; ++j) {
          uint32_t f = G.members[j];
          for (;;) { // rescan after each inline: the fn changed shape
            uint32_t site = NONE, callee = NONE;
            for (uint32_t b = 0; b < fns[f].nblocks() && site == NONE; ++b)
              for (uint32_t i = fns[f].blocks[b].begin;
                   i < fns[f].blocks[b].end; ++i) {
                const Instr &I = fns[f].code[i];
                if (I.k != K::Call) continue;
                uint32_t g = (uint32_t)I.c;
                if (g == 0 || g >= fns.size()) continue; // never inline main
                if (G.recursive[g]) continue;            // SCC: recursive
                uint32_t sz = (uint32_t)fns[g].code.size();
                bool small = sz <= 30;                   // always inline tiny
                bool single_use = callsites[g] == 1 && sz <= 300;
                if (!small && !single_use) continue;
                if (fns[g].nparams != I.args.size()) continue;
                if (fns[f].code.size() + sz > 8000) continue; // growth cap
                site = i;
                callee = g;
                break;
              }
            if (site == NONE) break;
            if (!opt_inline_one(fns[f], fns[callee], site)) break;
            ostats[f].inlines++;
            inlined_any = true;
            callsites = recount();
            { // re-fold this caller with TIER 0 ONLY and a tiny fixpoint
              // budget: this only needs to propagate the inlined body's
              // constants; the final full optimize below runs every tier
              // to ITS fixpoint once the shape settled. Level 2 here (with
              // its mem2reg/trapelim/licm analyses per round) made programs
              // with many call sites quadratic — fuzz seed 21 (26 sites)
              // spent 80+ s in these re-folds alone
              An A2{};
              PassCfg c2 = cfg;
              c2.level = 1;
              c2.fp_limit = 3;
              c2.p_unroll = false;
              c2.p_lcf = false;
              c2.p_cdfold = false;
              c2.p_ifconv = false;
              c2.p_constcall = false;
              c2.do_inline = false;
              optimize(fns[f], ostats[f], A2, main_aware(c2, f));
            }
          }
        }
      }
      if (!inlined_any) break;
    }
  };
  inline_rounds(4);
  // ---- 9e4: tail-call elimination of inner-SCC tail calls ------------------
  // After inlining (non-SCC helpers are gone, so what remains as calls is
  // genuine recursion): self tail-recursion becomes a loop, pure-tail SCCs
  // merge into one sel-dispatched leaf function. Both keep the recursion
  // depth cap EXACT via the budget = CAP+1 - Depth() counter. The merged
  // functions are small leaves now, so a second wave of inlining can pull
  // them (and their countdown loops) into their callers.
  if (cfg.p_tailrec) {
    if (opt_tailrec(fns, names, ostats, src_index)) {
      const uint32_t nf_after = (uint32_t)fns.size();
      for (uint32_t f = 0; f < nf_after; ++f) {
        An A{};
        PassCfg c4 = cfg;
        // every fn tailrec TRANSFORMED gets the FULL cfg now, so cdfold can
        // fold its recursion-budget loop into a closed form while it is
        // still in its own frame. A cheap re-fold here (the old behavior,
        // which only favored the appended merge leaf) left self-recursive
        // loops like cnt(n-1, a+n) UNFOLDED — inline_rounds(2) then pulled
        // the raw loops into every caller, and the callers' final optimize
        // had to fold N TCE loops at once (slow, and a multi-fold hazard).
        // Transformed fns are exactly the ones carrying the budget init:
        // K::Depth is emitted by tce_emit_budget_init alone — user code
        // never produces it, and nothing is inlined between tailrec and
        // here, so the marker is exact. (Callers whose sites were rewritten
        // only need the cheap re-fold below; their full optimize comes last.)
        bool tced = f != 0; // main is never a TCE product
        for (const Instr &I : fns[f].code)
          if (I.k == K::Depth) { tced = true; break; }
        if (tced) { // TCE'd (self loop or merged leaf): FULL cfg
          optimize(fns[f], ostats[f], A, main_aware(c4, f));
        } else { // everyone else: tier-0 re-fold (call sites were rewritten
          c4.level = 1; // to sel-const form; the consts just need folding)
          c4.fp_limit = 3;
          c4.p_constcall = false;
          c4.do_inline = false;
          optimize(fns[f], ostats[f], A, main_aware(c4, f));
        }
      }
      inline_rounds(2); // merged leaves are tiny: inline them into callers
    }
  }
  // ---- 9e3: const-call evaluation ---------------------------------------
  // pure fns called with constant arguments are evaluated exactly at
  // compile time (recursion included -- the call graph is just a CFG to
  // the evaluator); callers are re-folded so the new constants propagate
  // and can expose further const calls, then dead-fn elimination drops
  // every function nobody calls anymore. The re-fold runs the CHEAP tiers
  // only (0+1): constants propagate without unroll/lcf/inline, and the
  // final full optimize below picks up whatever the folds exposed.
  if (cfg.p_constcall) {
    for (int round = 0; round < 8; ++round) {
      if (!opt_constcall(fns, ostats)) break;
      for (uint32_t f = 0; f < fns.size(); ++f) {
        An A{};
        PassCfg c5 = cfg;
        c5.level = 1; // tier 0: propagate the new constants, nothing more
        c5.p_unroll = false;
        c5.p_lcf = false;
        c5.p_constcall = false;
        c5.do_inline = false;
        optimize(fns[f], ostats[f], A, main_aware(c5, f));
      }
    }
  }
  eliminate_dead_fns(fns, names, ostats, src_index);
  for (uint32_t f = 0; f < fns.size(); ++f) {
    An A{};
    optimize(fns[f], ostats[f], A, main_aware(cfg, f));
  }
}

// --- 9e. driver ----------------------------------------------------------------
// runtime-only pass tracing (never on the constexpr path: guarded by
// cfg.debug, which the compile-time pipeline always leaves at 0)
static void print_fn(const Fn &fn, const char *title); // (defined in 13)
static void dbg_pass_rt(uint8_t debug, const Fn &fn, const char *pass, bool ch) {
  std::printf("  [pass %-9s] %s\n", pass, ch ? "changed" : "fixed");
  if (debug >= 2 && ch) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "IR after %s", pass);
    print_fn(fn, buf);
  }
}
// constexpr-safe wrapper: the compile-time pipeline always leaves debug at
// 0, so the early return keeps constant evaluation away from the printf
// tracer (an unreached branch in a constexpr function is legal)
constexpr void dbg_pass(uint8_t debug, const Fn &fn, const char *pass, bool ch) {
  if (debug < 1) return;
  dbg_pass_rt(debug, fn, pass, ch);
}
struct Optimized {
  Fn fn{};         // SSA after optimization
  OptStats os{};
  An an{};         // final analysis (post-opt)
};
constexpr bool optimize(Fn &fn, OptStats &os, An &final_an, PassCfg cfg) {
  if (!cfg.do_opt || cfg.level == 0) { // -O0: untouched IR, fresh analysis
    final_an = analyze(fn);
    return false;
  }
  bool any = false;
  const uint32_t cap = cfg.fp_limit ? cfg.fp_limit : 24;
  if (cfg.debug >= 1) {
    char nb[64] = "<fn>";
    if (!cfg.dbg_name.empty())
      std::snprintf(nb, sizeof nb, "%.*s", (int)cfg.dbg_name.size(),
                    cfg.dbg_name.data());
    std::printf("==== optimize %s (level %u, fp-limit %u) ====\n", nb,
                (unsigned)cfg.level, (unsigned)cap);
  }
  // ---- tier 0: cheap simplification to its fixpoint -----------------------
  // (each round must make progress; dominance passes see a fresh An)
  auto tier0 = [&]() -> bool {
    bool ch_any = false;
    for (uint32_t it = 0; it < cap; ++it) {
      bool ch = false, c = false;
      c = opt_constprop(fn, os, cfg.is_main); // fold/identities/branch folds
      dbg_pass(cfg.debug, fn, "fold", c);
      ch |= c;
      normalize_phis(fn);
#ifndef MJZ_DBG_NO_COPYPROP
      if (cfg.p_copyprop) {
        c = opt_copyprop(fn, os); // copies -> uses of the source
        dbg_pass(cfg.debug, fn, "copyprop", c);
        ch |= c;
      }
#endif
      normalize_phis(fn);
      if (cfg.p_canon) {
        c = opt_canon(fn, os); // canonical forms + NOT-of-compare
        dbg_pass(cfg.debug, fn, "canon", c);
        ch |= c;
      }
      normalize_phis(fn);
#ifndef MJZ_DBG_NO_CSE
      if (cfg.p_cse) {
        An A = analyze(fn);
        c = opt_cse(fn, A, os); // dominating recomputation -> reuse
        dbg_pass(cfg.debug, fn, "cse", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
      if (cfg.p_unreach) {
        c = opt_remove_unreachable(fn, os); // dead blocks + phi degeneration
        dbg_pass(cfg.debug, fn, "unreach", c);
        ch |= c;
      }
      normalize_phis(fn);
#ifndef MJZ_DBG_NO_SPLICE
      if (cfg.p_splice) {
        c = opt_splice(fn, os); // collapse single-pred/succ jump chains
        dbg_pass(cfg.debug, fn, "splice", c);
        ch |= c;
      }
#endif
      normalize_phis(fn);
      if (cfg.p_dce) {
        c = opt_dce(fn, os);
        dbg_pass(cfg.debug, fn, "dce", c);
        ch |= c;
      }
      normalize_phis(fn);
      os.iters = it + 1;
      if (!ch) break;
      ch_any = true;
    }
    return ch_any;
  };
  any |= tier0();
  // ---- tier 1: middle passes; cheap fixpoint after each round -------------
  // trapelim runs ONCE per optimize() call: its nt proofs are monotone (a
  // proof stays valid as the fn shrinks), and its interval analysis is the
  // most expensive thing in the tier — re-running it every fixpoint round
  // of every tier re-analysis made pathological inputs ~20x slower for no
  // additional proofs. Later rounds keep the flags; the tier-2 post-round
  // re-runs it once more (CFG restructuring can expose new proofs).
  bool trapelim_ran = false;
  if (cfg.level >= 2) {
    for (uint32_t outer = 0; outer < cap; ++outer) {
      bool ch = false, c = false;
      if (cfg.p_mem2reg) {
        An A = analyze(fn);
        c = opt_mem2reg(fn, A, os, cfg.lift_ok, cfg.stkarr_ok);
        dbg_pass(cfg.debug, fn, "mem2reg", c);
        ch |= c;
        normalize_phis(fn);
      }
      if (cfg.p_trapelim && (outer == 0 || !trapelim_ran)) {
        An A = analyze(fn);
        c = opt_trapelim(fn, A, os); // ranges + aliasing -> nt flags
        dbg_pass(cfg.debug, fn, "trapelim", c);
        trapelim_ran = true;
        ch |= c;
        normalize_phis(fn);
      }
#ifndef MJZ_DBG_NO_LICM
      if (cfg.p_licm) {
        An A = analyze(fn);
        c = opt_licm(fn, A, os); // hoist invariants to the preheader
        dbg_pass(cfg.debug, fn, "licm", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
#ifndef MJZ_DBG_NO_DIVMAGIC
      if (cfg.p_divmagic) {
        c = opt_divmagic(fn, os); // /constant -> mulhi+shift
        dbg_pass(cfg.debug, fn, "divmagic", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
#ifndef MJZ_DBG_NO_REASSOC
      if (cfg.p_reassoc) {
        c = opt_reassoc(fn, os); // canonicalize +/-/*c chains (feeds LCF)
        dbg_pass(cfg.debug, fn, "reassoc", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
      if (cfg.p_ifconv) { // provably-binary cbr selects -> arithmetic
        c = opt_ifconv(fn, os);
        dbg_pass(cfg.debug, fn, "ifconv", c);
        ch |= c;
        normalize_phis(fn);
      }
      if (!ch) break;
      any = true;
      tier0();
    }
  }
  // ---- tier 2: expensive CFG restructuring, only after 0/1 fixpoint -------
  if (cfg.level >= 3) {
    uint32_t t2round = 0;
    for (uint32_t outer = 0; outer < cap; ++outer) {
      bool ch = false;
      ++t2round;
#ifndef MJZ_DBG_NO_UNROLL
      if (cfg.p_unroll && cfg.unroll) {
        An A = analyze(fn);
        bool c = opt_unroll(fn, A, os, cfg.unroll);
        dbg_pass(cfg.debug, fn, "unroll", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
#ifndef MJZ_DBG_NO_LCF
      if (cfg.p_lcf) {
        An A2 = analyze(fn); // unrolling may have reshaped the CFG
        bool c = opt_lcf(fn, A2, os); // closed forms / simulators
        dbg_pass(cfg.debug, fn, "lcf", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
      if (cfg.p_cdfold) { // TCE countdown loops -> exact closed form
        An A3 = analyze(fn);
        bool c = opt_cdfold(fn, A3, os);
        dbg_pass(cfg.debug, fn, "cdfold", c);
        ch |= c;
        normalize_phis(fn);
      }
      if (!ch) break;
      any = true;
      tier0();
      // one tier-1 round: restructuring exposes new opportunities
      if (cfg.level >= 2) {
        bool c2 = false;
        if (cfg.p_mem2reg) {
          An A = analyze(fn);
          c2 |= opt_mem2reg(fn, A, os, cfg.lift_ok, cfg.stkarr_ok);
          normalize_phis(fn);
        }
        if (cfg.p_trapelim && t2round < 2) { // restructuring-exposed proofs
          An A = analyze(fn);
          c2 |= opt_trapelim(fn, A, os);
          normalize_phis(fn);
        }
        if (cfg.p_licm) {
          An A = analyze(fn);
          c2 |= opt_licm(fn, A, os);
          normalize_phis(fn);
        }
        if (cfg.p_divmagic) {
          c2 |= opt_divmagic(fn, os);
          normalize_phis(fn);
        }
        if (cfg.p_reassoc) {
          c2 |= opt_reassoc(fn, os);
          normalize_phis(fn);
        }
        if (cfg.p_cdfold) {
          An A3 = analyze(fn);
          c2 |= opt_cdfold(fn, A3, os);
          normalize_phis(fn);
        }
        if (cfg.p_ifconv) {
          c2 |= opt_ifconv(fn, os);
          normalize_phis(fn);
        }
        (void)c2;
        tier0();
      }
    }
  }
  normalize_phis(fn);
  final_an = analyze(fn);
  return any;
}

// --- 9f2. stack-allocation analysis (for the C backend) ---------------------
// An `array(n)` with constant n whose handle only ever flows (through Copies)
// to Load/Store/Len as the HANDLE operand — never into a phi (an older
// iteration's array could otherwise be observed after the same instruction
// re-allocated its slot), never into arithmetic/comparisons/print/return/call
// arguments, never stored as a value (array-of-arrays) — is provably local:
// its handle value is unobservable, so the C backend can keep it in a stack
// array (zero-filled at the allocation point, exactly like the arena).
// Handles ARE ordinary values: any other use makes the numeric value
// observable and pins the array to the arena (indices must match every
// other runner). Budget: per array <= 16384 elements and <= 1 MiB total,
// to keep native frames far below the default 8 MiB stack.
inline constexpr val_t MJZ_STK_ARR_MAX_ELEMS = 16384;
inline constexpr val_t MJZ_STK_ARR_TOTAL_ELEMS = 131072;
constexpr void analyze_stack_allocs(Fn &fn) {
  fn.stk_arrays.clear();
  const uint32_t nb = fn.nblocks();
  if (!nb) return;
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nv) return;
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
  val_t total = 0;
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k != K::Alloc) continue;
      if (I.a == NONE || I.a >= nv || !kc[I.a]) continue; // runtime length
      val_t len = kv[I.a];
      if (len < 0 || len == 0 || len > MJZ_STK_ARR_MAX_ELEMS) continue;
      if (total + len > MJZ_STK_ARR_TOTAL_ELEMS) continue;
      // alias closure: the alloc dst + transitive Copy destinations
      std::vector<char> ali(nv, 0);
      ali[I.dst] = 1;
      for (bool grew = true; grew;) {
        grew = false;
        for (const Instr &C : fn.code)
          if (C.k == K::Copy && C.a != NONE && C.a < nv && C.dst < nv &&
              ali[C.a] && !ali[C.dst]) {
            ali[C.dst] = 1;
            grew = true;
          }
      }
      // escape scan over the FINAL alias set
      bool esc = false;
      for (uint32_t b2 = 0; b2 < nb && !esc; ++b2)
        for (uint32_t j = fn.blocks[b2].begin; j < fn.blocks[b2].end && !esc;
             ++j) {
          const Instr &U = fn.code[j];
          auto bad = [&](uint32_t v) { return v != NONE && v < nv && ali[v]; };
          switch (U.k) {
          case K::Copy: // source: alias extension (ok); dst cannot re-define
            break;
          case K::Load: // handle position ok; the INDEX is a value use
            if (bad(U.b2)) esc = true;
            break;
          case K::Store: // handle ok; index and stored value are value uses
            if (bad(U.b2) || bad(U.c2)) esc = true;
            break;
          case K::Len: // handle position ok
            break;
          default: // Bin/Cbr/Print/Call/Ret/Phi/Exit/Input: any alias use
            if (bad(U.a) || bad(U.b2) || bad(U.c2)) esc = true;
            for (uint32_t v : U.args)
              if (bad(v)) esc = true;
            break;
          }
        }
      if (esc) continue;
      total += len;
      Fn::StkArr sa{};
      sa.root = I.dst;
      sa.len = len;
      for (uint32_t v = 0; v < nv; ++v)
        if (ali[v]) sa.vregs.push_back(v);
      fn.stk_arrays.push_back(std::move(sa));
    }
}

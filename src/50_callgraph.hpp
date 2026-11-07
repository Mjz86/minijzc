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
//                       at compile time, recursive fns included.
//   interprocedural (optimize_prog, before the inliner rounds): scctco
//                       collapses every SCC whose internal edges are all
//                       tail calls into loops (see 50b_scctco.hpp)
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
  bool p_unroll{true}, p_lcf{true}, p_constcall{true};
  bool p_scctco{true}; // SCC tail-call collapse (inner-SCC tail calls)
  // interprocedural
  bool do_inline{true};
  bool do_opt{true}; // false: skip the optimizer entirely
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
    p_unroll = p_lcf = p_constcall = t2;
    p_scctco = t2;
    do_inline = t2;
    do_opt = lv != 0;
  }
};

constexpr bool optimize(Fn &fn, OptStats &os, An &final_an,
                        PassCfg cfg = {}); // (defined below)

// 9e1b. SCC tail-call optimization (defined in 50b_scctco.hpp): collapses
// inner-SCC tail calls into loops before the inliner rounds below -- the
// collapsed members are no longer recursive, so those rounds can inline
// them into their callers like any other small function.
constexpr bool opt_scc_tailcall(std::vector<Fn> &fns,
                                std::vector<OptStats> &ostats,
                                const std::vector<std::string_view> &names,
                                PassCfg cfg);

// --- 9e2. interprocedural: call graph, SCC (mjz_lib), inlining ---------------
// Nodes are functions (index 0 = the main body); edges are call sites. SCCs
// come from mjz::graph_ns::calculate_strongly_connected_components (Tarjan):
//   * a function in a multi-member SCC, or with a self-edge, is RECURSIVE
//     and is never inlined (inlining it would duplicate unbounded work and
//     break the SSA shape)
//   * Tarjan finalizes an SCC only after every SCC it can reach, so walking
//     SCCs in emission order processes callees before callers: inlined
//     bodies propagate through the rounds
struct CallGraph {
  std::vector<std::vector<ulen>> adj{}; // caller -> callees (with dups)
  std::vector<uint32_t> scc_of{};       // fn -> scc index
  std::vector<uint32_t> scc_begin{};    // scc -> start in members
  std::vector<uint32_t> members{};      // concatenated, scc by scc
  uint32_t nscc{};
  std::vector<char> recursive{};
};

constexpr CallGraph build_callgraph(const std::vector<Fn> &fns) {
  CallGraph G{};
  const uint32_t n = (uint32_t)fns.size();
  G.adj.assign(n, {});
  for (uint32_t f = 0; f < n; ++f)
    for (const Instr &I : fns[f].code)
      if (I.k == K::Call) {
        uint32_t c = (uint32_t)I.c;
        if (c < n) G.adj[f].push_back(c);
      }
  auto fs = gns::make_basic_forest<V>(G.adj);
  auto res = gns::calculate_strongly_connected_components<V>(fs);
  G.nscc = (uint32_t)res.nodes_index.size();
  G.members.assign(res.edges.begin(), res.edges.end());
  G.scc_begin.assign(G.nscc, 0);
  for (uint32_t s = 0; s < G.nscc; ++s)
    G.scc_begin[s] = (uint32_t)res.nodes_index[s];
  G.scc_of.assign(n, NONE);
  for (uint32_t s = 0; s < G.nscc; ++s) {
    uint32_t e = (s + 1 < G.nscc) ? G.scc_begin[s + 1]
                                  : (uint32_t)G.members.size();
    for (uint32_t j = G.scc_begin[s]; j < e; ++j)
      G.scc_of[G.members[j]] = s;
  }
  G.recursive.assign(n, 0);
  for (uint32_t f = 0; f < n; ++f) {
    if (G.scc_of[f] == NONE) continue;
    uint32_t s = G.scc_of[f];
    uint32_t e = (s + 1 < G.nscc) ? G.scc_begin[s + 1]
                                  : (uint32_t)G.members.size();
    if (e - G.scc_begin[s] > 1) G.recursive[f] = 1; // mutual recursion
    for (ulen c : G.adj[f])
      if ((uint32_t)c == f) G.recursive[f] = 1;     // self recursion
  }
  return G;
}

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
  auto ret_target = [&](uint32_t j) -> uint32_t {
    if (R == 1) return M;
    return (j == 0) ? (nbF + nbG) : (nbF + nbG + j - 1);
  };
  for (uint32_t g = 0; g < nbG; ++g) {
    uint32_t base = (uint32_t)nc.size();
    uint32_t ridx = NONE;
    for (uint32_t j = 0; j < R; ++j)
      if (rets[j].first == g) ridx = j;
    for (uint32_t i = G.blocks[g].begin; i < G.blocks[g].end; ++i) {
      const Instr &I = G.code[i];
      if (i + 1 == G.blocks[g].end && I.k == K::Ret) {
        nc.push_back({.k = K::Br, .t = ret_target(ridx)});
        continue;
      }
      Instr c = I;
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
  for (uint32_t f = 0; f < fns.size(); ++f) {
    An A{};
    PassCfg c1 = cfg;
    if (f < names.size()) c1.dbg_name = names[f];
    optimize(fns[f], ostats[f], A, c1);
  }
  if (fns.size() < 2) return;
  // ---- 9e1b: SCC tail-call collapse (before the inliner rounds) --------
  if (cfg.p_scctco)
    (void)opt_scc_tailcall(fns, ostats, names, cfg);
  if (cfg.do_inline) {
    for (int round = 0; round < 4; ++round) {
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
              // a body with a back edge (a LOOP) is not "tiny" however few
              // instrs it has: duplicating loop-carrying code into many call
              // sites multiplies the caller's analysis cost (and scctco
              // collapses tail recursion into exactly such loop bodies).
              // Single-use bodies still inline regardless of loops.
              bool has_back = false;
              for (uint32_t q = 0; q < fns[g].nblocks() && !has_back; ++q)
                for (uint32_t s : {fns[g].succ0(q), fns[g].succ1(q)})
                  if (s != NONE && s <= q) {
                    has_back = true;
                    break;
                  }
              bool small = sz <= 30 && !has_back;      // always inline tiny
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
          { // re-fold this caller; unroll/lcf wait for the final pass
            An A2{};
            PassCfg c2 = cfg;
            c2.p_unroll = false;
            optimize(fns[f], ostats[f], A2, c2);
          }
        }
      }
    }
    if (!inlined_any) break;
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
        c5.level = 2;
        c5.p_unroll = false;
        c5.p_lcf = false;
        c5.p_constcall = false;
        c5.do_inline = false;
        if (f < names.size()) c5.dbg_name = names[f];
        optimize(fns[f], ostats[f], A, c5);
      }
    }
  }
  eliminate_dead_fns(fns, names, ostats, src_index);
  for (uint32_t f = 0; f < fns.size(); ++f) {
    An A{};
    PassCfg c3 = cfg;
    if (f < names.size()) c3.dbg_name = names[f];
    optimize(fns[f], ostats[f], A, c3);
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

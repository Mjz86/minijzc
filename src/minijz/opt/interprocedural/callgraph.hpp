#pragma once
// === callgraph.hpp ========================================
// call graph + SCC (mjz_lib Tarjan) + the inliner (9e2) + the
// interprocedural driver hook (9e).
//
// THEORY (SCCs as the scheduling order, and the splice itself):
//   * THE CALL GRAPH. adj: caller -> callees (with duplicates:
//   each call site is an edge). Tarjan's SCC algorithm (mjz_lib)
//   partitions it into strongly connected components; a
//   single-member SCC with a self-edge is the recursive one
//   (G.recursive). Everything downstream -- scctco, tailrec,
//   deadargs, the inliner -- consumes scc_of/members, never the
//   raw edges.
//   * EMISSION-ORDER PROCESSING: SCCs are walked in emission
//   order, which processes callees before callers -- so when a
//   caller inlines a body, that body is already fully optimized,
//   and the inlined copies land in their final shape. The
//   re-optimization after each inlined round then folds constant
//   arguments THROUGH the inlined body (9e's rounds).
//   * THE SPLICE (opt_inline_one): the callee's blocks are
//   renumbered into the caller; the caller's block splits at the
//   call site -- the call instr's position becomes an unconditional
//   edge into the callee's entry, and the callee's Ret edges
//   become branches to the resume block, where the result vreg is
//   the call's dst. Fresh vregs for every copied value keep SSA
//   (the unroll naming trick again). K::Depth inside the copy
//   would read the CALLER's depth, so it is adjusted: dadj =
//   dm + 1 is materialized at the inline point (the 512-frame
//   budget must stay exact). Main is never inlined (it would
//   duplicate unbounded work and has nowhere to return to).
//   * ORDER IN 9e: scctco (9e1b) runs BEFORE the inliner rounds --
//   collapsing inner-SCC tail calls de-recursiveves the members,
//   so the inliner can then inline them; tiny constant-index
//   readers are cloned BEFORE that (fnspec) so the always-inline
//   rule cannot bury them (the ordering note at the 9e body).
//   Dead functions (unreachable from main) are swept at the end.

#include "../../base/pass_cfg.hpp"
#include "../../graph_lib.hpp"
#include "../../ir/instr.hpp"
#include "../analysis/an.hpp"
#include "../scalar/dce.hpp"
#include "argpromo.hpp"
#include "callcse.hpp"
#include "constcall.hpp"
#include "deadargs.hpp"
#include "fnspec.hpp"
#include "icf.hpp"

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
        if (c < n)
          G.adj[f].push_back(c);
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
    uint32_t e =
        (s + 1 < G.nscc) ? G.scc_begin[s + 1] : (uint32_t)G.members.size();
    for (uint32_t j = G.scc_begin[s]; j < e; ++j)
      G.scc_of[G.members[j]] = s;
  }
  G.recursive.assign(n, 0);
  for (uint32_t f = 0; f < n; ++f) {
    if (G.scc_of[f] == NONE)
      continue;
    uint32_t s = G.scc_of[f];
    uint32_t e =
        (s + 1 < G.nscc) ? G.scc_begin[s + 1] : (uint32_t)G.members.size();
    if (e - G.scc_begin[s] > 1)
      G.recursive[f] = 1; // mutual recursion
    for (ulen c : G.adj[f])
      if ((uint32_t)c == f)
        G.recursive[f] = 1; // self recursion
  }
  return G;
}

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

// 9e4. tail-call elimination (defined in 50c_tco.hpp): runs AFTER the
// inliner rounds and catches the SCCs scctco declined -- self tail
// recursion becomes a budget-exact loop, pure-tail SCCs merge into one
// sel-dispatched leaf that cdfold (50d) then folds to closed forms.
constexpr bool opt_tailrec(std::vector<Fn> &fns,
                           std::vector<std::string_view> &names,
                           std::vector<OptStats> &ostats,
                           std::vector<uint32_t> &src_index);

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
  if (ci >= F.code.size())
    return false;
  const Instr call = F.code[ci]; // value copy: the vector is rebuilt below
  if (call.k != K::Call || call.dst == NONE)
    return false;
  if (call.args.size() != G.nparams)
    return false;
  const uint32_t nbF = F.nblocks(), nbG = G.nblocks();
  const uint32_t nvG = G.nvars + (uint32_t)G.version_base.size();
  // G's entry must be phi-free (entry blocks never have phis; paranoia)
  for (uint32_t i = G.blocks[0].begin; i < G.blocks[0].end; ++i)
    if (G.code[i].k == K::Phi)
      return false;
  // locate the caller block holding the call
  uint32_t B = NONE;
  for (uint32_t b = 0; b < nbF; ++b)
    if (ci >= F.blocks[b].begin && ci < F.blocks[b].end) {
      B = b;
      break;
    }
  if (B == NONE)
    return false;
  const uint32_t Bs0 = F.succ[2 * B], Bs1 = F.succ[2 * B + 1];
  // vreg map: params -> caller args; all other callee vregs -> fresh ids
  std::vector<uint32_t> vmap(nvG, NONE);
  for (uint32_t v = 0; v < nvG; ++v) {
    if (v < G.nparams)
      vmap[v] = call.args[v];
    else
      vmap[v] = new_anon_vreg(F, G.base_of(v));
  }
  auto mapv = [&](uint32_t v) -> uint32_t {
    if (v == NONE || v >= nvG)
      return v;
    return vmap[v] != NONE ? vmap[v] : v;
  };
  // block map: fresh ids appended to F
  std::vector<uint32_t> bmap(nbG, NONE);
  for (uint32_t g = 0; g < nbG; ++g)
    bmap[g] = nbF + g;
  // return sites: (block, value)
  std::vector<std::pair<uint32_t, uint32_t>> rets;
  for (uint32_t g = 0; g < nbG; ++g)
    for (uint32_t i = G.blocks[g].begin; i < G.blocks[g].end; ++i)
      if (G.code[i].k == K::Ret && i + 1 == G.blocks[g].end)
        rets.push_back({g, G.code[i].a});
  if (rets.empty())
    return false;
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
      for (uint32_t i = F.blocks[b].begin; i < ci; ++i)
        nc.push_back(F.code[i]);
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
    if (I.k == K::Depth)
      G_has_depth = true;
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
    if (R == 1)
      return M;
    return (j == 0) ? (nbF + nbG) : (nbF + nbG + j - 1);
  };
  for (uint32_t g = 0; g < nbG; ++g) {
    uint32_t base = (uint32_t)nc.size();
    uint32_t ridx = NONE;
    for (uint32_t j = 0; j < R; ++j)
      if (rets[j].first == g)
        ridx = j;
    if (g == 0 && !depth_pre.empty()) { // the Depth adjustment prefix lives
      for (const Instr &dp : depth_pre)
        nc.push_back(dp); // INSIDE this block
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
      if (c.dst != NONE)
        c.dst = mapv(c.dst);
      c.a = mapv(c.a);
      c.b2 = mapv(c.b2);
      if (c.c2 != NONE)
        c.c2 = mapv(c.c2);
      for (uint32_t &av : c.args)
        av = mapv(av);
      if (c.k == K::Phi) {
        if (c.t != NONE && c.t < nbG)
          c.t = bmap[c.t];
        if (c.f != NONE && c.f < nbG)
          c.f = bmap[c.f];
      } else if (c.k == K::Br) {
        if (c.t < nbG)
          c.t = bmap[c.t];
      } else if (c.k == K::Cbr) {
        if (c.t < nbG)
          c.t = bmap[c.t];
        if (c.f < nbG)
          c.f = bmap[c.f];
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
    if (s == NONE || s >= nbF)
      continue;
    for (uint32_t i = nblocks[s].begin; i < nblocks[s].end; ++i) {
      Instr &I = nc[i];
      if (I.k != K::Phi)
        break;
      if (I.t == B)
        I.t = M;
      if (I.f == B)
        I.f = M;
    }
  }
  // 6) commit
  F.code = std::move(nc);
  F.blocks = std::move(nblocks);
  F.succ = std::move(nsucc);
  // 7) uses of the call result -> merged value
  for (Instr &I : F.code) {
    if (I.a == call.dst)
      I.a = result_val;
    if (I.b2 == call.dst)
      I.b2 = result_val;
    if (I.c2 == call.dst)
      I.c2 = result_val;
    for (uint32_t &av : I.args)
      if (av == call.dst)
        av = result_val;
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
    for (uint32_t f = 1; f < n; ++f)
      any_dead |= !reach[f];
    if (!any_dead)
      return;
    std::vector<uint32_t> remap(n, NONE);
    std::vector<Fn> nf;
    std::vector<std::string_view> nn;
    std::vector<OptStats> no;
    std::vector<uint32_t> nsrc;
    for (uint32_t f = 0; f < n; ++f) {
      if (!reach[f])
        continue;
      remap[f] = (uint32_t)nf.size();
      nf.push_back(std::move(fns[f]));
      nn.push_back(names[f]);
      no.push_back(ostats[f]);
      nsrc.push_back(src_index[f]);
    }
    for (Fn &F : nf)
      for (Instr &I : F.code)
        if (I.k == K::Call)
          I.c = remap[(uint32_t)I.c];
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
    if (f < names.size())
      c.dbg_name = names[f];
    return c;
  };
  for (uint32_t f = 0; f < fns.size(); ++f) {
    An A{};
    optimize(fns[f], ostats[f], A, main_aware(cfg, f));
  }
  if (fns.size() < 2)
    return;
  // ---- 9e1b: SCC tail-call collapse (before the inliner rounds) --------
  if (cfg.p_scctco)
    (void)opt_scc_tailcall(fns, ostats, names, cfg);
  // ---- 9e7: argument promotion --------------------------------------------
  // BEFORE the inliner rounds: a tiny constant-index reader would otherwise
  // be inlined first (the inliner's "always inline tiny" rule) and never
  // seen as a promotable shape. A promoted parameter becomes k scalar
  // element params; the callee's Loads become plain param uses; every
  // promoted site materializes the (provably in-bounds) element loads right
  // before the call. The tiny CLONE then feeds the inliner as usual -- with
  // scalar args it inlines cleanly and the caller's loads lift + const-fold;
  // arrays that stop escaping lift in the re-fold rounds below, and
  // constcall evaluates the promoted call at compile time when the elements
  // fold to constants. The dead-fn sweep drops generics that lost all their
  // sites to the clone.
  if (cfg.p_argpromo) {
    if (opt_argpromo(fns, names, ostats, src_index))
      eliminate_dead_fns(fns, names, ostats, src_index);
  }
  // ---- interprocedural inlining rounds (SCC-aware; recursive never) -------
  auto inline_rounds = [&](int rounds) {
    if (!cfg.do_inline)
      return;
    for (int round = 0; round < rounds; ++round) {
      CallGraph G = build_callgraph(fns);
      auto recount = [&]() {
        std::vector<uint32_t> cs(fns.size(), 0);
        for (uint32_t f = 0; f < fns.size(); ++f)
          for (const Instr &I : fns[f].code)
            if (I.k == K::Call) {
              uint32_t c = (uint32_t)I.c;
              if (c < cs.size())
                cs[c]++;
            }
        return cs;
      };
      std::vector<uint32_t> callsites = recount();
      bool inlined_any = false;
      for (uint32_t s = 0; s < G.nscc; ++s) {
        uint32_t e =
            (s + 1 < G.nscc) ? G.scc_begin[s + 1] : (uint32_t)G.members.size();
        for (uint32_t j = G.scc_begin[s]; j < e; ++j) {
          uint32_t f = G.members[j];
          for (;;) { // rescan after each inline: the fn changed shape
            uint32_t site = NONE, callee = NONE;
            for (uint32_t b = 0; b < fns[f].nblocks() && site == NONE; ++b)
              for (uint32_t i = fns[f].blocks[b].begin;
                   i < fns[f].blocks[b].end; ++i) {
                const Instr &I = fns[f].code[i];
                if (I.k != K::Call)
                  continue;
                uint32_t g = (uint32_t)I.c;
                if (g == 0 || g >= fns.size())
                  continue; // never inline main
                if (G.recursive[g])
                  continue; // SCC: recursive
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
                bool small = sz <= 30 && !has_back; // always inline tiny
                bool single_use = callsites[g] == 1 && sz <= 300;
                if (!small && !single_use)
                  continue;
                if (fns[g].nparams != I.args.size())
                  continue;
                if (fns[f].code.size() + sz > 8000)
                  continue; // growth cap
                site = i;
                callee = g;
                break;
              }
            if (site == NONE)
              break;
            if (!opt_inline_one(fns[f], fns[callee], site))
              break;
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
      if (!inlined_any)
        break;
    }
  };
  inline_rounds(4);
  // ---- 9e4: tail-call elimination of inner-SCC tail calls ------------------
  // After inlining (non-SCC helpers are gone, so what remains as calls is
  // genuine recursion — and anything scctco above declined): self
  // tail-recursion becomes a loop, pure-tail SCCs merge into one
  // sel-dispatched leaf function. Both keep the recursion depth cap EXACT
  // via the budget = CAP+1 - Depth() counter. The merged functions are
  // small leaves now, so a second wave of inlining can pull them (and
  // their countdown loops) into their callers.
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
          if (I.k == K::Depth) {
            tced = true;
            break;
          }
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
  // ---- 9e3b: pure-call CSE ---------------------------------------------
  // dedupe repeated calls to provably-pure functions: after the inliner
  // rounds, exactly the multi-use and recursive calls remain, and those
  // are the ones users ask twice (gcd(a,b) style shapes). Runs BEFORE
  // const-call so a pair where one site has constant arguments folds once.
  // The rewritten Copies are folded by the constcall re-fold below and the
  // final full optimize.
  if (cfg.p_callcse)
    (void)opt_callcse(fns, ostats);
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
      if (!opt_constcall(fns, ostats))
        break;
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
  // ---- 9e5: interprocedural dead-argument elimination -------------------
  // (after dead-fn elimination: all inlining/constcall/callcse are done, so
  // the surviving call sites are the final ones; before the last optimize
  // round, so callers whose dropped argument expressions turned dead get
  // DCE'd)
  if (cfg.p_deadargs)
    (void)opt_deadargs(fns, ostats);
  // ---- 9e5b: function specialization ------------------------------------
  // every-constant argument slot gets a specialized clone (the param bound
  // to a Const at entry); the dead-fn sweep right after drops generics that
  // lost all their sites. BEFORE the final round so the clones' bound
  // constants fold through tier 0 there.
  if (cfg.p_fnspec) {
    if (opt_fnspec(fns, names, ostats, src_index))
      eliminate_dead_fns(fns, names, ostats, src_index);
  }
  for (uint32_t f = 0; f < fns.size(); ++f) {
    An A{};
    optimize(fns[f], ostats[f], A, main_aware(cfg, f));
  }
  // ---- 9e6: identical code folding --------------------------------------
  // AFTER the final round: bodies are in final shape, so structural
  // identity == behavioral interchangeability (verified by the partition
  // refinement inside). Redirecting a call never changes a body, so no
  // re-optimization is needed; the sweep below deletes the duplicates.
  if (cfg.p_icf) {
    if (opt_icf(fns, ostats))
      eliminate_dead_fns(fns, names, ostats, src_index);
  }
}

// --- 9e. driver
// ---------------------------------------------------------------- runtime-only
// pass tracing (never on the constexpr path: guarded by cfg.debug, which the
// compile-time pipeline always leaves at 0)
inline void print_fn(const Fn &fn, const char *title,
                     std::string_view src); // (defined in prog/runners.hpp)
static void dbg_pass_rt(uint8_t debug, const Fn &fn, const char *pass,
                        bool ch) {
  std::printf("  [pass %-9s] %s\n", pass, ch ? "changed" : "fixed");
  if (debug >= 2 && ch) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "IR after %s", pass);
    print_fn(fn, buf, {}); // no source at hand here: no location comments
  }
}
// constexpr-safe wrapper: the compile-time pipeline always leaves debug at
// 0 and the hook null, so the early return keeps constant evaluation away
// from the printf tracer and the snapshot collector (an unreached branch
// in a constexpr function is legal)
constexpr void dbg_pass(const PassCfg &cfg, const Fn &fn, const char *pass,
                        bool ch) {
  if (cfg.snap_hook) // runtime-only: debugger `opt` / --opt-diff snapshots
    cfg.snap_hook(pass, ch, fn, cfg.snap_ctx);
  if (cfg.debug < 1)
    return;
  dbg_pass_rt(cfg.debug, fn, pass, ch);
}

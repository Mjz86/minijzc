#pragma once
// === astrun.hpp ========================================
// the AST oracle interpreter (the reference semantics).

#include "../back/emit_plan.hpp" // make_fuse_table (assign_slots)
#include "../front/ast.hpp"
#include "../ir/instr.hpp"

// ============================================================================
// 11. AST interpreter (the oracle)
// ============================================================================
struct AstRun {
  const Ast &a;
  RunOut R{};
  uint64_t steps{};
  uint64_t limit{};
  bool dead{};      // set on trap/timeout to short-circuit
  bool returned{};  // a 'return' completed: unwind this frame
  bool broke{};     // a 'break' completed: unwind to the enclosing loop
  bool continued{}; // a 'continue' completed: jump to the loop's re-test
  val_t retval{};   // ... with this value
  uint32_t depth{}; // call depth (capped: matches every other runner)

  constexpr AstRun(const Ast &a_, uint64_t lim = 200000) : a(a_), limit(lim) {}

  MJZ_RUNNER_FN bool eval(uint32_t ei, val_t &out); // body: astrun_impl.hpp
  std::vector<val_t> env{};
  MJZ_RUNNER_FN bool exec(uint32_t si); // body: astrun_impl.hpp
};
#ifdef MJZ_MULTI_CPP
RunOut run_ast(const Ast &a, uint32_t root, uint64_t limit = 200000,
               const std::vector<val_t> &ins = {}); // tu/runners.cpp
#else
constexpr RunOut run_ast(const Ast &a, uint32_t root, uint64_t limit = 200000,
                         const std::vector<val_t> &ins = {});
#include "astrun_impl.hpp"
#endif

// ============================================================================
// 11z. SLOT ALLOCATION — "register allocation" for the C + bytecode backends
// ============================================================================
// After SSA destruction every vreg behaves like a stack slot. This pass
// colors the interference graph so vregs with DISJOINT live ranges share one
// home: the C backend declares fewer locals (r0..r{K-1} instead of one
// variable per vreg) and the bytecode VM frames shrink (fewer slots per
// frame). Unlike the x86-64 backend there is NO spill machinery and no fixed
// register set: colors are unbounded (C has unlimited locals, the VM has
// unlimited slots), so greedy coloring is always feasible.
//   * liveness: per-block backward dataflow to a fixpoint (same discipline
//     as the x86 ra_plan). BRANCH FUSION AWARE: a Cbr whose comparison the
//     emission plan folds into the branch text reads the comparison's
//     OPERANDS at the branch, so their live ranges extend past the (now
//     dead) compare. Treating such operands as dying at the compare was
//     UNSOUND, not conservative: another vreg could legally share (and
//     clobber) the operand's slot in between — fuzz seed 33 hit exactly
//     that (a short-circuit gate compared stale slot values and consumed
//     an input() the source never evaluates). Const-inlining (kc) and dead
//     instrs, by contrast, only ever SHORTEN real live ranges, so ignoring
//     those stays conservative.
//   * interference: at each def, edges to everything live-after it; params
//     are "defined at entry", so they conflict with each other and with
//     everything live into the entry block.
//   * coloring: parameters are PINNED to slots 0..nparams-1 (the VM's Call
//     convention writes caller args into exactly these slots); every other
//     vreg takes the smallest color unused by its already-colored neighbors,
//     highest degree first (Chaitin-style heuristic -> few slots).
// With --no-regalloc (PassCfg::regalloc == false) this never runs and the
// backends emit the identity mapping — one named local per vreg ("SSA-like
// C", one VM slot per vreg).
constexpr void assign_slots(Fn &fn) {
  fn.slot_of.clear();
  fn.nslots = 0;
  const uint32_t nb = fn.nblocks();
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nb || !nv)
    return;
  // Branch-fusion awareness: the SHARED gate (back/emit_plan.hpp's
  // make_fuse_table -- the ckpt-31 dedup wave replaced this file's
  // hand-kept mirror of the plan's fusion decision with the call, so the
  // two can never drift again; same allocation profile: three nv-sized
  // vectors + one code-sized vector, keeping the constexpr pipeline's
  // 4 GiB evaluation budget undisturbed). A Cbr whose comparison will be
  // folded into the branch text is read by TWO kinds of backends and both
  // must stay sound:
  //  * the C backend folds the compare away and reads the comparison's
  //    OPERANDS at the branch — their live ranges extend past the (dead)
  //    compare. Missing this let another vreg reuse (clobber) the operand
  //    slots in between (fuzz seed 33: a short-circuit gate compared stale
  //    slots and consumed an input() the source never evaluates).
  //  * the bytecode VM does not fuse: it materializes the compare and
  //    branches on its RESULT — so I.a stays live to the branch as well.
  // Marking both (operands + result) is the conservative union; skipping
  // const-inlined operands would only shave slots, never correctness.
  const std::vector<uint32_t> fuse = make_fuse_table(fn, nv).fuse;
  auto each_use = [&](uint32_t i, auto &&f) {
    const Instr &I = fn.code[i];
    if (I.k == K::Cbr && i < fuse.size() && fuse[i] != NONE) {
      const Instr &D = fn.code[fuse[i]];
      auto one = [&](uint32_t v) {
        if (v != NONE && v < nv)
          f(v);
      };
      one(D.a);
      one(D.b2);
      one(I.a);
      return;
    }
    auto one = [&](uint32_t v) {
      if (v != NONE && v < nv)
        f(v);
    };
    for_each_operand(I, one); // the shared enumeration (instr.hpp)
  };
  auto is_def = [&](const Instr &I) -> uint32_t {
    if (I.dst == NONE || I.dst >= nv)
      return NONE;
    return I.dst;
  };
  // ---- liveness: block-granular backward dataflow to a fixpoint ----------
  std::vector<std::vector<char>> lin(nb, std::vector<char>(nv, 0)),
      lout(nb, std::vector<char>(nv, 0));
  for (uint32_t round = 0; round < 64; ++round) {
    bool changed = false;
    for (uint32_t bb = nb; bb-- > 0;) {
      std::vector<char> &lo = lout[bb];
      std::fill(lo.begin(), lo.end(), 0);
      for (uint32_t s : {fn.succ0(bb), fn.succ1(bb)})
        if (s != NONE && s < nb)
          for (uint32_t v = 0; v < nv; ++v)
            lo[v] |= lin[s][v];
      std::vector<char> live = lo;
      for (uint32_t i = fn.blocks[bb].end; i-- > fn.blocks[bb].begin;) {
        const Instr &I = fn.code[i];
        if (I.k == K::Phi)
          continue; // destroyed input: none (defensive)
        uint32_t d = is_def(I);
        if (d != NONE)
          live[d] = 0;
        each_use(i, [&](uint32_t v) { live[v] = 1; });
      }
      for (uint32_t v = 0; v < nv; ++v)
        if (live[v] != lin[bb][v]) {
          lin[bb][v] = live[v];
          changed = true;
        }
    }
    if (!changed)
      break;
  }
  // ---- interference edges -------------------------------------------------
  std::vector<std::vector<uint32_t>> adj(nv);
  auto edge = [&](uint32_t a, uint32_t b) {
    if (a == b || a >= nv || b >= nv)
      return;
    for (uint32_t x : adj[a])
      if (x == b)
        return;
    adj[a].push_back(b);
    adj[b].push_back(a);
  };
  { // parameters are defined at entry: conflict with each other and with
    // everything live into the entry block
    std::vector<char> live = lin[0];
    for (uint32_t p = 0; p < fn.nparams && p < nv; ++p) {
      for (uint32_t u = 0; u < nv; ++u)
        if (live[u] && u != p)
          edge(p, u);
      live[p] = 0;
    }
  }
  for (uint32_t b = 0; b < nb; ++b) {
    std::vector<char> live = lout[b];
    for (uint32_t i = fn.blocks[b].end; i-- > fn.blocks[b].begin;) {
      const Instr &I = fn.code[i];
      if (I.k == K::Phi)
        continue;
      uint32_t d = is_def(I);
      if (d != NONE) {
        for (uint32_t u = 0; u < nv; ++u)
          if (live[u])
            edge(d, u); // the write clobbers: no sharing here
        live[d] = 0;
      }
      each_use(i, [&](uint32_t v) { live[v] = 1; });
    }
  }
  // ---- coloring: params pinned, then greedy, highest degree first --------
  std::vector<int32_t> color(nv, -1);
  for (uint32_t p = 0; p < fn.nparams && p < nv; ++p)
    color[p] = (int32_t)p;
  std::vector<uint32_t> order{};
  order.reserve(nv);
  for (uint32_t v = 0; v < nv; ++v)
    order.push_back(v);
  // insertion sort by descending degree (stable, constexpr, nv is modest)
  for (size_t i = 1; i < order.size(); ++i)
    for (size_t j = i; j > 0 && adj[order[j - 1]].size() < adj[order[j]].size();
         --j)
      std::swap(order[j - 1], order[j]);
  std::vector<char> taken(nv, 0);
  for (uint32_t v : order) {
    if (color[v] >= 0)
      continue; // pinned param
    std::fill(taken.begin(), taken.end(), 0);
    for (uint32_t u : adj[v])
      if (color[u] >= 0 && (uint32_t)color[u] < nv)
        taken[color[u]] = 1;
    int32_t c = 0;
    while (c < (int32_t)nv && taken[c])
      ++c;
    color[v] = c;
  }
  uint32_t maxc = 0;
  for (uint32_t v = 0; v < nv; ++v)
    if (color[v] > (int32_t)maxc)
      maxc = (uint32_t)color[v];
  fn.slot_of.assign(nv, 0);
  for (uint32_t v = 0; v < nv; ++v)
    fn.slot_of[v] = (uint32_t)color[v];
  fn.nslots = maxc + 1;
  if (fn.nslots < fn.nparams)
    fn.nslots = fn.nparams; // params are pinned
}

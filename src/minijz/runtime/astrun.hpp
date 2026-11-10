#pragma once
// === astrun.hpp ========================================
// the AST oracle interpreter (the reference semantics).

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

  constexpr bool eval(uint32_t ei, val_t &out) {
    if (dead || ++steps > limit) {
      R.timeout = true;
      dead = true;
      out = 0;
      return false;
    }
    const Expr &e = a.es[ei];
    switch (e.k) {
    case Ek::Num:
      out = e.num;
      return true;
    case Ek::Var:
      out = env[e.var];
      return true;
    case Ek::Neg: {
      val_t x{};
      if (!eval(e.l, x))
        return false;
      out = w_sub(0, x).v;
      return true;
    }
    case Ek::Not: {
      val_t x{};
      if (!eval(e.l, x))
        return false;
      out = (x == 0) ? 1 : 0;
      return true;
    }
    case Ek::Bin: {
      val_t x{}, y{};
      if (!eval(e.l, x))
        return false;
      if (!eval(e.r, y))
        return false;
      Rv r{};
      switch (e.b) {
      case Bk::Add:
        r = w_add(x, y);
        break;
      case Bk::Sub:
        r = w_sub(x, y);
        break;
      case Bk::Mul:
        r = w_mul(x, y);
        break;
      case Bk::Div:
        r = w_div(x, y);
        break;
      case Bk::Mod:
        r = w_mod(x, y);
        break;
      case Bk::Lt:
        r = {x < y, false};
        break;
      case Bk::Le:
        r = {x <= y, false};
        break;
      case Bk::Gt:
        r = {x > y, false};
        break;
      case Bk::Ge:
        r = {x >= y, false};
        break;
      case Bk::Eq:
        r = {x == y, false};
        break;
      case Bk::Ne:
        r = {x != y, false};
        break;
      }
      if (r.trap) {
        R.trapped = true;
        R.trap_src = e.pos;
        R.trap_kind = "division by zero";
        dead = true;
        out = 0;
        return false;
      }
      out = r.v;
      return true;
    }
    case Ek::And: {
      val_t x{};
      if (!eval(e.l, x))
        return false;
      if (x == 0) {
        out = 0;
        return true;
      } // short-circuit
      val_t y{};
      if (!eval(e.r, y))
        return false;
      out = (y != 0) ? 1 : 0;
      return true;
    }
    case Ek::Or: {
      val_t x{};
      if (!eval(e.l, x))
        return false;
      if (x != 0) {
        out = 1;
        return true;
      } // short-circuit
      val_t y{};
      if (!eval(e.r, y))
        return false;
      out = (y != 0) ? 1 : 0;
      return true;
    }
    case Ek::Alloc: {
      val_t n{};
      if (!eval(e.l, n))
        return false;
      Rv r = R.arena_alloc(n);
      if (r.trap) {
        R.trapped = true;
        R.trap_src = e.pos;
        R.trap_kind = "invalid array size";
        dead = true;
        out = 0;
        return false;
      }
      out = r.v;
      return true;
    }
    case Ek::Load: {
      val_t h{}, i{};
      if (!eval(e.l, h))
        return false;
      if (!eval(e.r, i))
        return false;
      Rv r = R.arena_load(h, i);
      if (r.trap) {
        R.trapped = true;
        R.trap_src = e.pos;
        R.trap_kind = "index out of bounds";
        dead = true;
        out = 0;
        return false;
      }
      out = r.v;
      return true;
    }
    case Ek::Len: {
      val_t h{};
      if (!eval(e.l, h))
        return false;
      out = R.arena_len(h);
      return true;
    }
    case Ek::Input:
      out = R.next_input();
      return true;
    case Ek::Call: { // args left to right, then a fresh frame for the body
      const FnDef &fd = a.fns[e.var];
      std::vector<val_t> argv{};
      for (uint32_t ai : a.seq_kids[e.l]) {
        val_t v{};
        if (!eval(ai, v))
          return false;
        argv.push_back(v);
      }
      if (depth >= MJZ_MAX_CALL_DEPTH) {
        R.trapped = true;
        R.trap_src = e.pos;
        R.trap_kind = "recursion depth exceeded";
        dead = true;
        out = 0;
        return false;
      }
      std::vector<val_t> saved = std::move(env);
      env.assign(a.names.size(), 0);
      for (size_t p = 0; p < fd.params.size() && p < argv.size(); ++p)
        env[fd.params[p]] = argv[p];
      ++depth;
      bool was = returned;
      returned = false;
      retval = 0; // falling off the end returns 0 -- NOT the previous
                  // frame's leftover return value (empty fn bodies!)
      exec(fd.body);
      --depth;
      val_t rv = retval;
      env = std::move(saved);
      returned = was;
      if (dead) {
        out = 0;
        return false;
      }
      out = rv;
      return true;
    }
    }
    out = 0;
    return false;
  }
  std::vector<val_t> env{};
  constexpr bool exec(uint32_t si) {
    if (dead)
      return false;
    if (++steps > limit) {
      R.timeout = true;
      dead = true;
      return false;
    }
    const Stmt &s = a.ss[si];
    switch (s.k) {
    case Sk::Let:
    case Sk::Assign: {
      val_t v{};
      if (!eval(s.e, v))
        return false;
      env[s.var] = v;
      return true;
    }
    case Sk::Print: {
      val_t v{};
      if (!eval(s.e, v))
        return false;
      R.out.push_back(v);
      return true;
    }
    case Sk::Ret: {
      val_t v{};
      if (!eval(s.e, v))
        return false;
      retval = v;
      returned = true;
      return true; // frame finished; callers see `returned` and unwind
    }
    case Sk::Seq:
      for (uint32_t x : a.seq_kids[s.a]) {
        if (!exec(x))
          return false;
        if (returned || broke || continued)
          return true;
      }
      return true;
    case Sk::StoreA: { // a[i1]...[ik] = value (indices, then loads, then value)
      const auto &idx = a.seq_kids[s.a];
      val_t h = env[s.var];
      for (size_t j = 0; j + 1 < idx.size(); ++j) {
        val_t i{};
        if (!eval(idx[j], i))
          return false;
        Rv r = R.arena_load(h, i);
        if (r.trap) {
          R.trapped = true;
          R.trap_src = s.pos;
          dead = true;
          return false;
        }
        h = r.v;
      }
      val_t last{};
      if (!eval(idx.back(), last))
        return false;
      val_t v{};
      if (!eval(s.b, v))
        return false;
      if (!R.arena_store(h, last, v)) {
        R.trapped = true;
        R.trap_src = s.pos;
        R.trap_kind = "index out of bounds";
        dead = true;
        return false;
      }
      return true;
    }
    case Sk::If: {
      val_t c{};
      if (!eval(s.e, c))
        return false;
      return exec(c != 0 ? s.a : s.b);
    }
    case Sk::Brk:
      broke = true;
      return true; // the enclosing While consumes the flag
    case Sk::Cont:
      continued = true;
      return true;
    case Sk::Bkpt:
      // observation point: no effect on the oracle's output (the debugger
      // is the only consumer; every runner agrees it is a no-op)
      return true;
    case Sk::While: {
      for (;;) {
        val_t c{};
        if (!eval(s.e, c))
          return false;
        if (c == 0)
          return true;
        if (!exec(s.a))
          return false;
        if (returned)
          return true;
        if (broke) {
          broke = false;
          return true;
        }
        if (continued) {
          continued = false; /* re-test */
        }
      }
    }
    }
    return false;
  }
};
constexpr RunOut run_ast(const Ast &a, uint32_t root, uint64_t limit = 200000,
                         const std::vector<val_t> &ins = {}) {
  AstRun ar(a, limit);
  ar.R.ins = ins;
  ar.env.assign(a.names.size(), 0);
  ar.exec(root);
  return ar.R;
}

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
  // Branch-fusion awareness (mirrors make_emit_plan's fusion decision
  // EXACTLY, kept local and allocation-lean: three nv-sized vectors + one
  // code-sized vector, so the constexpr pipeline's 4 GiB evaluation budget
  // is not disturbed by the full plan's extra vectors). A Cbr whose
  // comparison will be folded into the branch text is read by TWO kinds of
  // backends and both must stay sound:
  //  * the C backend folds the compare away and reads the comparison's
  //    OPERANDS at the branch — their live ranges extend past the (dead)
  //    compare. Missing this let another vreg reuse (clobber) the operand
  //    slots in between (fuzz seed 33: a short-circuit gate compared stale
  //    slots and consumed an input() the source never evaluates).
  //  * the bytecode VM does not fuse: it materializes the compare and
  //    branches on its RESULT — so I.a stays live to the branch as well.
  // Marking both (operands + result) is the conservative union; skipping
  // const-inlined operands would only shave slots, never correctness.
  std::vector<uint32_t> raw(nv, 0), def_idx(nv, NONE), ndefs(nv, 0);
  std::vector<uint32_t> fuse(fn.code.size(), NONE);
  for (uint32_t i = 0; i < fn.code.size(); ++i) {
    const Instr &I = fn.code[i];
    if (I.a != NONE && I.a < nv)
      raw[I.a]++;
    if (I.b2 != NONE && I.b2 < nv)
      raw[I.b2]++;
    if (I.c2 != NONE && I.c2 < nv)
      raw[I.c2]++;
    for (uint32_t v : I.args)
      if (v < nv)
        raw[v]++;
    if (I.dst != NONE && I.dst < nv) {
      ++ndefs[I.dst];
      def_idx[I.dst] = i;
    }
  }
  for (uint32_t i = 0; i < fn.code.size(); ++i) {
    const Instr &I = fn.code[i];
    if (I.k != K::Cbr || I.a == NONE || I.a >= nv)
      continue;
    uint32_t d = def_idx[I.a];
    // single USE is not enough post-destruction (phi slots have one use but
    // per-edge defs); require a single DEF — same condition as the plan
    if (d == NONE || raw[I.a] != 1 || ndefs[I.a] != 1)
      continue;
    const Instr &D = fn.code[d];
    bool is_cmp =
        D.k == K::Bin && (D.b == B::Lt || D.b == B::Le || D.b == B::Gt ||
                          D.b == B::Ge || D.b == B::Eq || D.b == B::Ne);
    if (is_cmp)
      fuse[i] = d;
  }
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
    one(I.a);
    one(I.b2);
    one(I.c2);
    for (uint32_t v : I.args)
      one(v);
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

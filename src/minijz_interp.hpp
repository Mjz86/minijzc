// ============================================================================
// MiniJZ compiler — §10 SSA interpreter, §11 AST interpreter (the oracle)
// ----------------------------------------------------------------------------
// Ordered-include header: this file is a slice of the original single
// translation unit and relies on being included after its predecessors
// (see src/minijz.hpp for the canonical order). Do not include it
// directly in an arbitrary order.
// ============================================================================
#pragma once

// 10. SSA interpreter (phi resolution by pred identity via t/f)
// ============================================================================
// program-level SSA interpreter with an explicit frame stack: Call pushes a
// frame (params bound from the operand-side argument vregs), Ret pops back
// to the caller's resume point and stores the value into the result slot.
constexpr RunOut run_prog(const std::vector<Fn> &fns,
                          uint64_t step_limit = 200000,
                          const std::vector<val_t> &ins = {}) {
  RunOut R{};
  R.ins = ins;
  if (fns.empty() || fns[0].nblocks() == 0) { R.trapped = true; return R; }
  struct Fr {
    uint32_t fn;
    std::vector<val_t> val;
    std::vector<char> has;
  };
  struct Resume {
    uint32_t fn, blk, i, dst;
  };
  std::vector<Fr> stack;
  std::vector<Resume> resumes;
  {
    uint32_t nv0 = fns[0].nvars + (uint32_t)fns[0].version_base.size();
    stack.push_back({0, std::vector<val_t>(nv0, 0),
                     std::vector<char>(nv0, 0)});
  }
  uint64_t steps = 0;
  uint32_t cur_fn = 0, cur_blk = 0;
  int64_t i = (int64_t)fns[0].blocks[0].begin;
  for (;;) {
    if (++steps > step_limit) { R.timeout = true; return R; }
    if (cur_fn >= fns.size()) { R.trapped = true; return R; }
    const Fn &F = fns[cur_fn];
    if (cur_blk >= F.nblocks() ||
        i < (int64_t)F.blocks[cur_blk].begin ||
        i >= (int64_t)F.blocks[cur_blk].end) {
      R.trapped = true;
      return R;
    }
    const Instr &I = F.code[i];
    switch (I.k) {
    case K::Phi: // resolved on the edge (staged before jumping)
      ++i;
      continue;
    case K::Br:
    case K::Cbr: {
      uint32_t next = (I.k == K::Br)
                          ? I.t
                          : ((stack.back().val[I.a] != 0) ? I.t : I.f);
      if (next >= F.nblocks()) { R.trapped = true; return R; }
      // stage phi values of `next` for this edge: read all, then write all
      std::vector<std::pair<uint32_t, val_t>> staged{};
      for (uint32_t j = F.blocks[next].begin; j < F.blocks[next].end; ++j) {
        const Instr &P = F.code[j];
        if (P.k != K::Phi) break;
        uint32_t arg = NONE;
        if (P.t == cur_blk && P.a != NONE) arg = P.a;
        else if (P.f == cur_blk && P.b2 != NONE) arg = P.b2;
        if (arg != NONE)
          staged.push_back({P.dst, stack.back().val[arg]});
      }
      for (auto [d, v] : staged) {
        stack.back().val[d] = v;
        stack.back().has[d] = 1;
      }
      cur_blk = next;
      i = (int64_t)F.blocks[cur_blk].begin;
      continue;
    }
    case K::Exit:
      return R;
    case K::Ret: {
      val_t rv = (I.a == NONE) ? 0 : stack.back().val[I.a];
      if (resumes.empty() || stack.size() < 2) {
        R.trapped = true; // return with no caller (main has no Ret)
        return R;
      }
      Resume rs = resumes.back();
      resumes.pop_back();
      stack.pop_back();
      cur_fn = rs.fn;
      cur_blk = rs.blk;
      i = (int64_t)rs.i;
      if (rs.dst != NONE) {
        stack.back().val[rs.dst] = rv;
        stack.back().has[rs.dst] = 1;
      }
      continue;
    }
    case K::Call: {
      // depth cap: 512 CALLEE activations (the oracle/native convention;
      // stack.size() counts main's frame, hence the strict >)
      if (stack.size() > MJZ_MAX_CALL_DEPTH) {
        R.trapped = true; // recursion depth cap (matches native guards)
        return R;
      }
      uint32_t g = (uint32_t)I.c;
      if (g >= fns.size() || I.args.size() != fns[g].nparams) {
        R.trapped = true;
        return R;
      }
      resumes.push_back({cur_fn, cur_blk, (uint32_t)(i + 1), I.dst});
      uint32_t nvg = fns[g].nvars + (uint32_t)fns[g].version_base.size();
      std::vector<val_t> nv(nvg, 0);
      std::vector<char> nh(nvg, 0);
      for (uint32_t p = 0; p < I.args.size(); ++p) {
        nv[p] = stack.back().val[I.args[p]];
        nh[p] = 1;
      }
      stack.push_back({g, std::move(nv), std::move(nh)});
      cur_fn = g;
      cur_blk = 0;
      i = (int64_t)fns[g].blocks[0].begin;
      continue;
    }
    case K::Const:
      if (I.dst >= stack.back().val.size())
        std::fprintf(stderr, "OOB const: fn=%u blk=%u dst=%u valsize=%zu nfns=%zu\n",
                     cur_fn, cur_blk, I.dst, stack.back().val.size(), fns.size());
      stack.back().val[I.dst] = I.c;
      stack.back().has[I.dst] = 1;
      break;
    case K::Copy:
      stack.back().val[I.dst] = stack.back().val[I.a];
      stack.back().has[I.dst] = 1;
      break;
    case K::Bin: {
      if (I.a >= stack.back().val.size() || I.b2 >= stack.back().val.size())
        std::fprintf(stderr,
                     "OOB bin: fn=%u blk=%u a=%u b2=%u valsize=%zu nvars=%u\n",
                     cur_fn, cur_blk, I.a, I.b2, stack.back().val.size(),
                     fns[cur_fn].nvars);
      val_t x = stack.back().val[I.a], y = stack.back().val[I.b2];
      Rv r{};
      switch (I.b) {
      case B::Add: r = w_add(x, y); break;
      case B::Sub: r = w_sub(x, y); break;
      case B::Mul: r = w_mul(x, y); break;
      case B::Div: r = w_div(x, y); break;
      case B::Mod: r = w_mod(x, y); break;
      case B::Lt: r = {x < y, false}; break;
      case B::Le: r = {x <= y, false}; break;
      case B::Gt: r = {x > y, false}; break;
      case B::Ge: r = {x >= y, false}; break;
      case B::Eq: r = {x == y, false}; break;
      case B::Ne: r = {x != y, false}; break;
      case B::And: r = w_and(x, y); break;
      case B::Or: r = w_or(x, y); break;
      case B::Xor: r = w_xor(x, y); break;
      case B::Shl: r = w_shl(x, y); break;
      case B::LShr: r = w_lshr(x, y); break;
      case B::AShr: r = w_ashr(x, y); break;
      case B::MulHi: r = w_mulhi(x, y); break;
      }
      if (r.trap) { R.trapped = true; return R; }
      stack.back().val[I.dst] = r.v;
      stack.back().has[I.dst] = 1;
      break;
    }
    case K::Print:
      R.out.push_back(stack.back().val[I.a]);
      break;
    case K::Alloc: {
      Rv r = R.arena_alloc(stack.back().val[I.a]);
      if (r.trap) { R.trapped = true; return R; }
      stack.back().val[I.dst] = r.v;
      stack.back().has[I.dst] = 1;
      break;
    }
    case K::Load: {
      Rv r = R.arena_load(stack.back().val[I.a], stack.back().val[I.b2]);
      if (r.trap) { R.trapped = true; return R; }
      stack.back().val[I.dst] = r.v;
      stack.back().has[I.dst] = 1;
      break;
    }
    case K::Store:
      if (!R.arena_store(stack.back().val[I.a], stack.back().val[I.b2],
                         stack.back().val[I.c2])) {
        R.trapped = true;
        return R;
      }
      break;
    case K::Len:
      stack.back().val[I.dst] = R.arena_len(stack.back().val[I.a]);
      stack.back().has[I.dst] = 1;
      break;
    case K::Input:
      stack.back().val[I.dst] = R.next_input();
      stack.back().has[I.dst] = 1;
      break;
    case K::Depth: // frame count incl. main (matches VM + native)
      stack.back().val[I.dst] = (val_t)stack.size();
      stack.back().has[I.dst] = 1;
      break;
    }
    ++i;
  }
}

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

  constexpr AstRun(const Ast &a_, uint64_t lim = 200000)
      : a(a_), limit(lim) {}

  constexpr bool eval(uint32_t ei, val_t &out) {
    if (dead || ++steps > limit) { R.timeout = true; dead = true; out = 0; return false; }
    const Expr &e = a.es[ei];
    switch (e.k) {
    case Ek::Num: out = e.num; return true;
    case Ek::Var: out = env[e.var]; return true;
    case Ek::Neg: {
      val_t x{};
      if (!eval(e.l, x)) return false;
      out = w_sub(0, x).v;
      return true;
    }
    case Ek::Not: {
      val_t x{};
      if (!eval(e.l, x)) return false;
      out = (x == 0) ? 1 : 0;
      return true;
    }
    case Ek::Bin: {
      val_t x{}, y{};
      if (!eval(e.l, x)) return false;
      if (!eval(e.r, y)) return false;
      Rv r{};
      switch (e.b) {
      case Bk::Add: r = w_add(x, y); break;
      case Bk::Sub: r = w_sub(x, y); break;
      case Bk::Mul: r = w_mul(x, y); break;
      case Bk::Div: r = w_div(x, y); break;
      case Bk::Mod: r = w_mod(x, y); break;
      case Bk::Lt: r = {x < y, false}; break;
      case Bk::Le: r = {x <= y, false}; break;
      case Bk::Gt: r = {x > y, false}; break;
      case Bk::Ge: r = {x >= y, false}; break;
      case Bk::Eq: r = {x == y, false}; break;
      case Bk::Ne: r = {x != y, false}; break;
      }
      if (r.trap) { R.trapped = true; dead = true; out = 0; return false; }
      out = r.v;
      return true;
    }
    case Ek::And: {
      val_t x{};
      if (!eval(e.l, x)) return false;
      if (x == 0) { out = 0; return true; }   // short-circuit
      val_t y{};
      if (!eval(e.r, y)) return false;
      out = (y != 0) ? 1 : 0;
      return true;
    }
    case Ek::Or: {
      val_t x{};
      if (!eval(e.l, x)) return false;
      if (x != 0) { out = 1; return true; }   // short-circuit
      val_t y{};
      if (!eval(e.r, y)) return false;
      out = (y != 0) ? 1 : 0;
      return true;
    }
    case Ek::Alloc: {
      val_t n{};
      if (!eval(e.l, n)) return false;
      Rv r = R.arena_alloc(n);
      if (r.trap) { R.trapped = true; dead = true; out = 0; return false; }
      out = r.v;
      return true;
    }
    case Ek::Load: {
      val_t h{}, i{};
      if (!eval(e.l, h)) return false;
      if (!eval(e.r, i)) return false;
      Rv r = R.arena_load(h, i);
      if (r.trap) { R.trapped = true; dead = true; out = 0; return false; }
      out = r.v;
      return true;
    }
    case Ek::Len: {
      val_t h{};
      if (!eval(e.l, h)) return false;
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
        if (!eval(ai, v)) return false;
        argv.push_back(v);
      }
      if (depth >= MJZ_MAX_CALL_DEPTH) {
        R.trapped = true;
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
    if (dead) return false;
    if (++steps > limit) { R.timeout = true; dead = true; return false; }
    const Stmt &s = a.ss[si];
    switch (s.k) {
    case Sk::Let:
    case Sk::Assign: {
      val_t v{};
      if (!eval(s.e, v)) return false;
      env[s.var] = v;
      return true;
    }
    case Sk::Print: {
      val_t v{};
      if (!eval(s.e, v)) return false;
      R.out.push_back(v);
      return true;
    }
    case Sk::Ret: {
      val_t v{};
      if (!eval(s.e, v)) return false;
      retval = v;
      returned = true;
      return true; // frame finished; callers see `returned` and unwind
    }
    case Sk::Seq:
      for (uint32_t x : a.seq_kids[s.a]) {
        if (!exec(x)) return false;
        if (returned || broke || continued) return true;
      }
      return true;
    case Sk::StoreA: { // a[i1]...[ik] = value (indices, then loads, then value)
      const auto &idx = a.seq_kids[s.a];
      val_t h = env[s.var];
      for (size_t j = 0; j + 1 < idx.size(); ++j) {
        val_t i{};
        if (!eval(idx[j], i)) return false;
        Rv r = R.arena_load(h, i);
        if (r.trap) { R.trapped = true; dead = true; return false; }
        h = r.v;
      }
      val_t last{};
      if (!eval(idx.back(), last)) return false;
      val_t v{};
      if (!eval(s.b, v)) return false;
      if (!R.arena_store(h, last, v)) {
        R.trapped = true;
        dead = true;
        return false;
      }
      return true;
    }
    case Sk::If: {
      val_t c{};
      if (!eval(s.e, c)) return false;
      return exec(c != 0 ? s.a : s.b);
    }
    case Sk::Brk:
      broke = true;
      return true; // the enclosing While consumes the flag
    case Sk::Cont:
      continued = true;
      return true;
    case Sk::While: {
      for (;;) {
        val_t c{};
        if (!eval(s.e, c)) return false;
        if (c == 0) return true;
        if (!exec(s.a)) return false;
        if (returned) return true;
        if (broke) { broke = false; return true; }
        if (continued) { continued = false; /* re-test */ }
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


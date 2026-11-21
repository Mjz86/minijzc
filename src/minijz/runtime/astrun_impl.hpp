#pragma once
// === astrun_impl.hpp ========================================
// the AST-oracle bodies: AstRun::eval, AstRun::exec, run_ast (see
// astrun.hpp for the semantics). Included by astrun.hpp in the header-only
// build (constexpr, via MJZ_RUNNER_FN -- the cx proofs call run_ast in
// constant expressions) and by tu/runners.cpp in the multi-TU build (plain
// external linkage) -- include it nowhere else.

#include "astrun.hpp"

MJZ_RUNNER_FN bool AstRun::eval(uint32_t ei, val_t &out) {
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
  case Ek::And:
  case Ek::Or: { // mirror arms (the ckpt-33 dedup): only the
    // short-circuit polarity differs (And stops on 0, Or on nonzero)
    const bool is_and = e.k == Ek::And;
    val_t x{};
    if (!eval(e.l, x))
      return false;
    if (is_and ? x == 0 : x != 0) {
      out = is_and ? 0 : 1;
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

MJZ_RUNNER_FN bool AstRun::exec(uint32_t si) {
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

MJZ_RUNNER_FN RunOut run_ast(const Ast &a, uint32_t root, uint64_t limit,
                             const std::vector<val_t> &ins) {
  AstRun ar(a, limit);
  ar.R.ins = ins;
  ar.env.assign(a.names.size(), 0);
  ar.exec(root);
  return ar.R;
}

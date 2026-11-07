#pragma once

// ============================================================================
// 6. CFG builder: AST -> IR with short-circuit lowering.
//    Pre-SSA: each source variable is one vreg with possibly many defs;
//    temps are single-def except short-circuit results (two Const defs),
//    which the SSA pass turns into phis via standard IDF placement.
// ============================================================================
struct Builder {
  Fn &fn;
  uint32_t cur{NONE};
  // innermost-first stack of (header, exit) for `break`/`continue` targets
  std::vector<std::pair<uint32_t, uint32_t>> loop_stack{};
  // AST var id -> LOCAL vreg id. Each function compiles into its own vreg
  // space: params are vregs 0..P-1, then body locals and temps. (AST ids are
  // global across the whole program, so every reference goes through this.)
  std::vector<uint32_t> vmap{};

  constexpr Builder(Fn &f) : fn(f) {}

  constexpr uint32_t map_def(uint32_t ast_var, const Ast &a) {
    if (vmap[ast_var] == NONE) {
      vmap[ast_var] = fn.nvars++;
      fn.var_names.push_back(a.names[ast_var]);
    }
    return vmap[ast_var];
  }
  constexpr uint32_t fresh_temp() {
    uint32_t v = fn.nvars++;
    fn.var_names.push_back({});
    return v;
  }
  constexpr uint32_t new_block() {
    uint32_t b = fn.nblocks();
    fn.blocks.push_back({});
    fn.blocks.back().begin = NONE; // sentinel: real begin set on first emit
    fn.succ.push_back(NONE);
    fn.succ.push_back(NONE);
    return b;
  }
  constexpr void emit(Instr I) {
    if (fn.blocks[cur].begin == NONE)
      fn.blocks[cur].begin = (uint32_t)fn.code.size();
    fn.code.push_back(I);
  }
  constexpr void finish(uint32_t s0, uint32_t s1) {
    uint32_t b = cur;
    if (fn.blocks[b].begin == NONE)
      fn.blocks[b].begin = (uint32_t)fn.code.size(); // empty block
    fn.blocks[b].end = (uint32_t)fn.code.size();
    fn.succ[2 * b] = s0;
    fn.succ[2 * b + 1] = s1;
    cur = NONE;
  }
  constexpr void close_br(uint32_t tgt) {
    emit({.k = K::Br, .t = tgt});
    finish(tgt, NONE);
  }
  constexpr void close_cbr(uint32_t c, uint32_t tt, uint32_t ff) {
    emit({.k = K::Cbr, .a = c, .t = tt, .f = ff});
    finish(tt, ff);
  }
  constexpr void close_exit() {
    emit({.k = K::Exit});
    finish(NONE, NONE);
  }
  constexpr void ensure_open() {
    if (cur == NONE) cur = new_block();
  }
  constexpr uint32_t const_val(val_t v) {
    uint32_t d = fresh_temp();
    emit({.k = K::Const, .dst = d, .c = v});
    return d;
  }

  // returns vreg holding the expression value
  constexpr uint32_t ex(uint32_t ei, const Ast &a) {
    const Expr &e = a.es[ei];
    switch (e.k) {
    case Ek::Num: return const_val(e.num);
    case Ek::Var:
      // A use with no local mapping yet happens for `let x = ... x ...`
      // (the initializer sees the NEW binding, C-style). Storage is
      // zero-initialized, exactly like the AST oracle's env -- read 0.
      if (vmap[e.var] == NONE) return const_val(0);
      return vmap[e.var];
    case Ek::Neg: {
      uint32_t x = ex(e.l, a);
      uint32_t z = const_val(0);
      uint32_t d = fresh_temp();
      emit({.k = K::Bin, .b = B::Sub, .dst = d, .a = z, .b2 = x});
      return d;
    }
    case Ek::Not: {
      uint32_t x = ex(e.l, a);
      uint32_t z = const_val(0);
      uint32_t d = fresh_temp();
      emit({.k = K::Bin, .b = B::Eq, .dst = d, .a = x, .b2 = z});
      return d;
    }
    case Ek::Bin: {
      uint32_t x = ex(e.l, a);
      uint32_t y = ex(e.r, a);
      uint32_t d = fresh_temp();
      emit({.k = K::Bin, .b = ir_op(e.b), .dst = d, .a = x, .b2 = y});
      return d;
    }
    case Ek::Alloc: { // d = alloc(n): fresh handle into the bump arena
      uint32_t n = ex(e.l, a);
      uint32_t d = fresh_temp();
      emit({.k = K::Alloc, .dst = d, .a = n});
      return d;
    }
    case Ek::Load: { // d = h[i] (bounds-checked, TRAPs when out of range)
      uint32_t h = ex(e.l, a);
      uint32_t i = ex(e.r, a);
      uint32_t d = fresh_temp();
      emit({.k = K::Load, .dst = d, .a = h, .b2 = i});
      return d;
    }
    case Ek::Len: { // d = len(h): 0 unless h is an allocated handle
      uint32_t h = ex(e.l, a);
      uint32_t d = fresh_temp();
      emit({.k = K::Len, .dst = d, .a = h});
      return d;
    }
    case Ek::Input: { // d = input(): next integer from the stdin tape
      uint32_t d = fresh_temp();
      emit({.k = K::Input, .dst = d});
      return d;
    }
    case Ek::Call: { // d = f_k(a0, ..): args evaluated left to right
      std::vector<uint32_t> argv{};
      for (uint32_t ke : a.seq_kids[e.l]) argv.push_back(ex(ke, a));
      uint32_t d = fresh_temp();
      Instr ci{.k = K::Call, .dst = d, .c = (int64_t)e.var + 1};
      ci.args = std::move(argv);
      emit(ci);
      return d;
    }
    case Ek::And: {
      // short-circuit: l ? r : 0  — diamond, phi appears via SSA/IDF
      uint32_t res = fresh_temp();
      uint32_t l = ex(e.l, a);
      uint32_t rb = new_block(), tt = new_block(), ff = new_block(),
               mm = new_block();
      close_cbr(l, rb, ff);
      cur = rb;
      uint32_t r = ex(e.r, a);
      close_cbr(r, tt, ff);
      cur = tt; emit({.k = K::Const, .dst = res, .c = 1}); close_br(mm);
      cur = ff; emit({.k = K::Const, .dst = res, .c = 0}); close_br(mm);
      cur = mm;
      return res;
    }
    case Ek::Or: {
      uint32_t res = fresh_temp();
      uint32_t l = ex(e.l, a);
      uint32_t rb = new_block(), tt = new_block(), ff = new_block(),
               mm = new_block();
      close_cbr(l, tt, rb);        // true short-circuits
      cur = rb;
      uint32_t r = ex(e.r, a);
      close_cbr(r, tt, ff);
      cur = tt; emit({.k = K::Const, .dst = res, .c = 1}); close_br(mm);
      cur = ff; emit({.k = K::Const, .dst = res, .c = 0}); close_br(mm);
      cur = mm;
      return res;
    }
    }
    return NONE;
  }

  constexpr void st(uint32_t si, const Ast &a) {
    const Stmt &s = a.ss[si];
    switch (s.k) {
    case Sk::Let:
    case Sk::Assign: {
      ensure_open();
      uint32_t v = ex(s.e, a);
      emit({.k = K::Copy, .dst = map_def(s.var, a), .a = v});
      break;
    }
    case Sk::Print: {
      ensure_open();
      uint32_t v = ex(s.e, a);
      emit({.k = K::Print, .a = v});
      break;
    }
    case Sk::Ret: { // return expr;  -- terminates the block (no successors)
      ensure_open();
      uint32_t v = ex(s.e, a);
      emit({.k = K::Ret, .a = v});
      finish(NONE, NONE);
      break;
    }
    case Sk::StoreA: { // a[i1]...[ik] = value:  loads for k-1 indices, then Store
      ensure_open();
      const auto &idx = a.seq_kids[s.a];
      uint32_t h = vmap[s.var];         // base array variable (a handle vreg)
      for (size_t j = 0; j + 1 < idx.size(); ++j) {
        uint32_t i = ex(idx[j], a);     // index exprs left-to-right,
        uint32_t t = fresh_temp();      // intermediate handle loads
        emit({.k = K::Load, .dst = t, .a = h, .b2 = i});
        h = t;
      }
      uint32_t last = ex(idx.back(), a);
      uint32_t v = ex(s.b, a);          // value evaluated last (oracle order)
      emit({.k = K::Store, .a = h, .b2 = last, .c2 = v});
      break;
    }
    case Sk::Seq:
      for (uint32_t x : a.seq_kids[s.a]) st(x, a);
      break;
    case Sk::If: {
      ensure_open();
      uint32_t c = ex(s.e, a);
      uint32_t tb = new_block(), eb = new_block(), jb = new_block();
      close_cbr(c, tb, eb);
      cur = tb;
      st(s.a, a);
      ensure_open();
      close_br(jb);
      cur = eb;
      st(s.b, a);
      ensure_open();
      close_br(jb);
      cur = jb;
      break;
    }
    case Sk::Brk: { // break; -> unconditional jump to the loop's exit
      ensure_open();
      close_br(loop_stack.back().second);
      break;
    }
    case Sk::Cont: { // continue; -> jump back to the header (re-test)
      ensure_open();
      close_br(loop_stack.back().first);
      break;
    }
    case Sk::While: {
      ensure_open();
      uint32_t h = new_block(), body = new_block(), after = new_block();
      close_br(h);                       // preheader -> header
      cur = h;
      uint32_t c = ex(s.e, a);
      close_cbr(c, body, after);
      cur = body;
      loop_stack.push_back({h, after});
      st(s.a, a);
      loop_stack.pop_back();
      ensure_open();
      close_br(h);                       // back edge
      cur = after;
      break;
    }
    }
  }

  constexpr void start(const Ast &a) {
    fn.nvars = 0;
    fn.var_names.clear();
    fn.nparams = 0;
    vmap.assign(a.names.size(), NONE);
    new_block(); // entry = block 0
    cur = 0;
  }
  constexpr void build_main(const Ast &a, uint32_t root) {
    start(a);
    st(root, a);
    ensure_open();
    close_exit();
    fn.recompute_preds();
    normalize_multi_preds(fn); // break/continue can create 3+ pred blocks
  }
  constexpr void build_fn(const Ast &a, const FnDef &fd) {
    start(a);
    fn.nparams = (uint32_t)fd.params.size();
    for (uint32_t p : fd.params) { // params are the first vregs
      vmap[p] = fn.nvars++;
      fn.var_names.push_back(a.names[p]);
    }
    st(fd.body, a);
    ensure_open();
    // control falling off the end returns 0 (the block may be unreachable
    // when every path already returned -- unreachable removal cleans it)
    uint32_t z = const_val(0);
    emit({.k = K::Ret, .a = z});
    finish(NONE, NONE);
    fn.recompute_preds();
    normalize_multi_preds(fn);
  }
};
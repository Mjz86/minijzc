// ============================================================================
// MiniJZ compiler — §9h bytecode emitter (stack VM)
// ----------------------------------------------------------------------------
// Ordered-include header: this file is a slice of the original single
// translation unit and relies on being included after its predecessors
// (see src/minijz.hpp for the canonical order). Do not include it
// directly in an arbitrary order.
// ============================================================================
#pragma once

// 9h. BYTECODE backend — a tiny stack VM.
//     [u32 slot_count][code...] ; ops: Push i64, Load/Store u32 slot,
//     Add Sub Mul Div Mod Lt Le Gt Ge Eq Ne, Jmp u32, Jz u32, Print, Halt,
//     ArrNew/ArrLd/ArrSt/ArrLen (arena, operands via the stack), In (stdin).
//     Div/Mod trap exactly like the interpreters. Runs the destroyed IR.
// ============================================================================
enum class Bc : uint8_t {
  Halt = 0, Push, Load, Store, Add, Sub, Mul, Div, Mod,
  Lt, Le, Gt, Ge, Eq, Ne, Jmp, Jz, Print,
  ArrNew, ArrLd, ArrSt, ArrLen, In,
  Call, Ret, // Call u32 fnidx u32 nargs; Ret (value via the operand stack)
  And, Or, Xor, Shl, Lshr, Asr, MulHi, // optimizer-lowered bit ops
  Depth                                // push the frame count (call depth)
};
constexpr const char *bc_name(Bc op) {
  switch (op) {
  case Bc::Halt: return "halt";
  case Bc::Push: return "push";
  case Bc::Load: return "load";
  case Bc::Store: return "store";
  case Bc::Add: return "add";
  case Bc::Sub: return "sub";
  case Bc::Mul: return "mul";
  case Bc::Div: return "div";
  case Bc::Mod: return "mod";
  case Bc::Lt: return "lt";
  case Bc::Le: return "le";
  case Bc::Gt: return "gt";
  case Bc::Ge: return "ge";
  case Bc::Eq: return "eq";
  case Bc::Ne: return "ne";
  case Bc::Jmp: return "jmp";
  case Bc::Jz: return "jz";
  case Bc::Print: return "print";
  case Bc::ArrNew: return "arrnew";
  case Bc::ArrLd: return "arrld";
  case Bc::ArrSt: return "arrst";
  case Bc::ArrLen: return "arrlen";
  case Bc::In: return "in";
  case Bc::Call: return "call";
  case Bc::Ret: return "ret";
  case Bc::And: return "and";
  case Bc::Or: return "or";
  case Bc::Xor: return "xor";
  case Bc::Shl: return "shl";
  case Bc::Lshr: return "lshr";
  case Bc::Asr: return "asr";
  case Bc::MulHi: return "mulhi";
  case Bc::Depth: return "depth";
  }
  return "?";
}
constexpr void bc_put_u32(std::vector<uint8_t> &c, uint32_t v) {
  for (int i = 0; i < 4; ++i) { c.push_back((uint8_t)(v & 0xff)); v >>= 8; }
}
constexpr void bc_put_i64(std::vector<uint8_t> &c, int64_t v) {
  uint64_t u = (uint64_t)v;
  for (int i = 0; i < 8; ++i) { c.push_back((uint8_t)(u & 0xff)); u >>= 8; }
}
constexpr uint32_t bc_rd_u32(const std::vector<uint8_t> &c, size_t p) {
  return (uint32_t)c[p] | ((uint32_t)c[p + 1] << 8) |
         ((uint32_t)c[p + 2] << 16) | ((uint32_t)c[p + 3] << 24);
}
constexpr int64_t bc_rd_i64(const std::vector<uint8_t> &c, size_t p) {
  uint64_t u = 0;
  for (int i = 0; i < 8; ++i) u |= (uint64_t)c[p + (size_t)i] << (8 * i);
  return (int64_t)u;
}
constexpr Bc bc_of_bin(B b) {
  switch (b) {
  case B::Add: return Bc::Add;
  case B::Sub: return Bc::Sub;
  case B::Mul: return Bc::Mul;
  case B::Div: return Bc::Div;
  case B::Mod: return Bc::Mod;
  case B::Lt: return Bc::Lt;
  case B::Le: return Bc::Le;
  case B::Gt: return Bc::Gt;
  case B::Ge: return Bc::Ge;
  case B::Eq: return Bc::Eq;
  case B::Ne: return Bc::Ne;
  case B::And: return Bc::And;
  case B::Or: return Bc::Or;
  case B::Xor: return Bc::Xor;
  case B::Shl: return Bc::Shl;
  case B::LShr: return Bc::Lshr;
  case B::AShr: return Bc::Asr;
  case B::MulHi: return Bc::MulHi;
  }
  return Bc::Halt;
}
constexpr uint32_t bc_instr_size(const Instr &I) {
  switch (I.k) {
  case K::Const: return 9 + 5;              // Push + Store
  case K::Copy: return 5 + 5;               // Load + Store
  case K::Bin: return 5 + 5 + 1 + 5;        // Load,Load,op,Store
  case K::Br: return 5;                     // Jmp
  case K::Cbr: return 5 + 5 + 5;            // Load,Jz,Jmp
  case K::Print: return 5 + 1;              // Load,Print
  case K::Exit: return 1;                   // Halt
  case K::Phi: return 0;                    // gone (destroyed)
  case K::Alloc: return 5 + 1 + 5;          // Load,ArrNew,Store
  case K::Load: return 5 + 5 + 1 + 5;       // Load,Load,ArrLd,Store
  case K::Store: return 5 + 5 + 5 + 1;      // Load,Load,Load,ArrSt
  case K::Len: return 5 + 1 + 5;            // Load,ArrLen,Store
  case K::Input: return 1 + 5;              // In,Store
  case K::Depth: return 1 + 5;              // Depth,Store
  case K::Call:                             // Load*argc,Call,Store
    return 1 + 4 + 4 + 5 * (uint32_t)I.args.size() + 5;
  case K::Ret: return (I.a == NONE ? 9 : 5) + 1; // (Push|Load),Ret
  }
  return 0;
}
// layout: [u32 nfns][nfns x (u32 code_off, u32 nslots)][code...]
// fns[0] is the main body; block jumps are absolute byte offsets.
constexpr std::vector<uint8_t> emit_bytecode(const std::vector<Fn> &fns) {
  const uint32_t nf = (uint32_t)fns.size();
  std::vector<std::vector<uint32_t>> off(nf);
  std::vector<uint32_t> fn_off(nf, 0);
  uint32_t cur = 4 + 8 * nf;
  for (uint32_t f = 0; f < nf; ++f) {
    const Fn &fn = fns[f];
    off[f].assign(fn.nblocks(), 0);
    for (uint32_t b = 0; b < fn.nblocks(); ++b) {
      off[f][b] = cur;
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
        cur += bc_instr_size(fn.code[i]);
    }
    fn_off[f] = fn.nblocks() ? off[f][0] : cur;
  }
  std::vector<uint8_t> c{};
  bc_put_u32(c, nf);
  for (uint32_t f = 0; f < nf; ++f) {
    bc_put_u32(c, fn_off[f]);
    bc_put_u32(c, slot_count(fns[f])); // VM slots (== vregs w/o regalloc)
  }
  for (uint32_t f = 0; f < nf; ++f) {
    const Fn &fn = fns[f];
    for (uint32_t b = 0; b < fn.nblocks(); ++b) {
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        switch (I.k) {
        case K::Phi:
          break;
        case K::Const:
          c.push_back((uint8_t)Bc::Push); bc_put_i64(c, I.c);
          c.push_back((uint8_t)Bc::Store); bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Copy:
          c.push_back((uint8_t)Bc::Load); bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::Store); bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Bin:
          c.push_back((uint8_t)Bc::Load); bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::Load); bc_put_u32(c, slot_id(fns[f], I.b2));
          c.push_back((uint8_t)bc_of_bin(I.b));
          c.push_back((uint8_t)Bc::Store); bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Br:
          c.push_back((uint8_t)Bc::Jmp);
          bc_put_u32(c, I.t == NONE ? off[f][b] : off[f][I.t]);
          break;
        case K::Cbr:
          c.push_back((uint8_t)Bc::Load); bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::Jz);
          bc_put_u32(c, I.f == NONE ? off[f][b] : off[f][I.f]);
          c.push_back((uint8_t)Bc::Jmp);
          bc_put_u32(c, I.t == NONE ? off[f][b] : off[f][I.t]);
          break;
        case K::Print:
          c.push_back((uint8_t)Bc::Load); bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::Print);
          break;
        case K::Alloc:
          c.push_back((uint8_t)Bc::Load); bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::ArrNew);
          c.push_back((uint8_t)Bc::Store); bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Load:
          c.push_back((uint8_t)Bc::Load); bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::Load); bc_put_u32(c, slot_id(fns[f], I.b2));
          c.push_back((uint8_t)Bc::ArrLd);
          c.push_back((uint8_t)Bc::Store); bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Store:
          c.push_back((uint8_t)Bc::Load); bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::Load); bc_put_u32(c, slot_id(fns[f], I.b2));
          c.push_back((uint8_t)Bc::Load); bc_put_u32(c, slot_id(fns[f], I.c2));
          c.push_back((uint8_t)Bc::ArrSt);
          break;
        case K::Len:
          c.push_back((uint8_t)Bc::Load); bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::ArrLen);
          c.push_back((uint8_t)Bc::Store); bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Input:
          c.push_back((uint8_t)Bc::In);
          c.push_back((uint8_t)Bc::Store); bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Depth:
          c.push_back((uint8_t)Bc::Depth);
          c.push_back((uint8_t)Bc::Store); bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Call:
          for (uint32_t av : I.args) { // args pushed in parameter order
            c.push_back((uint8_t)Bc::Load); bc_put_u32(c, slot_id(fns[f], av));
          }
          c.push_back((uint8_t)Bc::Call);
          bc_put_u32(c, (uint32_t)I.c);
          bc_put_u32(c, (uint32_t)I.args.size());
          c.push_back((uint8_t)Bc::Store); bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Ret:
          if (I.a == NONE) {
            c.push_back((uint8_t)Bc::Push); bc_put_i64(c, 0);
          } else {
            c.push_back((uint8_t)Bc::Load); bc_put_u32(c, slot_id(fns[f], I.a));
          }
          c.push_back((uint8_t)Bc::Ret);
          break;
        case K::Exit:
          c.push_back((uint8_t)Bc::Halt);
          break;
        }
      }
    }
  }
  return c;
}

constexpr RunOut vm_run(const std::vector<uint8_t> &c,
                        uint64_t step_limit = 200000,
                        const std::vector<val_t> &ins = {}) {
  RunOut R{};
  R.ins = ins;
  if (c.size() < 4) { R.trapped = true; return R; }
  const uint32_t nfns = bc_rd_u32(c, 0);
  if (!nfns || c.size() < 4 + (size_t)8 * nfns) {
    R.trapped = true;
    return R;
  }
  auto fn_off = [&](uint32_t f) -> uint32_t {
    return bc_rd_u32(c, 4 + (size_t)8 * f);
  };
  auto fn_slots = [&](uint32_t f) -> uint32_t {
    return bc_rd_u32(c, 4 + (size_t)8 * f + 4);
  };
  std::vector<std::vector<val_t>> frames; // one slot vector per activation
  frames.push_back(std::vector<val_t>(fn_slots(0), 0));
  std::vector<uint32_t> rets;             // return pcs of suspended callers
  std::vector<val_t> st{};
  size_t pc = fn_off(0);
  // "steps" count IR-equivalent instructions (Store/Jmp/Jz/Print/Halt), not
  // sub-ops, so one step limit means the same thing for interpreter and VM.
  uint64_t steps = 0;
  for (;;) {
    if (pc >= c.size()) { R.trapped = true; return R; }
    Bc op = (Bc)c[pc++];
    switch (op) {
    case Bc::Halt:
      if (++steps > step_limit) { R.timeout = true; return R; }
      return R;
    case Bc::Push:
      st.push_back(bc_rd_i64(c, pc)); pc += 8; break;
    case Bc::Load:
      st.push_back(frames.back()[bc_rd_u32(c, pc)]); pc += 4; break;
    case Bc::Store: {
      if (++steps > step_limit) { R.timeout = true; return R; }
      val_t v = st.back(); st.pop_back();
      frames.back()[bc_rd_u32(c, pc)] = v; pc += 4; break;
    }
    case Bc::Call: {
      if (++steps > step_limit) { R.timeout = true; return R; }
      uint32_t g = bc_rd_u32(c, pc); pc += 4;
      uint32_t na = bc_rd_u32(c, pc); pc += 4;
      if (g >= nfns || frames.size() > MJZ_MAX_CALL_DEPTH) {
        R.trapped = true; // bad callee / recursion depth cap (512 CALLEE
        return R;        // activations; main's frame does not count)
      }
      std::vector<val_t> args(na, 0); // pop na args (a0 pushed first)
      for (uint32_t k = na; k-- > 0;) {
        if (st.empty()) { R.trapped = true; return R; }
        args[k] = st.back();
        st.pop_back();
      }
      rets.push_back((uint32_t)pc);
      std::vector<val_t> fr(fn_slots(g), 0);
      for (uint32_t k = 0; k < na && k < fr.size(); ++k) fr[k] = args[k];
      frames.push_back(std::move(fr));
      pc = fn_off(g);
      break;
    }
    case Bc::Ret: {
      if (++steps > step_limit) { R.timeout = true; return R; }
      if (rets.empty() || st.empty() || frames.size() < 2) {
        R.trapped = true;
        return R;
      }
      val_t v = st.back();
      st.pop_back();
      frames.pop_back();
      pc = rets.back();
      rets.pop_back();
      st.push_back(v); // the caller's Store pops it into the result slot
      break;
    }
    case Bc::Add: case Bc::Sub: case Bc::Mul: case Bc::Div: case Bc::Mod:
    case Bc::Lt: case Bc::Le: case Bc::Gt: case Bc::Ge: case Bc::Eq:
    case Bc::Ne:
    case Bc::And: case Bc::Or: case Bc::Xor: case Bc::Shl: case Bc::Lshr:
    case Bc::Asr: case Bc::MulHi: {
      if (st.size() < 2) { R.trapped = true; return R; }
      val_t y = st.back(); st.pop_back();
      val_t x = st.back(); st.pop_back();
      Rv v{};
      switch (op) {
      case Bc::Add: v = w_add(x, y); break;
      case Bc::Sub: v = w_sub(x, y); break;
      case Bc::Mul: v = w_mul(x, y); break;
      case Bc::Div: v = w_div(x, y); break;
      case Bc::Mod: v = w_mod(x, y); break;
      case Bc::Lt: v = {x < y, false}; break;
      case Bc::Le: v = {x <= y, false}; break;
      case Bc::Gt: v = {x > y, false}; break;
      case Bc::Ge: v = {x >= y, false}; break;
      case Bc::Eq: v = {x == y, false}; break;
      case Bc::Ne: v = {x != y, false}; break;
      case Bc::And: v = w_and(x, y); break;
      case Bc::Or: v = w_or(x, y); break;
      case Bc::Xor: v = w_xor(x, y); break;
      case Bc::Shl: v = w_shl(x, y); break;
      case Bc::Lshr: v = w_lshr(x, y); break;
      case Bc::Asr: v = w_ashr(x, y); break;
      case Bc::MulHi: v = w_mulhi(x, y); break;
      default: break;
      }
      if (v.trap) { R.trapped = true; return R; }
      st.push_back(v.v);
      break;
    }
    case Bc::Jmp:
      if (++steps > step_limit) { R.timeout = true; return R; }
      pc = bc_rd_u32(c, pc); break;
    case Bc::Jz: {
      if (++steps > step_limit) { R.timeout = true; return R; }
      val_t v = st.back(); st.pop_back();
      uint32_t t = bc_rd_u32(c, pc); pc += 4;
      if (v == 0) pc = t;
      break;
    }
    case Bc::Print: {
      if (++steps > step_limit) { R.timeout = true; return R; }
      val_t v = st.back(); st.pop_back();
      R.out.push_back(v);
      break;
    }
    case Bc::ArrNew: { // pop n -> push fresh handle
      val_t n = st.back(); st.pop_back();
      Rv r = R.arena_alloc(n);
      if (r.trap) { R.trapped = true; return R; }
      st.push_back(r.v);
      break;
    }
    case Bc::ArrLd: { // pop i, pop h -> push h[i]
      if (st.size() < 2) { R.trapped = true; return R; }
      val_t i = st.back(); st.pop_back();
      val_t h = st.back(); st.pop_back();
      Rv r = R.arena_load(h, i);
      if (r.trap) { R.trapped = true; return R; }
      st.push_back(r.v);
      break;
    }
    case Bc::ArrSt: { // pop v, pop i, pop h -> h[i] = v   (1 IR step)
      if (++steps > step_limit) { R.timeout = true; return R; }
      if (st.size() < 3) { R.trapped = true; return R; }
      val_t v = st.back(); st.pop_back();
      val_t i = st.back(); st.pop_back();
      val_t h = st.back(); st.pop_back();
      if (!R.arena_store(h, i, v)) { R.trapped = true; return R; }
      break;
    }
    case Bc::ArrLen: { // pop h -> push len(h)
      val_t h = st.back(); st.pop_back();
      st.push_back(R.arena_len(h));
      break;
    }
    case Bc::In: // push next stdin value (0 at EOF)
      st.push_back(R.next_input());
      break;
    case Bc::Depth: // frame count incl. main — same convention as every
      st.push_back((val_t)frames.size()); // runner (SSA interp: stack.size)
      break;
    }
  }
}
// ============================================================================

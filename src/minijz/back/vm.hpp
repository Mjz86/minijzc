#pragma once
// === vm.hpp ========================================
// the bytecode VM runner.

#include "../ir/instr.hpp"

// (MJZ_MAX_CALL_DEPTH is defined with the const-call evaluator in 9e3)

// ============================================================================
// 9h. BYTECODE backend — a tiny stack VM.
//     [u32 slot_count][code...] ; ops: Push i64, Load/Store u32 slot,
//     Add Sub Mul Div Mod Lt Le Gt Ge Eq Ne, Jmp u32, Jz u32, Print, Halt,
//     ArrNew/ArrLd/ArrSt/ArrLen (arena, operands via the stack), In (stdin).
//     Div/Mod trap exactly like the interpreters. Runs the destroyed IR.
// ============================================================================
enum class Bc : uint8_t {
  Halt = 0,
  Push,
  Load,
  Store,
  Add,
  Sub,
  Mul,
  Div,
  Mod,
  Lt,
  Le,
  Gt,
  Ge,
  Eq,
  Ne,
  Jmp,
  Jz,
  Print,
  ArrNew,
  ArrLd,
  ArrSt,
  ArrLen,
  In,
  Call,
  Ret, // Call u32 fnidx u32 nargs; Ret (value via the operand stack)
  And,
  Or,
  Xor,
  Shl,
  Lshr,
  Asr,
  MulHi, // optimizer-lowered bit ops
  Depth  // push the frame count (call depth)
};
constexpr const char *bc_name(Bc op) {
  switch (op) {
  case Bc::Halt:
    return "halt";
  case Bc::Push:
    return "push";
  case Bc::Load:
    return "load";
  case Bc::Store:
    return "store";
  case Bc::Add:
    return "add";
  case Bc::Sub:
    return "sub";
  case Bc::Mul:
    return "mul";
  case Bc::Div:
    return "div";
  case Bc::Mod:
    return "mod";
  case Bc::Lt:
    return "lt";
  case Bc::Le:
    return "le";
  case Bc::Gt:
    return "gt";
  case Bc::Ge:
    return "ge";
  case Bc::Eq:
    return "eq";
  case Bc::Ne:
    return "ne";
  case Bc::Jmp:
    return "jmp";
  case Bc::Jz:
    return "jz";
  case Bc::Print:
    return "print";
  case Bc::ArrNew:
    return "arrnew";
  case Bc::ArrLd:
    return "arrld";
  case Bc::ArrSt:
    return "arrst";
  case Bc::ArrLen:
    return "arrlen";
  case Bc::In:
    return "in";
  case Bc::Call:
    return "call";
  case Bc::Ret:
    return "ret";
  case Bc::And:
    return "and";
  case Bc::Or:
    return "or";
  case Bc::Xor:
    return "xor";
  case Bc::Shl:
    return "shl";
  case Bc::Lshr:
    return "lshr";
  case Bc::Asr:
    return "asr";
  case Bc::MulHi:
    return "mulhi";
  case Bc::Depth:
    return "depth";
  }
  return "?";
}
constexpr void bc_put_u32(std::vector<uint8_t> &c, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    c.push_back((uint8_t)(v & 0xff));
    v >>= 8;
  }
}
constexpr void bc_put_i64(std::vector<uint8_t> &c, int64_t v) {
  uint64_t u = (uint64_t)v;
  for (int i = 0; i < 8; ++i) {
    c.push_back((uint8_t)(u & 0xff));
    u >>= 8;
  }
}
constexpr uint32_t bc_rd_u32(const std::vector<uint8_t> &c, size_t p) {
  return (uint32_t)c[p] | ((uint32_t)c[p + 1] << 8) |
         ((uint32_t)c[p + 2] << 16) | ((uint32_t)c[p + 3] << 24);
}
constexpr int64_t bc_rd_i64(const std::vector<uint8_t> &c, size_t p) {
  uint64_t u = 0;
  for (int i = 0; i < 8; ++i)
    u |= (uint64_t)c[p + (size_t)i] << (8 * i);
  return (int64_t)u;
}
constexpr Bc bc_of_bin(B b) {
  switch (b) {
  case B::Add:
    return Bc::Add;
  case B::Sub:
    return Bc::Sub;
  case B::Mul:
    return Bc::Mul;
  case B::Div:
    return Bc::Div;
  case B::Mod:
    return Bc::Mod;
  case B::Lt:
    return Bc::Lt;
  case B::Le:
    return Bc::Le;
  case B::Gt:
    return Bc::Gt;
  case B::Ge:
    return Bc::Ge;
  case B::Eq:
    return Bc::Eq;
  case B::Ne:
    return Bc::Ne;
  case B::And:
    return Bc::And;
  case B::Or:
    return Bc::Or;
  case B::Xor:
    return Bc::Xor;
  case B::Shl:
    return Bc::Shl;
  case B::LShr:
    return Bc::Lshr;
  case B::AShr:
    return Bc::Asr;
  case B::MulHi:
    return Bc::MulHi;
  }
  return Bc::Halt;
}
constexpr uint32_t bc_instr_size(const Instr &I) {
  switch (I.k) {
  case K::Const:
    return 9 + 5; // Push + Store
  case K::Copy:
    return 5 + 5; // Load + Store
  case K::Bin:
    return 5 + 5 + 1 + 5; // Load,Load,op,Store
  case K::Br:
    return 5; // Jmp
  case K::Cbr:
    return 5 + 5 + 5; // Load,Jz,Jmp
  case K::Print:
    return 5 + 1; // Load,Print
  case K::Exit:
    return 1; // Halt
  case K::Phi:
    return 0; // gone (destroyed)
  case K::Alloc:
    return 5 + 1 + 5; // Load,ArrNew,Store
  case K::Load:
    return 5 + 5 + 1 + 5; // Load,Load,ArrLd,Store
  case K::Store:
    return 5 + 5 + 5 + 1; // Load,Load,Load,ArrSt
  case K::Len:
    return 5 + 1 + 5; // Load,ArrLen,Store
  case K::Input:
    return 1 + 5; // In,Store
  case K::Depth:
    return 1 + 5; // Depth,Store
  case K::Call:   // Load*argc,Call,Store
    return 1 + 4 + 4 + 5 * (uint32_t)I.args.size() + 5;
  case K::Ret:
    return (I.a == NONE ? 9 : 5) + 1; // (Push|Load),Ret
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
          c.push_back((uint8_t)Bc::Push);
          bc_put_i64(c, I.c);
          c.push_back((uint8_t)Bc::Store);
          bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Copy:
          c.push_back((uint8_t)Bc::Load);
          bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::Store);
          bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Bin:
          c.push_back((uint8_t)Bc::Load);
          bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::Load);
          bc_put_u32(c, slot_id(fns[f], I.b2));
          c.push_back((uint8_t)bc_of_bin(I.b));
          c.push_back((uint8_t)Bc::Store);
          bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Br:
          c.push_back((uint8_t)Bc::Jmp);
          bc_put_u32(c, I.t == NONE ? off[f][b] : off[f][I.t]);
          break;
        case K::Cbr:
          c.push_back((uint8_t)Bc::Load);
          bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::Jz);
          bc_put_u32(c, I.f == NONE ? off[f][b] : off[f][I.f]);
          c.push_back((uint8_t)Bc::Jmp);
          bc_put_u32(c, I.t == NONE ? off[f][b] : off[f][I.t]);
          break;
        case K::Print:
          c.push_back((uint8_t)Bc::Load);
          bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::Print);
          break;
        case K::Alloc:
          c.push_back((uint8_t)Bc::Load);
          bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::ArrNew);
          c.push_back((uint8_t)Bc::Store);
          bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Load:
          c.push_back((uint8_t)Bc::Load);
          bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::Load);
          bc_put_u32(c, slot_id(fns[f], I.b2));
          c.push_back((uint8_t)Bc::ArrLd);
          c.push_back((uint8_t)Bc::Store);
          bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Store:
          c.push_back((uint8_t)Bc::Load);
          bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::Load);
          bc_put_u32(c, slot_id(fns[f], I.b2));
          c.push_back((uint8_t)Bc::Load);
          bc_put_u32(c, slot_id(fns[f], I.c2));
          c.push_back((uint8_t)Bc::ArrSt);
          break;
        case K::Len:
          c.push_back((uint8_t)Bc::Load);
          bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::ArrLen);
          c.push_back((uint8_t)Bc::Store);
          bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Input:
          c.push_back((uint8_t)Bc::In);
          c.push_back((uint8_t)Bc::Store);
          bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Depth:
          c.push_back((uint8_t)Bc::Depth);
          c.push_back((uint8_t)Bc::Store);
          bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Call:
          for (uint32_t av : I.args) { // args pushed in parameter order
            c.push_back((uint8_t)Bc::Load);
            bc_put_u32(c, slot_id(fns[f], av));
          }
          c.push_back((uint8_t)Bc::Call);
          bc_put_u32(c, (uint32_t)I.c);
          bc_put_u32(c, (uint32_t)I.args.size());
          c.push_back((uint8_t)Bc::Store);
          bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Ret:
          if (I.a == NONE) {
            c.push_back((uint8_t)Bc::Push);
            bc_put_i64(c, 0);
          } else {
            c.push_back((uint8_t)Bc::Load);
            bc_put_u32(c, slot_id(fns[f], I.a));
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

// ============================================================================
// 9h2. steppable VM state — the debugger's live view. vm_run() below is the
//      historical one-shot API (a thin loop over vm_step_op); VmCtx + vm_step
//      expose the machine single-op-at-a-time so an interactive session can
//      watch every slot, the operand stack, the arena, and the pc move.
// ============================================================================
struct VmCtx {
  const std::vector<uint8_t> *c{};        // the bytecode image (not owned)
  size_t pc{};                            // next op to fetch
  std::vector<std::vector<val_t>> frames; // one slot vector per activation
  std::vector<uint32_t> rets;             // return pcs of suspended callers
  std::vector<uint32_t> fr_fn;            // fn index of each frame (debugger)
  std::vector<val_t> st{};                // the operand stack
  RunOut R{}; // output + arena + input tape position
  uint64_t steps{}, step_limit{};
  bool started{}, halted{}, trapped{}, timeout{};
  // which function is pc in? (bytecode layout: [u32 nfns][nfns x (u32 off,
  // u32 nslots)][code...]; fns[0] = main body)
  constexpr uint32_t fn_at(size_t p) const {
    uint32_t nf = bc_rd_u32(*c, 0);
    uint32_t best = 0;
    for (uint32_t f = 0; f < nf; ++f) {
      uint32_t off = bc_rd_u32(*c, 4 + (size_t)8 * f);
      if (off <= p)
        best = f;
    }
    return best;
  }
  constexpr uint32_t fn_off(uint32_t f) const {
    return bc_rd_u32(*c, 4 + (size_t)8 * f);
  }
  constexpr uint32_t fn_slots(uint32_t f) const {
    return bc_rd_u32(*c, 4 + (size_t)8 * f + 4);
  }
  constexpr uint32_t nfns() const { return bc_rd_u32(*c, 0); }
};

constexpr bool vm_init(VmCtx &S, const std::vector<uint8_t> &c,
                       uint64_t step_limit, const std::vector<val_t> &ins) {
  S = VmCtx{};
  S.c = &c;
  S.R.ins = ins;
  S.step_limit = step_limit;
  S.started = true;
  if (c.size() < 4) {
    S.trapped = true;
    S.R.trapped = true;
    return false;
  }
  const uint32_t nfns = bc_rd_u32(c, 0);
  if (!nfns || c.size() < 4 + (size_t)8 * nfns) {
    S.trapped = true;
    S.R.trapped = true;
    return false;
  }
  S.frames.push_back(std::vector<val_t>(S.fn_slots(0), 0));
  S.fr_fn.push_back(0);
  S.pc = S.fn_off(0);
  return true;
}
// run ONE bytecode op; false => the machine stopped (see S.halted/.trapped/
// .timeout). "steps" count IR-equivalent instructions (Store/Jmp/Jz/Print/
// Halt/Call/Ret/ArrSt), not sub-ops, so one step limit means the same thing
// for interpreter and VM.
constexpr bool vm_step_op(VmCtx &S) {
  if (!S.started || S.halted || S.trapped || S.timeout)
    return false;
  const std::vector<uint8_t> &c = *S.c;
  if (S.pc >= c.size()) {
    S.trapped = true;
    S.R.trapped = true;
    return false;
  }
  RunOut &R = S.R;
  Bc op = (Bc)c[S.pc++];
  switch (op) {
  case Bc::Halt:
    if (++S.steps > S.step_limit) {
      S.timeout = true;
      R.timeout = true;
      return false;
    }
    S.halted = true;
    return false;
  case Bc::Push:
    S.st.push_back(bc_rd_i64(c, S.pc));
    S.pc += 8;
    break;
  case Bc::Load:
    S.st.push_back(S.frames.back()[bc_rd_u32(c, S.pc)]);
    S.pc += 4;
    break;
  case Bc::Store: {
    if (++S.steps > S.step_limit) {
      S.timeout = true;
      R.timeout = true;
      return false;
    }
    val_t v = S.st.back();
    S.st.pop_back();
    S.frames.back()[bc_rd_u32(c, S.pc)] = v;
    S.pc += 4;
    break;
  }
  case Bc::Call: {
    if (++S.steps > S.step_limit) {
      S.timeout = true;
      R.timeout = true;
      return false;
    }
    uint32_t g = bc_rd_u32(c, S.pc);
    S.pc += 4;
    uint32_t na = bc_rd_u32(c, S.pc);
    S.pc += 4;
    if (g >= S.nfns() || S.frames.size() > MJZ_MAX_CALL_DEPTH) {
      S.trapped = true; // bad callee / recursion depth cap (512 CALLEE
      R.trapped = true; // activations; main's frame does not count)
      return false;
    }
    std::vector<val_t> args(na, 0); // pop na args (a0 pushed first)
    for (uint32_t k = na; k-- > 0;) {
      if (S.st.empty()) {
        S.trapped = true;
        R.trapped = true;
        return false;
      }
      args[k] = S.st.back();
      S.st.pop_back();
    }
    S.rets.push_back((uint32_t)S.pc);
    std::vector<val_t> fr(S.fn_slots(g), 0);
    for (uint32_t k = 0; k < na && k < fr.size(); ++k)
      fr[k] = args[k];
    S.frames.push_back(std::move(fr));
    S.fr_fn.push_back(g);
    S.pc = S.fn_off(g);
    break;
  }
  case Bc::Ret: {
    if (++S.steps > S.step_limit) {
      S.timeout = true;
      R.timeout = true;
      return false;
    }
    if (S.rets.empty() || S.st.empty() || S.frames.size() < 2) {
      S.trapped = true;
      R.trapped = true;
      return false;
    }
    val_t v = S.st.back();
    S.st.pop_back();
    S.frames.pop_back();
    S.fr_fn.pop_back();
    S.pc = S.rets.back();
    S.rets.pop_back();
    S.st.push_back(v); // the caller's Store pops it into the result slot
    break;
  }
  case Bc::Add:
  case Bc::Sub:
  case Bc::Mul:
  case Bc::Div:
  case Bc::Mod:
  case Bc::Lt:
  case Bc::Le:
  case Bc::Gt:
  case Bc::Ge:
  case Bc::Eq:
  case Bc::Ne:
  case Bc::And:
  case Bc::Or:
  case Bc::Xor:
  case Bc::Shl:
  case Bc::Lshr:
  case Bc::Asr:
  case Bc::MulHi: {
    if (S.st.size() < 2) {
      S.trapped = true;
      R.trapped = true;
      return false;
    }
    val_t y = S.st.back();
    S.st.pop_back();
    val_t x = S.st.back();
    S.st.pop_back();
    Rv v{};
    switch (op) {
    case Bc::Add:
      v = w_add(x, y);
      break;
    case Bc::Sub:
      v = w_sub(x, y);
      break;
    case Bc::Mul:
      v = w_mul(x, y);
      break;
    case Bc::Div:
      v = w_div(x, y);
      break;
    case Bc::Mod:
      v = w_mod(x, y);
      break;
    case Bc::Lt:
      v = {x < y, false};
      break;
    case Bc::Le:
      v = {x <= y, false};
      break;
    case Bc::Gt:
      v = {x > y, false};
      break;
    case Bc::Ge:
      v = {x >= y, false};
      break;
    case Bc::Eq:
      v = {x == y, false};
      break;
    case Bc::Ne:
      v = {x != y, false};
      break;
    case Bc::And:
      v = w_and(x, y);
      break;
    case Bc::Or:
      v = w_or(x, y);
      break;
    case Bc::Xor:
      v = w_xor(x, y);
      break;
    case Bc::Shl:
      v = w_shl(x, y);
      break;
    case Bc::Lshr:
      v = w_lshr(x, y);
      break;
    case Bc::Asr:
      v = w_ashr(x, y);
      break;
    case Bc::MulHi:
      v = w_mulhi(x, y);
      break;
    default:
      break;
    }
    if (v.trap) {
      S.trapped = true;
      R.trapped = true;
      return false;
    }
    S.st.push_back(v.v);
    break;
  }
  case Bc::Jmp:
    if (++S.steps > S.step_limit) {
      S.timeout = true;
      R.timeout = true;
      return false;
    }
    S.pc = bc_rd_u32(c, S.pc);
    break;
  case Bc::Jz: {
    if (++S.steps > S.step_limit) {
      S.timeout = true;
      R.timeout = true;
      return false;
    }
    val_t v = S.st.back();
    S.st.pop_back();
    uint32_t t = bc_rd_u32(c, S.pc);
    S.pc += 4;
    if (v == 0)
      S.pc = t;
    break;
  }
  case Bc::Print: {
    if (++S.steps > S.step_limit) {
      S.timeout = true;
      R.timeout = true;
      return false;
    }
    val_t v = S.st.back();
    S.st.pop_back();
    R.out.push_back(v);
    break;
  }
  case Bc::ArrNew: { // pop n -> push fresh handle
    val_t n = S.st.back();
    S.st.pop_back();
    Rv r = R.arena_alloc(n);
    if (r.trap) {
      S.trapped = true;
      R.trapped = true;
      return false;
    }
    S.st.push_back(r.v);
    break;
  }
  case Bc::ArrLd: { // pop i, pop h -> push h[i]
    if (S.st.size() < 2) {
      S.trapped = true;
      R.trapped = true;
      return false;
    }
    val_t i = S.st.back();
    S.st.pop_back();
    val_t h = S.st.back();
    S.st.pop_back();
    Rv r = R.arena_load(h, i);
    if (r.trap) {
      S.trapped = true;
      R.trapped = true;
      return false;
    }
    S.st.push_back(r.v);
    break;
  }
  case Bc::ArrSt: { // pop v, pop i, pop h -> h[i] = v   (1 IR step)
    if (++S.steps > S.step_limit) {
      S.timeout = true;
      R.timeout = true;
      return false;
    }
    if (S.st.size() < 3) {
      S.trapped = true;
      R.trapped = true;
      return false;
    }
    val_t v = S.st.back();
    S.st.pop_back();
    val_t i = S.st.back();
    S.st.pop_back();
    val_t h = S.st.back();
    S.st.pop_back();
    if (!R.arena_store(h, i, v)) {
      S.trapped = true;
      R.trapped = true;
      return false;
    }
    break;
  }
  case Bc::ArrLen: { // pop h -> push len(h)
    val_t h = S.st.back();
    S.st.pop_back();
    S.st.push_back(R.arena_len(h));
    break;
  }
  case Bc::In: // push next stdin value (0 at EOF)
    S.st.push_back(R.next_input());
    break;
  case Bc::Depth: // frame count incl. main — same convention as every
    S.st.push_back((val_t)S.frames.size()); // runner (SSA interp: stack.size)
    break;
  }
  return true;
}

constexpr RunOut vm_run(const std::vector<uint8_t> &c,
                        uint64_t step_limit = 200000,
                        const std::vector<val_t> &ins = {}) {
  VmCtx S{};
  if (!vm_init(S, c, step_limit, ins))
    return S.R;
  while (vm_step_op(S)) {
  }
  return S.R;
}
// ============================================================================
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
  if (fns.empty() || fns[0].nblocks() == 0) {
    R.trapped = true;
    return R;
  }
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
    stack.push_back({0, std::vector<val_t>(nv0, 0), std::vector<char>(nv0, 0)});
  }
  uint64_t steps = 0;
  uint32_t cur_fn = 0, cur_blk = 0;
  int64_t i = (int64_t)fns[0].blocks[0].begin;
  for (;;) {
    if (++steps > step_limit) {
      R.timeout = true;
      return R;
    }
    if (cur_fn >= fns.size()) {
      R.trapped = true;
      return R;
    }
    const Fn &F = fns[cur_fn];
    if (cur_blk >= F.nblocks() || i < (int64_t)F.blocks[cur_blk].begin ||
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
      uint32_t next =
          (I.k == K::Br) ? I.t : ((stack.back().val[I.a] != 0) ? I.t : I.f);
      if (next >= F.nblocks()) {
        R.trapped = true;
        return R;
      }
      // stage phi values of `next` for this edge: read all, then write all
      std::vector<std::pair<uint32_t, val_t>> staged{};
      for (uint32_t j = F.blocks[next].begin; j < F.blocks[next].end; ++j) {
        const Instr &P = F.code[j];
        if (P.k != K::Phi)
          break;
        uint32_t arg = NONE;
        if (P.t == cur_blk && P.a != NONE)
          arg = P.a;
        else if (P.f == cur_blk && P.b2 != NONE)
          arg = P.b2;
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
        std::fprintf(
            stderr, "OOB const: fn=%u blk=%u dst=%u valsize=%zu nfns=%zu\n",
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
      case B::Add:
        r = w_add(x, y);
        break;
      case B::Sub:
        r = w_sub(x, y);
        break;
      case B::Mul:
        r = w_mul(x, y);
        break;
      case B::Div:
        r = w_div(x, y);
        break;
      case B::Mod:
        r = w_mod(x, y);
        break;
      case B::Lt:
        r = {x < y, false};
        break;
      case B::Le:
        r = {x <= y, false};
        break;
      case B::Gt:
        r = {x > y, false};
        break;
      case B::Ge:
        r = {x >= y, false};
        break;
      case B::Eq:
        r = {x == y, false};
        break;
      case B::Ne:
        r = {x != y, false};
        break;
      case B::And:
        r = w_and(x, y);
        break;
      case B::Or:
        r = w_or(x, y);
        break;
      case B::Xor:
        r = w_xor(x, y);
        break;
      case B::Shl:
        r = w_shl(x, y);
        break;
      case B::LShr:
        r = w_lshr(x, y);
        break;
      case B::AShr:
        r = w_ashr(x, y);
        break;
      case B::MulHi:
        r = w_mulhi(x, y);
        break;
      }
      if (r.trap) {
        R.trapped = true;
        return R;
      }
      stack.back().val[I.dst] = r.v;
      stack.back().has[I.dst] = 1;
      break;
    }
    case K::Print:
      R.out.push_back(stack.back().val[I.a]);
      break;
    case K::Alloc: {
      Rv r = R.arena_alloc(stack.back().val[I.a]);
      if (r.trap) {
        R.trapped = true;
        return R;
      }
      stack.back().val[I.dst] = r.v;
      stack.back().has[I.dst] = 1;
      break;
    }
    case K::Load: {
      Rv r = R.arena_load(stack.back().val[I.a], stack.back().val[I.b2]);
      if (r.trap) {
        R.trapped = true;
        return R;
      }
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

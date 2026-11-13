#pragma once
// === vm_step.hpp ========================================
// the steppable bytecode machine (split from vm.hpp, checkpoint 21 —
// the vm/steppable/interpreter tripart): VmCtx + vm_init + vm_step_op +
// vm_run. Depends on the bytecode definition layer (bc_defs.hpp) and the
// IR — standalone-compilable on its own; vm.hpp includes this after the
// same layer, so the parent-child order preserves the historical
// definition order exactly (all includers unchanged).
//
// ALGORITHM:
//   * vm_init(S, c, step_limit, ins): validate the header, push main's
//     zeroed frame of fn_slots(0) slots, set pc = fn_off(0).
//   * vm_step_op(S): fetch and run ONE opcode -- Push/Load/ArrLen/In/
//     Depth push; Store pops into frames.back()[slot]; the 18 binops
//     pop y, x and push the w_* wrap/trap result; Jmp/Jz rewrite pc;
//     Call pops na args, pushes a callee frame with them in slots
//     0..na-1 and saves the return pc on rets (cap
//     MJZ_MAX_CALL_DEPTH = 512 callee activations); Ret pops a frame
//     and re-pushes the value for the caller's Store; Bkpt counts one
//     step, then skips its operand bytes.
//   * vm_run: the historical one-shot API -- vm_init plus a while loop
//     over vm_step_op, returning S.R.
//
// DATA STRUCTURES: VmCtx{c, pc, frames (slot vector per activation),
// rets, fr_fn (debugger), st (operand stack), R (output + arena +
// input tape), steps/step_limit, started/halted/trapped/timeout};
// fn_at/fn_off/fn_slots/nfns re-read the directory; "steps" count
// IR-equivalent ops (Store/Jmp/Jz/Print/Call/Ret/ArrSt/Halt/Bkpt),
// so one limit means the same as in the SSA interpreter.
//
// THEORY: the machine must be steppable because the debugger must
// observe state BETWEEN ops: stops at Bkpt slots and line breakpoints
// (the per-byte srcs map), and value-change watchpoints re-resolve
// the watched names after EVERY step -- vm_step_op exposes pc/frames/
// st/arena for that stop/inspect/resume cycle (vm_run cannot).
//
// CONSUMERS: dbg_sess.hpp (run_bounded + watch_step) and dbg_support
// drive VmCtx directly; cli_run.hpp and the tours use vm_run.

#include "bc_defs.hpp"

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
  case Bc::Bkpt: { // observation point: counts as one IR step (matches
    if (++S.steps > S.step_limit) { // the interpreters), then skipped
      S.timeout = true;
      R.timeout = true;
      return false;
    }
    S.pc += 4 + 4 * (size_t)bc_rd_u32(c, S.pc);
    break;
  }
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

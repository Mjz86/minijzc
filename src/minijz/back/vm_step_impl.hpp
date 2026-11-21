#pragma once
// === vm_step_impl.hpp ========================================
// the vm_init / vm_step_op / vm_run bodies (see vm_step.hpp for the
// machine's contract + theory). Included by vm_step.hpp in the header-only
// build (constexpr, via MJZ_RUNNER_FN) and by tu/runners.cpp in the multi-TU
// build (plain external linkage) -- include it nowhere else.

#include "vm_step.hpp"

MJZ_RUNNER_FN bool vm_init(VmCtx &S, const std::vector<uint8_t> &c,
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

MJZ_RUNNER_FN bool vm_step_op(VmCtx &S) {
  if (!S.started || S.halted || S.trapped || S.timeout)
    return false;
  const std::vector<uint8_t> &c = *S.c;
  // the uniform stop every opcode shares: trap set, step fails. The
  // eleven hand-rolled copies collapsed in the ckpt-33 dedup.
  auto trap_out = [&]() -> bool {
    S.trapped = true;
    S.R.trapped = true;
    return false;
  };
  if (S.pc >= c.size())
    return trap_out();
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
    if (g >= S.nfns() || S.frames.size() > MJZ_MAX_CALL_DEPTH)
      // bad callee / recursion depth cap (512 CALLEE activations;
      // main's frame does not count)
      return trap_out();
    std::vector<val_t> args(na, 0); // pop na args (a0 pushed first)
    for (uint32_t k = na; k-- > 0;) {
      if (S.st.empty())
        return trap_out();
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
    if (S.rets.empty() || S.st.empty() || S.frames.size() < 2)
      return trap_out();
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
    if (S.st.size() < 2)
      return trap_out();
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
    if (v.trap)
      return trap_out();
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
    if (r.trap)
      return trap_out();
    S.st.push_back(r.v);
    break;
  }
  case Bc::ArrLd: { // pop i, pop h -> push h[i]
    if (S.st.size() < 2)
      return trap_out();
    val_t i = S.st.back();
    S.st.pop_back();
    val_t h = S.st.back();
    S.st.pop_back();
    Rv r = R.arena_load(h, i);
    if (r.trap)
      return trap_out();
    S.st.push_back(r.v);
    break;
  }
  case Bc::ArrSt: { // pop v, pop i, pop h -> h[i] = v   (1 IR step)
    if (++S.steps > S.step_limit) {
      S.timeout = true;
      R.timeout = true;
      return false;
    }
    if (S.st.size() < 3)
      return trap_out();
    val_t v = S.st.back();
    S.st.pop_back();
    val_t i = S.st.back();
    S.st.pop_back();
    val_t h = S.st.back();
    S.st.pop_back();
    if (!R.arena_store(h, i, v))
      return trap_out();
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

MJZ_RUNNER_FN RunOut vm_run(const std::vector<uint8_t> &c, uint64_t step_limit,
                            const std::vector<val_t> &ins) {
  VmCtx S{};
  if (!vm_init(S, c, step_limit, ins))
    return S.R;
  while (vm_step_op(S)) {
  }
  return S.R;
}

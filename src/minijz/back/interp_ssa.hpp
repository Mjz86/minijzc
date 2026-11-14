#pragma once
// === interp_ssa.hpp ========================================
// the SSA interpreter (split from vm.hpp, checkpoint 21 — the
// vm/steppable/interpreter tripart): run_prog with an explicit frame
// stack — Call pushes a frame (params bound from the operand-side
// argument vregs), Ret pops back to the caller's resume point and
// stores the value into the result slot. Phi resolution by pred
// identity via t/f. Depends only on the IR + RunOut (ir/instr.hpp
// brings both), not on the bytecode layer — standalone-compilable.
//
// THEORY (executing SSA directly -- no destruction, no slots):
//   * FRAMES ARE ACTIVATIONS. Each Call pushes {fn, val[], has[]}
//     -- val is indexed by VREG ID (SSA values get a home per
//     activation; no register allocation needed), has[] tracks
//     definedness (undef reads are the interpreter's safety net).
//     Ret pops to the saved Resume point and writes the return
//     value into the caller's result slot (resumes is a parallel
//     stack of {fn, blk, i, dst}).
//   * PHIS RESOLVE ON THE EDGE, in parallel. A Br/Cbr transfer
//     stages the target block's phis FIRST: read every arg keyed
//     to the CURRENT block (t/f), then write all dsts -- the
//     read-all-then-write-all discipline is exactly destruct's
//     parallel-copy semantics (a phi is a simultaneous assignment
//     on the edge). Phis are skipped at +i (K::Phi case): by the
//     time control reaches them they have already been staged.
//   * SEMANTICS = THE RUNNER PARITY CHAIN. This is one of the
//     --check-all runners: AST oracle (astrun) vs THIS SSA
//     interpreter vs lowered C vs the bytecode VM must all agree
//     -- the five-way agreement is the compiler's correctness
//     oracle, so the interpreter mirrors the wrapping arithmetic
//     (w_*), the arena traps (with located trap_src), the 512
//     callee depth cap (stack.size() counts main, hence the
//     strict >), the step_limit fuel, and K::Depth = frame count
//     including main.

#include "../ir/instr.hpp"

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
        R.trap_src = I.src;
        R.trap_kind = "corrupt branch target";
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
        R.trapped = true;
        R.trap_src = I.src; // return with no caller (main has no Ret)
        R.trap_kind = "return without caller";
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
        R.trapped = true;
        R.trap_src = I.src; // recursion depth cap (matches native guards)
        R.trap_kind = "recursion depth exceeded";
        return R;
      }
      uint32_t g = (uint32_t)I.c;
      if (g >= fns.size() || I.args.size() != fns[g].nparams) {
        R.trapped = true;
        R.trap_src = I.src;
        R.trap_kind = "bad call";
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
        R.trap_src = I.src;
        R.trap_kind = "division by zero";
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
        R.trap_src = I.src;
        R.trap_kind = "invalid array size";
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
        R.trap_src = I.src;
        R.trap_kind = "index out of bounds";
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
        R.trap_src = I.src;
        R.trap_kind = "index out of bounds";
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
    case K::Bkpt: // observation point: no output, no state change (the
      break;      // debugger VM stops on the Bc::Bkpt op before it)
    }
    ++i;
  }
}

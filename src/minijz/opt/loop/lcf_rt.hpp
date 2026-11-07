#pragma once
// === lcf_rt.hpp ========================================
// RUNTIME trip counts: guarded/unguarded closed forms with the wrap
// fallback (header phis re-keyed to the new entry edge).

#include "minijz/base/pass_cfg.hpp"
#include "minijz/ir/instr.hpp"
#include "minijz/opt/analysis/an.hpp"
#include "minijz/opt/linear/linform.hpp"

// ============================================================================
// 9e3. CONST-CALL EVALUATION (interprocedural)
// ============================================================================
// A call whose arguments are all compile-time constants and whose callee (and
// every fn that callee transitively calls) is PURE ARITHMETIC -- only
// Const/Copy/Bin/Br/Cbr/Phi/Call/Ret, so no Print, no input(), no arena
// (Alloc/Load/Store/Len), no observable side effect and no handle value -- is
// evaluated exactly at compile time with the same wrapping/trapping arithmetic
// the runtime uses (a trap, the 512-frame recursion cap, or the step budget
// aborts the attempt and keeps the runtime call: a runtime TRAP is
// observable, so folding it away would be a miscompile). This is what turns
// `fact(4)` and the mutually recursive `is_even(10)` into constants:
// recursion is exactly why inlining can never touch them, and exactly why a
// whole-program evaluator can -- the recursive call graph is just a
// control-flow graph the evaluator walks.

// call depth cap shared by EVERY runner (interpreters, VM, emitted C,
// and emitted asm all enforce the same limit: runaway recursion TRAPs
// everywhere) -- also enforced by the compile-time evaluator below, so a
// program that would TRAP at depth 512 is never silently folded
inline constexpr uint32_t MJZ_MAX_CALL_DEPTH = 512;
// step budget for one const-call evaluation attempt (shared across the
// whole recursive invocation; generous: the demo programs need < 200)
inline constexpr uint64_t MJZ_CONSTCALL_STEPS = 1ull << 20;

// fn-local purity: every instruction is pure arithmetic / control flow
constexpr bool fn_local_pure(const Fn &f) {
  for (const Instr &I : f.code)
    switch (I.k) {
    case K::Const:
    case K::Copy:
    case K::Bin:   // Div/Mod may trap -> the EVALUATOR aborts on real traps
    case K::Br:
    case K::Cbr:
    case K::Phi:
    case K::Depth: // frame count: no observable effect; the VALUE depends
                   // on the calling context, which opt_constcall controls
    case K::Call:  // nested calls are checked transitively
    case K::Ret:
      break;
    default:
      return false; // Print/Input/Alloc/Load/Store/Len/Exit: observable
    }
  return true;
}
// does the call closure of fi contain a K::Depth (frame-sensitive value)?
constexpr bool closure_has_depth(const std::vector<Fn> &fns, uint32_t fi) {
  const uint32_t n = (uint32_t)fns.size();
  std::vector<uint8_t> seen(n, 0);
  std::vector<uint32_t> work{fi};
  seen[fi] = 1;
  while (!work.empty()) {
    uint32_t f = work.back();
    work.pop_back();
    for (const Instr &I : fns[f].code) {
      if (I.k == K::Depth) return true;
      if (I.k == K::Call) {
        uint32_t g = (uint32_t)I.c;
        if (g < n && !seen[g]) {
          seen[g] = 1;
          work.push_back(g);
        }
      }
    }
  }
  return false;
}
// transitive purity over the call graph (cycle-safe DFS)
constexpr bool fn_pure_closure(const std::vector<Fn> &fns, uint32_t fi) {
  const uint32_t n = (uint32_t)fns.size();
  std::vector<uint8_t> seen(n, 0);
  std::vector<uint32_t> work{fi};
  seen[fi] = 1;
  while (!work.empty()) {
    uint32_t f = work.back();
    work.pop_back();
    if (!fn_local_pure(fns[f])) return false;
    for (const Instr &I : fns[f].code)
      if (I.k == K::Call) {
        uint32_t g = (uint32_t)I.c;
        if (g < n && !seen[g]) {
          seen[g] = 1;
          work.push_back(g);
        }
      }
  }
  return true;
}

// exact SSA execution of one pure fn; mirrors run_prog's semantics: phis are
// resolved on the edge they came from (read all, then write all), the env is
// flat with default 0 ("storage is zero-initialized"), Ret yields val[a].
constexpr bool eval_pure_fn(const std::vector<Fn> &fns, uint32_t fi,
                            const std::vector<val_t> &args, val_t &out,
                            uint32_t depth, uint64_t &steps) {
  if (depth >= MJZ_MAX_CALL_DEPTH) return false; // runtime would TRAP
  const Fn &F = fns[fi];
  if (F.nblocks() == 0) return false;
  const uint32_t nv = F.nvars + (uint32_t)F.version_base.size();
  std::vector<val_t> val(nv, 0);
  for (size_t k = 0; k < args.size() && k < F.nparams; ++k)
    if (k < nv) val[k] = args[k];
  uint32_t blk = 0;
  uint32_t pc = F.blocks[0].begin;
  for (;;) {
    if (++steps > MJZ_CONSTCALL_STEPS) return false; // budget exhausted
    if (blk >= F.nblocks() || pc >= F.blocks[blk].end) return false;
    const Instr &I = F.code[pc];
    switch (I.k) {
    case K::Phi: // resolved on the incoming edge, never inline
      ++pc;
      continue;
    case K::Depth: // eval's `depth` = activations-1; frames incl. main =
      // depth+2 (main=1; the callee called from main sits at frame 2)
      if (I.dst >= nv) return false;
      val[I.dst] = (val_t)depth + 2;
      ++pc;
      continue;
    case K::Const:
      if (I.dst >= nv) return false;
      val[I.dst] = I.c;
      ++pc;
      continue;
    case K::Copy:
      if (I.dst >= nv || I.a >= nv) return false;
      val[I.dst] = val[I.a];
      ++pc;
      continue;
    case K::Bin: {
      if (I.a >= nv || I.b2 >= nv || I.dst >= nv) return false;
      Rv r = w_bin(I.b, val[I.a], val[I.b2]);
      if (r.trap) return false; // runtime trap is observable: keep the call
      val[I.dst] = r.v;
      ++pc;
      continue;
    }
    case K::Br:
    case K::Cbr: {
      uint32_t next = (I.k == K::Br)
                          ? I.t
                          : ((val[I.a] != 0) ? I.t : I.f);
      if (next >= F.nblocks()) return false;
      // stage the successor's phis for THIS edge: read all, then write all
      std::vector<std::pair<uint32_t, val_t>> staged{};
      for (uint32_t j = F.blocks[next].begin; j < F.blocks[next].end; ++j) {
        const Instr &P = F.code[j];
        if (P.k != K::Phi) break;
        uint32_t arg = NONE;
        if (P.t == blk && P.a != NONE) arg = P.a;
        else if (P.f == blk && P.b2 != NONE) arg = P.b2;
        if (arg != NONE && arg < nv && P.dst < nv)
          staged.push_back({P.dst, val[arg]});
      }
      for (const auto &st : staged) val[st.first] = st.second;
      blk = next;
      pc = F.blocks[blk].begin;
      continue;
    }
    case K::Ret:
      out = (I.a != NONE && I.a < nv) ? val[I.a] : 0;
      return true;
    case K::Call: {
      std::vector<val_t> cargs{};
      for (uint32_t a : I.args) {
        if (a >= nv) return false;
        cargs.push_back(val[a]);
      }
      val_t r{};
      if (!eval_pure_fn(fns, (uint32_t)I.c, cargs, r, depth + 1, steps))
        return false;
      if (I.dst < nv) val[I.dst] = r;
      ++pc;
      continue;
    }
    default:
      return false; // impure instruction: impossible after the purity check
    }
  }
}

// fold every constant-argument call to a provably-pure callee
constexpr bool opt_constcall(std::vector<Fn> &fns,
                             std::vector<OptStats> &ostats) {
  const uint32_t n = (uint32_t)fns.size();
  if (n < 2 || ostats.size() < n) return false;
  std::vector<uint8_t> pure(n, 0);
  for (uint32_t f = 0; f < n; ++f)
    pure[f] = fn_pure_closure(fns, f) ? 1 : 0;
  bool changed = false;
  for (uint32_t f = 0; f < n; ++f) {
    Fn &F = fns[f];
    const uint32_t nv = F.nvars + (uint32_t)F.version_base.size();
    if (!nv || F.nblocks() == 0) continue;
    // def index (SSA: one def per vreg) + bounded const-value walk
    std::vector<uint32_t> def_at(nv, NONE);
    for (uint32_t b = 0; b < F.nblocks(); ++b)
      for (uint32_t i = F.blocks[b].begin; i < F.blocks[b].end; ++i) {
        const Instr &I = F.code[i];
        if (I.dst != NONE && I.dst < nv) def_at[I.dst] = i;
      }
    auto cval = [&](uint32_t v, val_t &out) -> bool {
      for (int d = 0; d < 16; ++d) { // bounded Copy/phi-of-same chains
        if (v == NONE || v >= nv) return false;
        uint32_t at = def_at[v];
        if (at == NONE) return false;
        const Instr &D = F.code[at];
        if (D.k == K::Const) {
          out = D.c;
          return true;
        }
        if (D.k == K::Copy && D.a != NONE) {
          v = D.a;
          continue;
        }
        if (D.k == K::Phi && D.a != NONE && D.a == D.b2) {
          v = D.a;
          continue;
        }
        return false;
      }
      return false;
    };
    for (uint32_t b = 0; b < F.nblocks(); ++b)
      for (uint32_t i = F.blocks[b].begin; i < F.blocks[b].end; ++i) {
        Instr &I = F.code[i];
        if (I.k != K::Call) continue;
        uint32_t g = (uint32_t)I.c;
        if (g >= n || !pure[g] || I.args.size() != fns[g].nparams) continue;
        std::vector<val_t> args{};
        bool allc = true;
        for (uint32_t a : I.args) {
          val_t v{};
          if (!cval(a, v)) {
            allc = false;
            break;
          }
          args.push_back(v);
        }
        if (!allc) continue;
        val_t out{};
        uint64_t steps = 0;
        // Frame-exactness rules for the evaluator's recursion cap and its
        // K::Depth values: a call site IN MAIN is exact (the callee's chain
        // starts at frame 2, always). A call site in any other fn runs at
        // an UNKNOWN frame X >= 2: refuse if the callee chain READS the
        // frame count, and evaluate with an initial depth of CAP-1 so only
        // non-nesting evaluations (a single activation, which can never
        // hit the cap) fold — a longer chain's cap point depends on X.
        bool in_main = (f == 0);
        uint32_t d0 = 0;
        if (!in_main) {
          if (closure_has_depth(fns, g)) continue;
          d0 = MJZ_MAX_CALL_DEPTH - 1;
        }
        if (!eval_pure_fn(fns, g, args, out, d0, steps)) continue;
        // same dst: every user reads the constant directly
        I.k = K::Const;
        I.b = B::Add;
        I.a = NONE;
        I.b2 = NONE;
        I.t = NONE;
        I.f = NONE;
        I.c = out;
        I.c2 = NONE;
        I.nt = false;
        I.args.clear();
        ostats[f].constcalls++;
        changed = true;
      }
  }
  return changed;
}

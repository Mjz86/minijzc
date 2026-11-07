// ============================================================================
// MiniJZ compiler — §9e3 const-call, §9e4 tail-call elimination, §9e5 countdown closed form, §9e6 if-conversion
// ----------------------------------------------------------------------------
// Ordered-include header: this file is a slice of the original single
// translation unit and relies on being included after its predecessors
// (see src/minijz.hpp for the canonical order). Do not include it
// directly in an arbitrary order.
// ============================================================================
#pragma once

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

struct CallGraph {
  std::vector<std::vector<ulen>> adj{}; // caller -> callees (with dups)
  std::vector<uint32_t> scc_of{};       // fn -> scc index
  std::vector<uint32_t> scc_begin{};    // scc -> start in members
  std::vector<uint32_t> members{};      // concatenated, scc by scc
  uint32_t nscc{};
  std::vector<char> recursive{};
};

constexpr CallGraph build_callgraph(const std::vector<Fn> &fns) {
  CallGraph G{};
  const uint32_t n = (uint32_t)fns.size();
  G.adj.assign(n, {});
  for (uint32_t f = 0; f < n; ++f)
    for (const Instr &I : fns[f].code)
      if (I.k == K::Call) {
        uint32_t c = (uint32_t)I.c;
        if (c < n) G.adj[f].push_back(c);
      }
  auto fs = gns::make_basic_forest<V>(G.adj);
  auto res = gns::calculate_strongly_connected_components<V>(fs);
  G.nscc = (uint32_t)res.nodes_index.size();
  G.members.assign(res.edges.begin(), res.edges.end());
  G.scc_begin.assign(G.nscc, 0);
  for (uint32_t s = 0; s < G.nscc; ++s)
    G.scc_begin[s] = (uint32_t)res.nodes_index[s];
  G.scc_of.assign(n, NONE);
  for (uint32_t s = 0; s < G.nscc; ++s) {
    uint32_t e = (s + 1 < G.nscc) ? G.scc_begin[s + 1]
                                  : (uint32_t)G.members.size();
    for (uint32_t j = G.scc_begin[s]; j < e; ++j)
      G.scc_of[G.members[j]] = s;
  }
  G.recursive.assign(n, 0);
  for (uint32_t f = 0; f < n; ++f) {
    if (G.scc_of[f] == NONE) continue;
    uint32_t s = G.scc_of[f];
    uint32_t e = (s + 1 < G.nscc) ? G.scc_begin[s + 1]
                                  : (uint32_t)G.members.size();
    if (e - G.scc_begin[s] > 1) G.recursive[f] = 1; // mutual recursion
    for (ulen c : G.adj[f])
      if ((uint32_t)c == f) G.recursive[f] = 1;     // self recursion
  }
  return G;
}

// ============================================================================
// 9e4. TAIL-CALL ELIMINATION for inner-SCC tail calls
// ============================================================================
// A tail call is a Call whose result flows DIRECTLY into the enclosing Ret
// (the caller returns the callee's value unchanged): `return f(x);`. Inside
// a call-graph SCC (recursion), such a call needs no return-address: it is
// a GUMP back to the (callee's) entry with fresh arguments. Two shapes:
//
//   * single-member SCC (self tail recursion): the function becomes a LOOP
//     in place — a new entry preheader computes the EXACT remaining
//     recursion budget (MJZ_MAX_CALL_DEPTH + 1 - Depth(): the number of
//     further activations the original chain could still push before the
//     512-frame TRAP), the entry block becomes the loop header with phis
//     for the parameters, and every tail site becomes [budget check ->
//     synthesized trap; args already computed; jump back]. Binary-search
//     style functions (several tail sites) funnel through a binary merge
//     chain of latch blocks, so every phi keeps exactly 2 args.
//
//   * multi-member SCC (mutual tail recursion, e.g. is_even/is_odd): all
//     members merge into ONE function `Fm(sel, a0..a{M-1})` — sel selects
//     the entry member (callers pass the member index; tailrec rewrites
//     every call site itself, so sel in [0,k) is an invariant it records
//     in Fn::tce_sel), the header dispatches on it, and each member's tail
//     call to a sibling becomes [sel' = sibling; args; budget check; jump].
//     The merged function is a leaf w.r.t. the SCC: no recursion remains.
//
// SEMANTIC EXACTNESS: the original chain pushed one activation per tail
// call and TRAPs when the 512-activation cap would be exceeded; the budget
// counter reproduces this bit-for-bit (budget = CAP+1 - Depth() at entry,
// one decrement per tail jump, trap when a jump is needed at budget 0 —
// including the "negative n never reaches the base case" case: the budget
// runs out exactly where the original recursion cap would). The trap is
// synthesized with the existing idiom `t = 1 / 0` (div-by-constant-zero),
// which every backend already emits as `printf("TRAP\n"); exit(1);` and
// which the optimizer already treats as an observable, unfoldable, may-trap
// root. After the transformation the loop passes (unroll, cdfold) can see
// the recursion as the loop it always was.
constexpr uint32_t MJZ_TCE_MAX_MEMBERS = 5;   // sel dispatch chain depth
constexpr uint32_t MJZ_TCE_MAX_SITES = 8;     // tail sites per merge
constexpr uint32_t MJZ_TCE_MAX_ARITY = 5;     // 1+M params must fit SysV's 6
constexpr uint32_t MJZ_TCE_MAX_FNSIZE = 400;  // members stay mergeable

// is the Call at index ci in block b of fn a tail call? (dst used exactly
// once, by the block's terminating Ret, after the call)
constexpr bool is_tail_call(const Fn &fn, uint32_t b, uint32_t ci,
                            const std::vector<uint32_t> &uses) {
  const Instr &I = fn.code[ci];
  if (I.k != K::Call || I.dst == NONE) return false;
  if (I.dst >= uses.size() || uses[I.dst] != 1) return false;
  const uint32_t end = fn.blocks[b].end;
  if (end == 0 || fn.code[end - 1].k != K::Ret) return false;
  if (fn.code[end - 1].a != I.dst) return false;
  return ci + 1 < end; // the call precedes the ret
}

// ---- shared construction helpers -------------------------------------------
// append the recursion-budget preheader instrs; returns the bInit vreg
constexpr uint32_t tce_emit_budget_init(Fn &fn, std::vector<Instr> &out) {
  uint32_t dv = new_anon_vreg(fn, 0);
  out.push_back({.k = K::Depth, .dst = dv});
  uint32_t kc = new_anon_vreg(fn, 0);
  out.push_back({.k = K::Const, .dst = kc, .c = (val_t)MJZ_MAX_CALL_DEPTH + 1});
  uint32_t b0 = new_anon_vreg(fn, 0);
  out.push_back({.k = K::Bin, .b = B::Sub, .dst = b0, .a = kc, .b2 = dv});
  return b0;
}
// append the per-site budget check + decrement into `out`; bL/eq are the
// (pre-created) destination vregs -- pre-creation matters because header
// phis reference bL BEFORE the site block is emitted
constexpr void tce_emit_site_check(Fn &fn, std::vector<Instr> &out,
                                    uint32_t bH, uint32_t bL, uint32_t eq) {
  uint32_t one = new_anon_vreg(fn, 0);
  out.push_back({.k = K::Const, .dst = one, .c = 1});
  out.push_back({.k = K::Bin, .b = B::Sub, .dst = bL, .a = bH, .b2 = one});
  uint32_t cz = new_anon_vreg(fn, 0);
  out.push_back({.k = K::Const, .dst = cz, .c = 0});
  out.push_back({.k = K::Bin, .b = B::Eq, .dst = eq, .a = bH, .b2 = cz});
}
// the synthesized trap block instrs: t = 1 / 0 (the existing idiom)
constexpr void tce_emit_trap(Fn &fn, std::vector<Instr> &out) {
  uint32_t c1 = new_anon_vreg(fn, 0);
  out.push_back({.k = K::Const, .dst = c1, .c = 1});
  uint32_t cz = new_anon_vreg(fn, 0);
  out.push_back({.k = K::Const, .dst = cz, .c = 0});
  uint32_t r = new_anon_vreg(fn, 0);
  out.push_back({.k = K::Bin, .b = B::Div, .dst = r, .a = c1, .b2 = cz});
  out.push_back({.k = K::Ret}); // unreachable after the trap; a terminator
}

// ---- single member: self tail-recursion -> loop in place --------------------
constexpr bool tce_self(Fn &fn, OptStats &os, uint32_t self_idx) {
  const uint32_t nb = fn.nblocks();
  const uint32_t P = fn.nparams;
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nb || !nv) return false;
  // collect tail sites (self calls in tail position)
  struct Site {
    uint32_t b, ci;             // block + call instr index
    std::vector<uint32_t> args; // the call's argument vregs
  };
  std::vector<uint32_t> uses(nv, 0);
  for (const Instr &I : fn.code) {
    auto bump = [&](uint32_t v) {
      if (v != NONE && v < nv) ++uses[v];
    };
    bump(I.a);
    bump(I.b2);
    bump(I.c2);
    for (uint32_t v : I.args) bump(v);
  }
  std::vector<Site> sites;
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      if (fn.code[i].k != K::Call) continue;
      if ((uint32_t)fn.code[i].c != self_idx) continue; // not a self call
      if (fn.code[i].args.size() != P) return false;
      if (!is_tail_call(fn, b, i, uses)) return false;
      Site s;
      s.b = b;
      s.ci = i;
      s.args = fn.code[i].args;
      sites.push_back(std::move(s));
    }
  if (sites.empty() || sites.size() > MJZ_TCE_MAX_SITES) return false;
  const uint32_t k = (uint32_t)sites.size();
  // ---- new block ids -------------------------------------------------------
  //   0        = P0 (the new entry preheader)
  //   ob+1     = every original block (H, the loop header, = old block 0)
  //   nb+1..   = the latch merge chain M_1..M_{k-1} (only when k > 1)
  //   T        = the synthesized trap block
  // site j (0-based) false-arm target: k==1 -> H; j+1<k -> M_{j+1}; else
  // M_{k-1}.  M_j merges (site j, M_{j+1} or site k-1) and jumps to
  // M_{j-1} / H, so the header keeps exactly ONE latch pred.
  const uint32_t H = 1;
  std::vector<uint32_t> Mb(k + 1, NONE); // M_j block id, 1-based
  for (uint32_t j = 1; j < k; ++j) Mb[j] = nb + j;
  const uint32_t T = (k > 1) ? nb + k : nb + 1;
  const uint32_t latch = (k == 1) ? (sites[0].b + 1) : Mb[1];
  // ---- pre-create every fresh vreg id (blocks reference each other) --------
  std::vector<uint32_t> pHv(P, NONE); // header phis for the params
  for (uint32_t p = 0; p < P; ++p)
    pHv[p] = new_anon_vreg(fn, fn.base_of(p));
  const uint32_t bHv = new_anon_vreg(fn, 0); // header phi: the budget
  std::vector<uint32_t> siteBL(k, NONE), siteEQ(k, NONE);
  for (uint32_t j = 0; j < k; ++j) { // PRE-create: the H phis reference
    siteBL[j] = new_anon_vreg(fn, 0); // siteBL[0] before the site is emitted
    siteEQ[j] = new_anon_vreg(fn, 0);
  }
  std::vector<std::vector<uint32_t>> Mp(k + 1,
                                        std::vector<uint32_t>(P, NONE));
  std::vector<uint32_t> Mbv(k + 1, NONE);
  for (uint32_t j = 1; j < k; ++j) {
    for (uint32_t p = 0; p < P; ++p)
      Mp[j][p] = new_anon_vreg(fn, fn.base_of(p));
    Mbv[j] = new_anon_vreg(fn, 0);
  }
  // ---- build the new code --------------------------------------------------
  std::vector<Instr> nc{};
  nc.reserve(fn.code.size() + 12 * k + 4 * P + 16);
  std::vector<Block> nbl{};
  nbl.reserve(nb + k + 1);
  // P0: budget = CAP+1 - Depth(); br H
  uint32_t bInitV = NONE;
  {
    uint32_t base = (uint32_t)nc.size();
    bInitV = tce_emit_budget_init(fn, nc);
    nc.push_back({.k = K::Br, .t = H});
    nbl.push_back({base, (uint32_t)nc.size()});
  }
  // param-use rewrite: any use of param vreg p becomes the header phi pHv[p]
  auto rw = [&](uint32_t v) -> uint32_t {
    if (v != NONE && v < P) return pHv[v];
    return v;
  };
  // original blocks (ids shifted +1)
  for (uint32_t ob = 0; ob < nb; ++ob) {
    uint32_t base = (uint32_t)nc.size();
    if (ob == 0) { // the header: its phis come first
      for (uint32_t p = 0; p < P; ++p)
        nc.push_back({.k = K::Phi, .dst = pHv[p], .a = p, .b2 = (k == 1 ? sites[0].args[p] : Mp[1][p]), .t = 0, .f = latch});
      nc.push_back({.k = K::Phi, .dst = bHv, .a = bInitV, .b2 = (k == 1 ? siteBL[0] : Mbv[1]), .t = 0, .f = latch});
    }
    int sidx = -1; // which site lives in this block (>= 0: the tail site)
    for (uint32_t j = 0; j < k; ++j)
      if (sites[j].b == ob) sidx = (int)j;
    for (uint32_t i = fn.blocks[ob].begin; i < fn.blocks[ob].end; ++i) {
      const Instr &I = fn.code[i];
      if (sidx >= 0 && i == sites[(uint32_t)sidx].ci)
        continue; // the tail call dies
      if (sidx >= 0 && I.k == K::Ret && i + 1 == fn.blocks[ob].end) {
        // the tail ret becomes: [budget-1][bH==0?] -> trap or the chain
        tce_emit_site_check(fn, nc, bHv, siteBL[(uint32_t)sidx],
                            siteEQ[(uint32_t)sidx]);
        (void)0;
        uint32_t next =
            (k == 1) ? H
                     : ((uint32_t)sidx + 1 < k ? Mb[(uint32_t)sidx + 1]
                                               : Mb[k - 1]);
        nc.push_back({.k = K::Cbr, .a = siteEQ[(uint32_t)sidx], .t = T,
                      .f = next});
        continue;
      }
      Instr c = I;
      c.a = rw(c.a);
      c.b2 = rw(c.b2);
      if (c.c2 != NONE) c.c2 = rw(c.c2);
      for (uint32_t &av : c.args) av = rw(av);
      if (c.dst != NONE && c.dst < P) c.dst = pHv[c.dst];
      if (c.k == K::Br || c.k == K::Cbr || c.k == K::Phi) {
        if (c.t != NONE && c.t < nb) c.t = c.t + 1;
        if (c.f != NONE && c.f < nb) c.f = c.f + 1;
      }
      nc.push_back(c);
    }
    nbl.push_back({base, (uint32_t)nc.size()});
  }
  // latch merge chain M_{k-1} .. M_1 (M_j: phis + br M_{j-1} / H)
  for (uint32_t j = k - 1; j >= 1; --j) {
    uint32_t base = (uint32_t)nc.size();
    uint32_t pa = sites[j - 1].b + 1; // site j (1-based) edge
    uint32_t pb = (j == k - 1) ? (sites[j].b + 1) : Mb[j + 1];
    for (uint32_t p = 0; p < P; ++p)
      nc.push_back({.k = K::Phi, .dst = Mp[j][p], .a = sites[j - 1].args[p], .b2 = (j == k - 1 ? sites[j].args[p] : Mp[j + 1][p]), .t = pa, .f = pb});
    nc.push_back({.k = K::Phi, .dst = Mbv[j], .a = siteBL[j - 1], .b2 = (j == k - 1 ? siteBL[j] : Mbv[j + 1]), .t = pa, .f = pb});
    nc.push_back({.k = K::Br, .t = (j == 1 ? H : Mb[j - 1])});
    nbl.push_back({base, (uint32_t)nc.size()});
  }
  // the trap block
  {
    uint32_t base = (uint32_t)nc.size();
    tce_emit_trap(fn, nc);
    nbl.push_back({base, (uint32_t)nc.size()});
  }
  // ---- commit ----------------------------------------------------------------
  fn.code = std::move(nc);
  fn.blocks = std::move(nbl);
  fn.succ.assign(2 * fn.nblocks(), NONE);
  for (uint32_t b = 0; b < fn.nblocks(); ++b) {
    const Instr &term = fn.code[fn.blocks[b].end - 1];
    if (term.k == K::Br) fn.succ[2 * b] = term.t;
    else if (term.k == K::Cbr) {
      fn.succ[2 * b] = term.t;
      fn.succ[2 * b + 1] = term.f;
    }
  }
  fn.recompute_preds();
  os.tces++;
  return true;
}

// ---- multi member: merge a pure-tail SCC into one sel-dispatched function --
// Fm(sel, a0..a{M-1}): sel = the entry member's index (callers pass it;
// tailrec rewrites every external call site itself, so sel in [0,k) is an
// invariant recorded in Fn::tce_sel). The header dispatches on sel; each
// member's tail call to a sibling becomes [sel'=sibling; args; budget
// check; jump back through the latch chain]. Members' Ret instrs stay Ret
// (a tail caller returned the callee's value directly, so the merged fn
// returning it is exactly the original semantics).
struct TceSite {
  uint32_t m, b, ci;           // member, block (member-local id), call idx
  uint32_t callee;             // member index of the sibling callee
  std::vector<uint32_t> args;  // call args (MEMBER vreg space)
};
constexpr bool tce_merge(std::vector<Fn> &fns,
                         std::vector<std::string_view> &names,
                         std::vector<OptStats> &ostats,
                         std::vector<uint32_t> &src_index,
                         const std::vector<uint32_t> &members) {
  const uint32_t k = (uint32_t)members.size();
  if (k < 2 || k > MJZ_TCE_MAX_MEMBERS) return false;
  uint32_t M = 0;
  for (uint32_t mi : members)
    M = M > fns[mi].nparams ? M : fns[mi].nparams;
  if (M > MJZ_TCE_MAX_ARITY) return false;
  // collect the intra-SCC tail sites (args in member vreg space)
  std::vector<TceSite> sites;
  for (uint32_t x = 0; x < k; ++x) {
    const Fn &F = fns[members[x]];
    const uint32_t nvx = F.nvars + (uint32_t)F.version_base.size();
    std::vector<uint32_t> uses(nvx, 0);
    for (const Instr &I : F.code) {
      auto bump = [&](uint32_t v) {
        if (v != NONE && v < nvx) ++uses[v];
      };
      bump(I.a);
      bump(I.b2);
      bump(I.c2);
      for (uint32_t v : I.args) bump(v);
    }
    for (uint32_t b = 0; b < F.nblocks(); ++b)
      for (uint32_t i = F.blocks[b].begin; i < F.blocks[b].end; ++i) {
        if (F.code[i].k != K::Call) continue;
        uint32_t callee = (uint32_t)F.code[i].c;
        bool intra = false;
        for (uint32_t y : members) intra |= (y == callee);
        if (!intra) continue; // call out of the SCC: copied as a real call
        if (!is_tail_call(F, b, i, uses)) return false; // mixed SCC: bail
        TceSite s;
        s.m = x;
        s.b = b;
        s.ci = i;
        s.callee = NONE;
        for (uint32_t y = 0; y < k; ++y)
          if (members[y] == callee) s.callee = y;
        s.args = F.code[i].args;
        sites.push_back(std::move(s));
      }
  }
  if (sites.size() < 2 || sites.size() > MJZ_TCE_MAX_SITES) return false;
  const uint32_t ks = (uint32_t)sites.size();
  // (a k>=2-member cycle has >= k >= 2 tail sites: every member needs an
  // outgoing intra-SCC edge, so the latch chain always exists)
  // ---- build Fm -------------------------------------------------------------
  Fn Fm{};
  Fm.nparams = 1 + M;
  Fm.nvars = 1 + M;
  Fm.var_names.push_back("sel");
  for (uint32_t j = 0; j < M; ++j) Fm.var_names.push_back("a");
  // per-member vreg map: param p -> the aH phi output; others -> fresh
  std::vector<std::vector<uint32_t>> vmap(k);
  for (uint32_t x = 0; x < k; ++x) {
    const Fn &F = fns[members[x]];
    const uint32_t nvx = F.nvars + (uint32_t)F.version_base.size();
    vmap[x].assign(nvx, NONE);
  }
  // block layout: 0=P0, 1=H, 2..(1+k-2)=dispatch chain (k>2), then the
  // member bodies back to back, then the latch chain M_1..M_{ks-1}, then T
  std::vector<uint32_t> bmap_base(k);
  uint32_t next_blk = 2 + (k > 2 ? k - 2 : 0);
  for (uint32_t x = 0; x < k; ++x) {
    bmap_base[x] = next_blk;
    next_blk += fns[members[x]].nblocks();
  }
  std::vector<uint32_t> Mb(ks + 1, NONE); // latch chain M_j (1-based)
  for (uint32_t j = 1; j < ks; ++j) Mb[j] = next_blk++;
  const uint32_t T = next_blk++;
  const uint32_t latch = Mb[1];
  // ---- pre-create vreg ids --------------------------------------------------
  const uint32_t selHv = new_anon_vreg(Fm, 0);
  std::vector<uint32_t> aHv(M, NONE);
  for (uint32_t j = 0; j < M; ++j) aHv[j] = new_anon_vreg(Fm, 0);
  const uint32_t bHv = new_anon_vreg(Fm, 0);
  // member vmaps (params AFTER the aH ids exist)
  for (uint32_t x = 0; x < k; ++x) {
    const Fn &F = fns[members[x]];
    const uint32_t nvx = F.nvars + (uint32_t)F.version_base.size();
    for (uint32_t v = 0; v < nvx; ++v)
      vmap[x][v] = (v < F.nparams)
                       ? aHv[v]
                       : new_anon_vreg(Fm, F.base_of(v));
  }
  // per-site emitted vregs (sel const, budget-1, eq) — in Fm space
  std::vector<uint32_t> siteSEL(ks, NONE), siteBL(ks, NONE), siteEQ(ks, NONE);
  for (uint32_t j = 0; j < ks; ++j) { // pre-create (the header phis and the
    siteBL[j] = new_anon_vreg(Fm, 0); // chain reference them before emission)
    siteEQ[j] = new_anon_vreg(Fm, 0);
  }
  // latch chain outputs: phis for [sel, a_0..a_{M-1}, budget]
  std::vector<std::vector<uint32_t>> Mp(
      ks + 1, std::vector<uint32_t>(1 + M, NONE));
  std::vector<uint32_t> Mbv(ks + 1, NONE);
  for (uint32_t j = 1; j < ks; ++j) {
    for (uint32_t q = 0; q <= M; ++q) Mp[j][q] = new_anon_vreg(Fm, 0);
    Mbv[j] = new_anon_vreg(Fm, 0);
  }
  // ---- build the code -------------------------------------------------------
  std::vector<Instr> nc{};
  std::vector<Block> nbl(T + 1);
  auto setblock = [&](uint32_t b, uint32_t begin) {
    nbl[b].begin = begin;
    nbl[b].end = (uint32_t)nc.size();
  };
  // P0
  uint32_t bInitV = NONE;
  {
    bInitV = tce_emit_budget_init(Fm, nc);
    nc.push_back({.k = K::Br, .t = 1});
    setblock(0, 0);
  }
  // H: phis + the dispatch terminator
  {
    uint32_t base = (uint32_t)nc.size();
    nc.push_back({.k = K::Phi, .dst = selHv, .a = 0,
                  .b2 = Mp[1][0], .t = 0, .f = latch});
    for (uint32_t j = 0; j < M; ++j)
      nc.push_back({.k = K::Phi, .dst = aHv[j], .a = 1 + j,
                    .b2 = Mp[1][1 + j], .t = 0, .f = latch});
    nc.push_back({.k = K::Phi, .dst = bHv, .a = bInitV,
                  .b2 = Mbv[1], .t = 0, .f = latch});
    // dispatch: sel == x -> member x's entry. k==2: right here.
    // k>2: a chain of dispatch blocks 2..k-1
    auto emit_dispatch = [&](uint32_t from_blk) {
      uint32_t eqv = new_anon_vreg(Fm, 0);
      uint32_t cv = new_anon_vreg(Fm, 0);
      nc.push_back({.k = K::Const, .dst = cv, .c = (val_t)from_blk});
      nc.push_back({.k = K::Bin, .b = B::Eq, .dst = eqv, .a = selHv,
                    .b2 = cv});
      uint32_t tt = bmap_base[from_blk];
      uint32_t ff = (from_blk + 1 <= k - 2) ? (from_blk + 2)
                                            : bmap_base[0];
      // D_i at block id i+1: compares sel == i+1; false -> D_{i+1} / E_0
      nc.push_back({.k = K::Cbr, .a = eqv, .t = tt, .f = ff});
    };
    if (k == 2) {
      emit_dispatch(1);
    } else {
      emit_dispatch(1); // H itself: sel==1 ? E_1 : D_1(block 2)
    }
    setblock(1, base);
  }
  // dispatch chain blocks 2..k-1 (k > 2): sel == i ? E_i : next
  for (uint32_t i = 2; i <= k - 1; ++i) {
    uint32_t base = (uint32_t)nc.size();
    uint32_t eqv = new_anon_vreg(Fm, 0);
    uint32_t cv = new_anon_vreg(Fm, 0);
    nc.push_back({.k = K::Const, .dst = cv, .c = (val_t)i});
    nc.push_back({.k = K::Bin, .b = B::Eq, .dst = eqv, .a = selHv, .b2 = cv});
    nc.push_back({.k = K::Cbr, .a = eqv, .t = bmap_base[i],
                  .f = (i < k - 1) ? (i + 1) : bmap_base[0]});
    setblock(i, base);
  }
  // member bodies
  for (uint32_t x = 0; x < k; ++x) {
    const Fn &F = fns[members[x]];
    const uint32_t nvx = F.nvars + (uint32_t)F.version_base.size();
    auto mapv = [&](uint32_t v) -> uint32_t {
      if (v == NONE || v >= nvx) return v;
      return vmap[x][v] != NONE ? vmap[x][v] : v;
    };
    for (uint32_t b = 0; b < F.nblocks(); ++b) {
      uint32_t base = (uint32_t)nc.size();
      int sidx = -1;
      for (uint32_t j = 0; j < ks; ++j)
        if (sites[j].m == x && sites[j].b == b) sidx = (int)j;
      for (uint32_t i = F.blocks[b].begin; i < F.blocks[b].end; ++i) {
        const Instr &I = F.code[i];
        if (sidx >= 0 && i == sites[(uint32_t)sidx].ci) {
          // the tail call dies; its CHAIN args are emitted at the ret below
          continue;
        }
        if (sidx >= 0 && I.k == K::Ret && i + 1 == F.blocks[b].end) {
          // sel' = Const(callee); pads; budget check; cbr T : chain-next
          TceSite &s = sites[(uint32_t)sidx];
          siteSEL[(uint32_t)sidx] = new_anon_vreg(Fm, 0);
          nc.push_back({.k = K::Const, .dst = siteSEL[(uint32_t)sidx],
                        .c = (val_t)s.callee});
          uint32_t zp = NONE;
          for (uint32_t q = s.args.size(); q < M; ++q) { // pad unused args
            if (zp == NONE) {
              zp = new_anon_vreg(Fm, 0);
              nc.push_back({.k = K::Const, .dst = zp, .c = 0});
            }
            s.args.push_back(zp); // pad so chain phis can index uniformly
          }
          tce_emit_site_check(Fm, nc, bHv, siteBL[(uint32_t)sidx],
                              siteEQ[(uint32_t)sidx]);
          uint32_t next = ((uint32_t)sidx + 1 < ks) ? Mb[(uint32_t)sidx + 1]
                                                    : Mb[ks - 1];
          nc.push_back({.k = K::Cbr, .a = siteEQ[(uint32_t)sidx], .t = T,
                        .f = next});
          continue;
        }
        Instr c = I;
        if (c.dst != NONE) c.dst = mapv(c.dst);
        c.a = mapv(c.a);
        c.b2 = mapv(c.b2);
        if (c.c2 != NONE) c.c2 = mapv(c.c2);
        for (uint32_t &av : c.args) av = mapv(av);
        if (c.k == K::Br || c.k == K::Cbr || c.k == K::Phi) {
          if (c.t != NONE && c.t < F.nblocks())
            c.t = bmap_base[x] + c.t;
          if (c.f != NONE && c.f < F.nblocks())
            c.f = bmap_base[x] + c.f;
        }
        nc.push_back(c);
      }
      setblock(bmap_base[x] + b, base);
    }
  }
  // latch merge chain M_{ks-1}..M_1: phis for [sel, a_0.., budget]
  for (uint32_t j = ks - 1; j >= 1; --j) {
    uint32_t base = (uint32_t)nc.size();
    const TceSite &sa = sites[j - 1];
    uint32_t pa = bmap_base[sa.m] + sa.b;
    uint32_t pb = (j == ks - 1) ? (bmap_base[sites[j].m] + sites[j].b)
                                : Mb[j + 1];
    // arg list of site j-1 in Fm space: [sel'] + args (mapped)
    auto sitearg = [&](const TceSite &s, uint32_t q) -> uint32_t {
      const Fn &SF = fns[members[s.m]];
      const uint32_t nvx = SF.nvars + (uint32_t)SF.version_base.size();
      if (q == 0) return siteSEL[(uint32_t)(&s - &sites[0])];
      uint32_t v = s.args[q - 1];
      return (v != NONE && v < nvx) ? vmap[s.m][v] : v;
    };
    nc.push_back({.k = K::Phi, .dst = Mp[j][0], .a = sitearg(sites[j - 1], 0), .b2 = (j == ks - 1 ? sitearg(sites[j], 0) : Mp[j + 1][0]), .t = pa, .f = pb});
    for (uint32_t q = 1; q <= M; ++q)
      nc.push_back({.k = K::Phi, .dst = Mp[j][q], .a = sitearg(sites[j - 1], q), .b2 = (j == ks - 1 ? sitearg(sites[j], q)
                                       : Mp[j + 1][q]), .t = pa, .f = pb});
    nc.push_back({.k = K::Phi, .dst = Mbv[j], .a = siteBL[j - 1], .b2 = (j == ks - 1 ? siteBL[j] : Mbv[j + 1]), .t = pa, .f = pb});
    nc.push_back({.k = K::Br, .t = (j == 1 ? 1 : Mb[j - 1])});
    setblock(Mb[j], base);
  }
  // the trap block
  {
    uint32_t base = (uint32_t)nc.size();
    tce_emit_trap(Fm, nc);
    setblock(T, base);
  }
  Fm.code = std::move(nc);
  Fm.blocks = std::move(nbl);
  Fm.succ.assign(2 * Fm.nblocks(), NONE);
  for (uint32_t b = 0; b < Fm.nblocks(); ++b) {
    const Instr &term = Fm.code[Fm.blocks[b].end - 1];
    if (term.k == K::Br) Fm.succ[2 * b] = term.t;
    else if (term.k == K::Cbr) {
      Fm.succ[2 * b] = term.t;
      Fm.succ[2 * b + 1] = term.f;
    }
  }
  Fm.recompute_preds();
  Fm.tce_sel = k; // sel in [0,k) at every call site: an invariant we made
  // ---- rewrite every external call site: member m -> Fm(sel=m, ...) ------
  const uint32_t FmIdx = (uint32_t)fns.size();
  for (uint32_t xf = 0; xf < fns.size(); ++xf) {
    bool is_member = false;
    for (uint32_t mi : members) is_member |= (mi == xf);
    if (is_member) continue;
    Fn &X = fns[xf];
    bool touched = false;
    for (uint32_t b = 0; b < X.nblocks(); ++b)
      for (uint32_t i = X.blocks[b].begin; i < X.blocks[b].end; ++i) {
        Instr &I = X.code[i];
        if (I.k != K::Call) continue;
        uint32_t callee = (uint32_t)I.c;
        int m = -1;
        for (uint32_t y = 0; y < k; ++y)
          if (members[y] == callee) m = (int)y;
        if (m < 0) continue;
        // insert sel const (+ pads) right before the call, then redirect
        std::vector<Instr> pre{};
        uint32_t sv = new_anon_vreg(X, 0);
        pre.push_back({.k = K::Const, .dst = sv, .c = (val_t)m});
        uint32_t zp = NONE;
        for (uint32_t q = I.args.size(); q < M; ++q) {
          if (zp == NONE) {
            zp = new_anon_vreg(X, 0);
            pre.push_back({.k = K::Const, .dst = zp, .c = 0});
          }
          I.args.push_back(zp);
        }
        I.args.insert(I.args.begin(), sv);
        I.c = (int64_t)FmIdx;
        // splice `pre` before the call instr
        X.code.insert(X.code.begin() + (long)i, pre.begin(), pre.end());
        const uint32_t add = (uint32_t)pre.size();
        for (uint32_t xb = 0; xb < X.nblocks(); ++xb) {
          if (xb == b) {
            X.blocks[xb].end += add;
            continue;
          }
          if (X.blocks[xb].begin > i) X.blocks[xb].begin += add;
          if (X.blocks[xb].end > i) X.blocks[xb].end += add;
        }
        i += add; // skip the inserted prefix; the call is next
        touched = true;
      }
    (void)touched;
  }
  // ---- append Fm; bookkeeping ------------------------------------------------
  fns.push_back(std::move(Fm));
  names.push_back("tce_merged");
  ostats.push_back(OptStats{});
  // count on main too: the merged fn is usually inlined and then removed by
  // dead-fn elimination, which would take its (interesting) stats with it
  ostats[FmIdx].tces++;
  ostats[FmIdx].tce_merges++;
  if (!ostats.empty()) {
    ostats[0].tces++;
    ostats[0].tce_merges++;
  }
  src_index.push_back(src_index[members[0]]); // display placeholder
  return true;
}

// ---- the pass driver ----------------------------------------------------------
constexpr bool opt_tailrec(std::vector<Fn> &fns,
                           std::vector<std::string_view> &names,
                           std::vector<OptStats> &ostats,
                           std::vector<uint32_t> &src_index) {
  if (fns.size() < 2) return false;
  bool changed = false;
  CallGraph G = build_callgraph(fns);
  for (uint32_t s = 0; s < G.nscc; ++s) {
    uint32_t e = (s + 1 < G.nscc) ? G.scc_begin[s + 1]
                                  : (uint32_t)G.members.size();
    std::vector<uint32_t> members(G.members.begin() + G.scc_begin[s],
                                  G.members.begin() + e);
    if (members.empty()) continue;
    bool has_main = false;
    for (uint32_t m : members) has_main |= (m == 0);
    if (has_main) continue; // main is never a callee
    // must actually recurse: an intra-SCC edge
    bool intra = false;
    for (uint32_t m : members)
      for (const Instr &I : fns[m].code)
        if (I.k == K::Call) {
          uint32_t c = (uint32_t)I.c;
          for (uint32_t m2 : members) intra |= (m2 == c);
        }
    if (!intra) continue;
    if (members.size() == 1) {
      if (fns[members[0]].code.size() > MJZ_TCE_MAX_FNSIZE) continue;
      if (tce_self(fns[members[0]], ostats[members[0]], members[0]))
        changed = true;
      continue;
    }
    bool big = false;
    for (uint32_t m : members)
      big |= fns[m].code.size() > MJZ_TCE_MAX_FNSIZE;
    if (big) continue;
    if (tce_merge(fns, names, ostats, src_index, members)) changed = true;
  }
  return changed;
}

// ============================================================================
// 9e5. COUNTDOWN LOOP CLOSED FORM (cdfold)
// ============================================================================
// The loop shape tail-call elimination emits (after the cheap tiers have
// spliced and folded it) is a COUNTDOWN with an exact recursion budget:
//
//   pre:  bInit = CAP+1 - Depth(); [sel0 = Const]      (entry state)
//   H:    selH = phi[sel0][selL]; nP = phi[n0][nL]; bH = phi[bInit][bL]
//         cbr (selH == 1) ? E_t : E_f                  (member dispatch)
//   E_m:  cbr (nP == 0) ? X_m : A_m                    (the base test)
//   X_m:  [K_m = Const]; br NEXT / ret K_m             (the member's value)
//   A_m:  n' = nP - 1; b' = bH - 1; cbr (bH == 0) ? T : L   (budget check)
//   L:    selL = phi[c_t][c_f]; nL = phi[n'][n']; bL = phi[b'][b']; br H
//   T:    1 / 0                                        (the depth TRAP)
//
// The closed form is exact: N = (u64)n0 iterations (n hits 0 iff it ever
// does — else the budget fires first), sel_N = toggle^N(sel0) with the
// toggle closed form (1-2p)*sel0 + p, p = N&1 (EXACT for every sel0, and
// sel stays in {0,1} when sel0 is, which is when the final select is
// sound), and the guard `(u64)n0 > (u64)bInit` jumps to the SAME trap the
// budget would have fired (n0 < 0 wraps to a huge unsigned count and n0
// over budget both trap — exactly where the original recursion cap did).
// Additive accumulators x' = x + g + b*nP fold via x_N = x0 + gN +
// b(N*n0 - N(N-1)/2), all mod-2^64 exact. Binary-search-shaped bodies
// (args not affine in the phis) fail the resolver and keep their loop.
struct CdAff { // affine over H-phi outputs: sum(coeff_i * phi_i) + k
  std::vector<std::pair<uint32_t, val_t>> t{}; // (H-phi vreg, coeff), sorted
  val_t c{};
};
constexpr bool cd_aff_eq(const CdAff &a, const CdAff &b) {
  if (a.c != b.c || a.t.size() != b.t.size()) return false;
  for (size_t i = 0; i < a.t.size(); ++i)
    if (a.t[i] != b.t[i]) return false;
  return true;
}
constexpr void cd_term(CdAff &L, val_t k, uint32_t v) {
  if (k == 0) return;
  for (size_t i = 0; i < L.t.size(); ++i)
    if (L.t[i].first == v) {
      L.t[i].second = (val_t)((uint64_t)L.t[i].second + (uint64_t)k);
      if (L.t[i].second == 0) L.t.erase(L.t.begin() + (long)i);
      return;
    }
  size_t i = 0;
  while (i < L.t.size() && L.t[i].first < v) ++i;
  L.t.insert(L.t.begin() + (long)i, {v, k});
}
// resolve v to an affine form over `hphis` (bounded depth; walks
// Const/Copy/Bin(+,-,*const)/Phi-with-equal-affine-args)
constexpr bool cd_resolve(const Fn &fn, uint32_t v,
                          const std::vector<uint32_t> &hphis, CdAff &out,
                          int depth) {
  if (depth > 8) return false;
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (v == NONE || v >= nv) return false;
  for (uint32_t h : hphis) {
    if (v == h) {
      out = CdAff{};
      cd_term(out, 1, v);
      return true;
    }
  }
  // def site
  uint32_t di = NONE, db = NONE;
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
      if (fn.code[i].dst == v) {
        di = i;
        db = b;
      }
  if (di == NONE) return false;
  const Instr &D = fn.code[di];
  if (D.k == K::Const) {
    out = CdAff{};
    out.c = D.c;
    return true;
  }
  if (D.k == K::Copy && D.a != NONE)
    return cd_resolve(fn, D.a, hphis, out, depth + 1);
  if (D.k == K::Bin && (D.b == B::Add || D.b == B::Sub || D.b == B::Mul)) {
    CdAff x{}, y{};
    if (!cd_resolve(fn, D.a, hphis, x, depth + 1)) return false;
    if (!cd_resolve(fn, D.b2, hphis, y, depth + 1)) return false;
    if (D.b == B::Mul) { // one side must be a pure constant
      if (!x.t.empty()) { // x is the value, y the const
        if (!y.t.empty()) return false;
        out = x;
        for (auto &pr : out.t)
          pr.second = (val_t)((uint64_t)pr.second * (uint64_t)y.c);
        out.c = (val_t)((uint64_t)out.c * (uint64_t)y.c);
        return true;
      }
      if (!y.t.empty()) return false; // both const: shouldn't happen
      out = CdAff{};
      out.c = D.c; // placeholder, replaced below
      out = x;
      out.c = (val_t)0;
      // const*const: fold via c
      CdAff cx{};
      cd_resolve(fn, D.a, hphis, cx, depth + 1); // known const
      out.c = (val_t)((uint64_t)cx.c * (uint64_t)y.c);
      return true;
    }
    val_t sgn = (D.b == B::Add) ? 1 : (val_t)-1;
    out = x; // x - y = x + sgn*y: x UNSCALED, y scaled (a - b, not -(a+b))
    for (auto &pr : y.t)
      cd_term(out, (val_t)((uint64_t)pr.second * (uint64_t)sgn), pr.first);
    out.c = (val_t)((uint64_t)x.c + (uint64_t)((val_t)((uint64_t)y.c * (uint64_t)sgn)));
    return true;
  }
  if (D.k == K::Phi) { // equal affines on all live args
    // resolve every arg; all must agree
    std::vector<CdAff> as{};
    auto arg_of = [&](uint32_t key, uint32_t &av) -> bool {
      if (D.t == key) { av = D.a; return D.a != NONE; }
      if (D.f == key) { av = D.b2; return D.b2 != NONE; }
      return false;
    };
    for (uint32_t p : fn.preds[db]) {
      uint32_t av = NONE;
      if (!arg_of(p, av)) continue; // dead pred
      CdAff a{};
      if (!cd_resolve(fn, av, hphis, a, depth + 1)) return false;
      as.push_back(a);
    }
    if (as.empty()) return false;
    for (size_t i = 1; i < as.size(); ++i)
      if (!cd_aff_eq(as[0], as[i])) return false;
    out = as[0];
    return true;
  }
  return false;
}

constexpr bool opt_cdfold(Fn &fn, const An &A, OptStats &os) {
  if (A.n != fn.nblocks() || !fn.nblocks()) return false;
  const uint32_t nb = fn.nblocks();
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nv) return false;
  bool changed = false;
  for (uint32_t h = 0; h < A.n; ++h) {
    if (A.inner[h] != h) continue; // not a loop header
    bool folded = [&]() -> bool {
    auto row = A.loops.range()[h];
    // sized to the CURRENT block count: earlier folds in this same pass
    // call appended closed-form blocks (ids >= A.n); they are outside this
    // loop, so 0 is the right default for them, and the succ scan below may
    // legitimately index them
    std::vector<char> inL(fn.nblocks(), 0);
    for (ulen x : row) inL[(uint32_t)x] = 1;
    inL[h] = 1;
    // exactly one outside pred (preheader) + one latch
    uint32_t pre = NONE, L = NONE;
    for (uint32_t p : fn.preds[h]) {
      if (inL[p]) {
        if (L != NONE) return false; // multiple latches (multi-site self)
        L = p;
      } else {
        if (pre != NONE) return false;
        pre = p;
      }
    }
    if (pre == NONE || L == NONE) return false;
    // (the latch ends with `br h` when a merge chain exists, or with the
    // budget check `cbr ? T : h` when the site block IS the latch; the
    // exit classification below validates the trap edge either way)
    // ---- collect + classify the header phis ---------------------------------
    struct HP {
      uint32_t dst, entry, latch;
    };
    std::vector<HP> hp;
    for (uint32_t i = fn.blocks[h].begin; i < fn.blocks[h].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k != K::Phi) break;
      uint32_t e = (I.t == pre) ? I.a : ((I.f == pre) ? I.b2 : NONE);
      uint32_t l = (I.t == L) ? I.a : ((I.f == L) ? I.b2 : NONE);
      if (e == NONE || l == NONE) return false;
      hp.push_back({I.dst, e, l});
    }
    if (hp.size() < 2) return false; // nP + bH at least
    std::vector<uint32_t> hids;
    for (auto &p : hp) hids.push_back(p.dst);
    // resolve each latch value; classify
    struct Cls {
      uint32_t phi;      // the H-phi vreg
      uint32_t entry;    // entry (pre) value vreg
      CdAff laff;        // latch evolution, affine over hphis
      bool is_sel{};     // selector (latch merges per-arm CONSTS)
    };
    std::vector<Cls> cls;
    for (auto &p : hp) {
      CdAff la{};
      if (cd_resolve(fn, p.latch, hids, la, 0)) {
        cls.push_back({p.dst, p.entry, la, false});
        continue;
      }
      // selector? the latch is a Phi of CONSTS selected by the dispatch
      // (resolve failed => not uniform affine) -- verify structurally:
      uint32_t di = NONE, db = NONE;
      for (uint32_t b = 0; b < nb; ++b)
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
          if (fn.code[i].dst == p.latch) {
            di = i;
            db = b;
          }
      if (di == NONE || fn.code[di].k != K::Phi) return false;
      cls.push_back({p.dst, p.entry, CdAff{}, true});
      (void)db;
    }
    // exactly one selector at most
    uint32_t selp = NONE;
    for (auto &c : cls)
      if (c.is_sel) {
        if (selp != NONE) return false;
        selp = c.phi;
      }
    // find nP and bH: nP's latch affine = nP - 1; bH's = bH - 1
    uint32_t nP = NONE, bH = NONE, nEntry = NONE, bEntry = NONE;
    for (auto &c : cls) {
      if (c.is_sel) continue;
      if (c.laff.t.size() == 1 && c.laff.t[0].first == c.phi &&
          c.laff.t[0].second == 1 && c.laff.c == (val_t)-1) {
        if (nP == NONE) {
          nP = c.phi;
          nEntry = c.entry;
        } else if (bH == NONE) {
          bH = c.phi;
          bEntry = c.entry;
        } else
          return false; // a third pure countdown: unsupported
      }
    }
    if (nP == NONE || bH == NONE) return false;
    // any remaining non-selector phis must be additive/identity over
    // {self, nP}: x' = x + g (+ b*nP)
    struct Add {
      uint32_t phi, entry;
      val_t g, bn;
    };
    std::vector<Add> adds;
    for (auto &c : cls) {
      if (c.is_sel || c.phi == nP || c.phi == bH) continue;
      val_t g = 0, bn = 0;
      bool ok = true;
      for (auto &pr : c.laff.t) {
        if (pr.first == c.phi) {
          if (pr.second != 1) ok = false;
        } else if (pr.first == nP) {
          bn = pr.second;
        } else
          ok = false;
      }
      if (!ok || c.laff.t.size() > 2) return false;
      for (auto &pr : c.laff.t)
        if (pr.first == c.phi && pr.second != 1) return false;
      // require self coefficient exactly 1 (additive) — it is in t only once
      bool hasSelf = false;
      for (auto &pr : c.laff.t)
        if (pr.first == c.phi) hasSelf = true;
      if (!hasSelf) return false; // pure function of nP: unsupported here
      adds.push_back({c.phi, c.entry, g, bn});
      // g = the constant part
      adds.back().g = c.laff.c;
    }
    // ---- the dispatch / base / trap structure --------------------------------
    // dispatch: H's terminator (single member: the base test itself)
    const Instr hterm = fn.code[fn.blocks[h].end - 1];
    uint32_t Xt = NONE, Xf = NONE; // value exits (true/false dispatch arms)
    uint32_t T = NONE;             // the common trap exit
    auto is_np_zero = [&](const Instr &C, uint32_t &xt, uint32_t &af) -> bool {
      // C: cbr cond ? a : b where cond = Bin(nP == 0): the ==0 arm exits
      if (C.k != K::Cbr) return false;
      uint32_t di = NONE;
      for (uint32_t b = 0; b < nb; ++b)
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
          if (fn.code[i].dst == C.a) di = i;
      if (di == NONE) return false;
      const Instr &D = fn.code[di];
      if (D.k != K::Bin || D.b != B::Eq) return false;
      bool aIsN = (D.a == nP);
      bool bIsN = (D.b2 == nP);
      uint32_t other = aIsN ? D.b2 : D.b2;
      other = aIsN ? D.b2 : (bIsN ? D.a : NONE);
      if (other == NONE) return false;
      // the other operand must be const 0
      uint32_t oi = NONE;
      for (uint32_t b = 0; b < nb; ++b)
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
          if (fn.code[i].dst == other) oi = i;
      if (oi == NONE || fn.code[oi].k != K::Const || fn.code[oi].c != 0)
        return false;
      // which arm is the n==0 (base) arm? Eq is symmetric: TRUE arm = equal
      xt = C.t;
      af = C.f;
      return true;
    };
    auto is_bh_zero_trap = [&](const Instr &C, uint32_t &tt) -> bool {
      if (C.k != K::Cbr) return false;
      uint32_t di = NONE;
      for (uint32_t b = 0; b < nb; ++b)
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
          if (fn.code[i].dst == C.a) di = i;
      if (di == NONE) return false;
      const Instr &D = fn.code[di];
      if (D.k != K::Bin || D.b != B::Eq) return false;
      uint32_t other = (D.a == bH) ? D.b2
                                    : ((D.b2 == bH) ? D.a : NONE);
      if (other == NONE) return false;
      uint32_t oi = NONE;
      for (uint32_t b = 0; b < nb; ++b)
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
          if (fn.code[i].dst == other) oi = i;
      if (oi == NONE || fn.code[oi].k != K::Const || fn.code[oi].c != 0)
        return false;
      tt = C.t; // the ==0 arm traps
      return true;
    };
    // every block WITH an outside edge must exit via a base test (nP==0
    // at a member block) or the budget trap (bH==0); the dispatch header
    // and the latch have only internal successors and are skipped here
    std::vector<uint32_t> Xs;
    for (uint32_t b = 0; b < nb; ++b) {
      if (!inL[b]) continue;
      bool has_out = false;
      for (uint32_t s : {fn.succ0(b), fn.succ1(b)})
        if (s != NONE && !inL[s]) has_out = true;
      if (!has_out) continue;
      const Instr &tm = fn.code[fn.blocks[b].end - 1];
      uint32_t xt = NONE, af = NONE, tt = NONE;
      if (is_np_zero(tm, xt, af)) {
        if (inL[xt] || !inL[af]) return false;
        Xs.push_back(xt);
        continue;
      }
      if (is_bh_zero_trap(tm, tt)) {
        if (inL[tt]) return false;
        if (T == NONE) T = tt;
        else if (T != tt) return false;
        continue;
      }
      return false; // some other exit: keep the loop
    }
    if (T == NONE) return false; // no trap edge: not the TCE shape
    // Xs: dedupe
    {
      std::vector<uint32_t> u;
      for (uint32_t x : Xs) {
        bool dup = false;
        for (uint32_t y : u) dup |= (x == y);
        if (!dup) u.push_back(x);
      }
      Xs = u;
    }
    if (Xs.empty() || Xs.size() > 2) return false;
    // X blocks (the base-case exits): a Br to a common next (or a Ret),
    // at most ONE Const (the member's base value when there is one), and
    // nothing that uses a loop phi except the RET OPERAND (an accumulator
    // returns a loop-carried value; the outside-use rewrite below replaces
    // it with the closed form).
    std::vector<uint32_t> kval(Xs.size(), NONE);
    uint32_t xnext = NONE;
    bool xret = false;
    for (size_t xi = 0; xi < Xs.size(); ++xi) {
      uint32_t b = Xs[xi];
      const Instr &tm = fn.code[fn.blocks[b].end - 1];
      if (tm.k == K::Ret) {
        if (tm.a == NONE) return false;
        xret = true;
        // the ret operand: a Const def here, or a loop phi (accumulator)
        uint32_t di = NONE;
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
          if (fn.code[i].dst == tm.a) di = i;
        bool isLphi = false;
        for (uint32_t hid : hids) isLphi |= (tm.a == hid);
        if (di != NONE && fn.code[di].k == K::Const) kval[xi] = fn.code[di].c;
        else if (!isLphi) return false;
      } else if (tm.k == K::Br) {
        if (xret) return false;
        if (xnext == NONE) xnext = tm.t;
        else if (xnext != tm.t) return false;
        uint32_t nconst = 0;
        for (uint32_t i = fn.blocks[b].begin; i + 1 < fn.blocks[b].end; ++i) {
          const Instr &I = fn.code[i];
          if (I.k == K::Const) {
            ++nconst;
            kval[xi] = I.c;
          } else if (I.k == K::Phi)
            continue;
          else
            return false;
        }
        if (nconst > 1) return false;
        if (nconst == 0 && selp != NONE)
          return false; // the member select needs its base consts
      } else
        return false;
      // no loop-phi uses inside X (except a Ret operand, handled above)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        auto usesL = [&](uint32_t v) {
          for (uint32_t hid : hids)
            if (v == hid) return true;
          return false;
        };
        if (i + 1 == fn.blocks[b].end && I.k == K::Ret) {
          if (usesL(I.b2) || usesL(I.c2)) return false;
          continue; // I.a checked above
        }
        if (usesL(I.a) || usesL(I.b2) || usesL(I.c2)) return false;
      }
    }
    // ---- selector soundness: sel in {0,1} ------------------------------------
    // entry: a Const in {0,1}, or the fn's sel param under Fn::tce_sel == 2
    // toggle consts in {0,1}: checked from the latch phi args
    uint32_t selEntry = NONE;
    bool selOk = false;
    if (selp != NONE) {
      for (auto &p : hp)
        if (p.dst == selp) selEntry = p.entry;
      uint32_t di = NONE;
      for (uint32_t b = 0; b < nb; ++b)
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
          if (fn.code[i].dst == selEntry) di = i;
      if (di != NONE && fn.code[di].k == K::Const &&
          (fn.code[di].c == 0 || fn.code[di].c == 1))
        selOk = true;
      if (!selOk && fn.tce_sel == 2 && selEntry == 0) selOk = true;
      if (!selOk) return false;
      // find the sel latch phi (the L-phi feeding selp) and check its args
      uint32_t selLatch = NONE;
      for (auto &p : hp)
        if (p.dst == selp) selLatch = p.latch;
      uint32_t si = NONE;
      for (uint32_t b = 0; b < nb; ++b)
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
          if (fn.code[i].dst == selLatch) si = i;
      if (si == NONE) return false;
      // its args must be consts in {0,1}, and DISTINCT
      {
        uint32_t a1 = fn.code[si].a, b1 = fn.code[si].b2;
        uint32_t ia = NONE, ib = NONE;
        for (uint32_t b = 0; b < nb; ++b)
          for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
            if (fn.code[i].dst == a1) ia = i;
            if (fn.code[i].dst == b1) ib = i;
          }
        if (ia == NONE || ib == NONE) return false;
        if (fn.code[ia].k != K::Const || fn.code[ib].k != K::Const)
          return false;
        val_t ca = fn.code[ia].c, cb = fn.code[ib].c;
        if ((ca != 0 && ca != 1) || (cb != 0 && cb != 1) || ca == cb)
          return false;
        // A = cb - ca must be -1 or +1 (a toggle or identity)
        val_t A = (val_t)((uint64_t)cb - (uint64_t)ca);
        if (A != 1 && A != (val_t)-1) return false;
      }
    }
    // ---- body purity: only Phi/Const/Copy/Bin(nt)/Br/Cbr in the loop -------
    for (uint32_t b = 0; b < nb; ++b) {
      if (!inL[b]) continue;
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        switch (I.k) {
        case K::Phi:
        case K::Const:
        case K::Copy:
        case K::Br:
        case K::Cbr:
          break;
        case K::Bin:
          if ((I.b == B::Div || I.b == B::Mod) && !I.nt) return false;
          break;
        default:
          return false; // Print/Call/Input/Alloc/Load/Store/Len: observable
        }
      }
    }
    // ---- fold -----------------------------------------------------------------
    // guard (unsigned n0 > bInit) -> T ; closed forms -> select / X
    {
      std::vector<Instr> g{}; // appended to the preheader
      auto konst = [&](val_t c) {
        uint32_t v = new_anon_vreg(fn, 0);
        g.push_back({.k = K::Const, .dst = v, .c = c});
        return v;
      };
      auto bin = [&](B b, uint32_t d, uint32_t a, uint32_t b2) {
        g.push_back({.k = K::Bin, .b = b, .dst = d, .a = a, .b2 = b2});
      };
      auto nt = [&]() { return new_anon_vreg(fn, 0); };
      const uint32_t MINc = konst(INT64_MIN);
      const uint32_t xN = nt();
      bin(B::Xor, xN, nEntry, MINc);
      const uint32_t xB = nt();
      bin(B::Xor, xB, bEntry, MINc);
      const uint32_t bad = nt();
      bin(B::Lt, bad, xB, xN); // unsigned bInit < N  <=>  N > bInit
      // CF block: closed forms + the select
      std::vector<Instr> cf{};
      auto ckonst = [&](val_t c) {
        uint32_t v = new_anon_vreg(fn, 0);
        cf.push_back({.k = K::Const, .dst = v, .c = c});
        return v;
      };
      auto cbin = [&](B b, uint32_t d, uint32_t a, uint32_t b2) {
        cf.push_back({.k = K::Bin, .b = b, .dst = d, .a = a, .b2 = b2});
      };
      // p = n0 & 1
      const uint32_t oneC = ckonst(1);
      const uint32_t pV = nt();
      cbin(B::And, pV, nEntry, oneC);
      uint32_t selN = NONE;
      if (selp != NONE) {
        // selN = (1-2p)*selEntry + p   (exact for every selEntry; the
        // SELECT below additionally needs sel in {0,1}, proven above)
        const uint32_t two = nt();
        cbin(B::Add, two, pV, pV);
        const uint32_t k1 = nt();
        cbin(B::Sub, k1, oneC, two);
        const uint32_t m = nt();
        cbin(B::Mul, m, k1, selEntry);
        selN = nt();
        cbin(B::Add, selN, m, pV);
      }
      // additive closed forms: x_N = x0 + g*N + bn*(N*n0 - T), T = N(N-1)/2
      std::vector<std::pair<uint32_t, uint32_t>> xform; // (H-phi -> value)
      if (!adds.empty()) {
        const uint32_t e0 = ckonst(1); // reuse oneC
        const uint32_t two = nt();
        cbin(B::Add, two, nEntry, nEntry); // 2N (parity of N: N&1 — have pV)
        // T = (N - (N&1))/2 * (N + (N&1) - 1)  [exact, same trick as lcf rt]
        const uint32_t Nm = nt();
        cbin(B::Sub, Nm, nEntry, pV);
        const uint32_t h2 = nt();
        cbin(B::LShr, h2, Nm, e0);
        const uint32_t t1 = nt();
        cbin(B::Add, t1, nEntry, pV);
        const uint32_t o1 = nt();
        cbin(B::Sub, o1, t1, e0);
        const uint32_t Tv = nt();
        cbin(B::Mul, Tv, h2, o1);
        const uint32_t NN = nt(); // N*n0
        cbin(B::Mul, NN, nEntry, nEntry);
        for (auto &ad : adds) {
          // x_N = x0 + g*N + bn*(N*n0 - T)
          uint32_t acc = ad.entry;
          if (ad.g != 0) {
            const uint32_t gn = nt();
            cbin(B::Mul, gn, ckonst(ad.g), nEntry);
            const uint32_t a2 = nt();
            cbin(B::Add, a2, acc, gn);
            acc = a2;
          }
          if (ad.bn != 0) {
            const uint32_t sn = nt();
            cbin(B::Sub, sn, NN, Tv);
            const uint32_t bt = nt();
            cbin(B::Mul, bt, sn, ckonst(ad.bn));
            const uint32_t a3 = nt();
            cbin(B::Add, a3, acc, bt);
            acc = a3;
          }
          xform.push_back({ad.phi, acc});
        }
      }
      // n_N = 0; b_N = bInit - N (only if used outside)
      const uint32_t zeroC = ckonst(0);
      xform.push_back({nP, zeroC});
      // ---- rewrite outside uses of the H-phis --------------------------------
      // (X blocks use none; T uses none; anywhere else outside inL —
      // including closed-form blocks appended by earlier folds in this same
      // pass call, which never reference this loop's phis but are covered
      // here anyway so the rewrite is complete by construction)
      for (uint32_t b = 0; b < fn.nblocks(); ++b) {
        if (b >= inL.size() || inL[b]) continue;
        if (b == pre) continue; // handled below (guard emission)
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
          Instr &I = fn.code[i];
          auto rwx = [&](uint32_t &v) {
            if (v == NONE) return;
            if (v == selp && selN != NONE) v = selN;
            for (auto &xf : xform)
              if (v == xf.first) v = xf.second;
          };
          rwx(I.a);
          rwx(I.b2);
          rwx(I.c2);
          for (uint32_t &av : I.args) rwx(av);
        }
      }
      // ---- emit: pre gets the guard; CF becomes a new block -----------------
      const uint32_t CFb = fn.nblocks(); // appended block id — CURRENT
      // count: an earlier fold in this same pass call already appended its
      // own CF block, and reusing the entry-time count would collide with it
      // pre's terminator: br h -> cbr bad ? T : CF
      {
        Instr &pt = fn.code[fn.blocks[pre].end - 1];
        if (pt.k != K::Br || pt.t != h) return false;
        pt.k = K::Cbr;
        pt.a = bad;
        pt.t = T;
        pt.f = CFb;
      }
      // CF terminator: the select (multi) or br X (single member)
      if (selp != NONE) {
        if (Xs.size() != 2) return false;
        // the header dispatches on sel: hterm's cond must be Eq(selH, ..)
        uint32_t di = NONE;
        for (uint32_t b = 0; b < nb; ++b)
          for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
            if (fn.code[i].dst == hterm.a) di = i;
        if (di == NONE || fn.code[di].k != K::Bin ||
            fn.code[di].b != B::Eq || fn.code[di].a != selp)
          return false;
        // sel == 1 -> TRUE arm; walk E_true to its X
        uint32_t Et = hterm.t;
        if (Et >= nb) return false;
        const Instr &et = fn.code[fn.blocks[Et].end - 1];
        uint32_t xt2 = NONE, af2 = NONE;
        if (!is_np_zero(et, xt2, af2)) return false;
        uint32_t Xtrue = xt2, Xfalse = NONE;
        for (uint32_t x : Xs)
          if (x != Xtrue) Xfalse = x;
        if (Xfalse == NONE) return false;
        cf.push_back({.k = K::Cbr, .a = selN, .t = Xtrue, .f = Xfalse});
      } else {
        if (Xs.size() != 1) return false;
        cf.push_back({.k = K::Br, .t = Xs[0]});
      }
      // splice g into pre (before its terminator) and append CF
      {
        uint32_t pe = fn.blocks[pre].end - 1;
        fn.code.insert(fn.code.begin() + (long)pe, g.begin(), g.end());
        const uint32_t add = (uint32_t)g.size();
        // shift EVERY current block range — including CF blocks appended by
        // earlier folds in this same pass call, whose instrs sit at the end
        // of the code vector and move when g is spliced in (a stale entry-
        // time bound here left their ranges pointing into unrelated code)
        for (uint32_t b = 0; b < fn.nblocks(); ++b) {
          if (b == pre) {
            fn.blocks[b].end += add;
            continue;
          }
          if (fn.blocks[b].begin >= pe + 1) fn.blocks[b].begin += add;
          if (fn.blocks[b].end > pe) fn.blocks[b].end += add;
        }
      }
      fn.code.insert(fn.code.end(), cf.begin(), cf.end());
      fn.blocks.push_back({(uint32_t)fn.code.size() - cf.size(),
                           (uint32_t)fn.code.size()});
      fn.succ.push_back(NONE);
      fn.succ.push_back(NONE);
      // pre -> T / CF ; CF -> its terminator target
      fn.succ[2 * pre] = T;
      fn.succ[2 * pre + 1] = CFb;
      {
        const Instr &ct = fn.code[fn.blocks[CFb].end - 1];
        if (ct.k == K::Br) fn.succ[2 * CFb] = ct.t;
        else if (ct.k == K::Cbr) {
          fn.succ[2 * CFb] = ct.t;
          fn.succ[2 * CFb + 1] = ct.f;
        }
      }
      // the loop is now unreachable: succ severed AND the blocks' code
      // ranges emptied — a Br-with-NONE-target left in the code vector
      // would crash the bytecode emitter (off[f][NONE]) even though the
      // block is dead; empty ranges are inert everywhere
      for (uint32_t b = 0; b < nb; ++b) {
        if (!inL[b]) continue;
        fn.succ[2 * b] = NONE;
        fn.succ[2 * b + 1] = NONE;
        fn.blocks[b].end = fn.blocks[b].begin; // drop every instr
      }
      fn.recompute_preds();
      os.cdfolds++;
      changed = true;
    }
  return true;
  }();
  }
  return changed;
}

// ============================================================================
// 9e6. IF-CONVERSION of provably-binary selects (ifconv)
// ============================================================================
// `cbr c ? X1 : X0` where both arms just materialize a CONSTANT (a Const
// def + a jump to the same continuation, or a Ret of a Const) and the
// range analysis proves c in [0,1]: the branch is a SELECT, and
//   dst = K0 + (K1 - K0) * c
// is exact (c in {0,1}). This is the last step of the is_even/is_odd
// pipeline: the countdown fold leaves `cbr selN ? ret1 : ret0`, the range
// analysis proves selN = n&1 in [0,1], and the whole mutual recursion has
// become `print(input() & 1)`.
constexpr bool opt_ifconv(Fn &fn, OptStats &os) {
  const uint32_t nb = fn.nblocks();
  if (!nb) return false;
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nv) return false;
  std::vector<char> kc(nv, 0);
  std::vector<val_t> kv(nv, 0);
  for (const Instr &I : fn.code)
    if (I.k == K::Const && I.dst != NONE && I.dst < nv) {
      kc[I.dst] = 1;
      kv[I.dst] = I.c;
    }
  // def lookup
  std::vector<uint32_t> defi(nv, NONE), defb(nv, NONE);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
      if (fn.code[i].dst != NONE && fn.code[i].dst < nv) {
        defi[fn.code[i].dst] = i;
        defb[fn.code[i].dst] = b;
      }
  // range analysis for the [0,1] proof — computed LAZILY on the first
  // candidate (the cheap structural pre-checks below reject most blocks
  // without ever paying for the analysis)
  An ARA{};
  Ranges RA{};
  bool ra_done = false;
  auto ensure_ra = [&]() {
    if (ra_done) return;
    ra_done = true;
    ARA = analyze(fn);
    RA = range_analyze(fn, ARA);
  };
  bool changed = false;
  for (uint32_t b = 0; b < nb; ++b) {
    const Instr &tm = fn.code[fn.blocks[b].end - 1];
    if (tm.k != K::Cbr || tm.a == NONE || tm.a >= nv) continue;
    if (tm.t == NONE || tm.f == NONE || tm.t == tm.f) continue;
    // cond range [0,1] AT THE CBR: walk this block's transfers from its
    // entry state (the cond may be defined inside the block, e.g. n & 1)
    Iv cond = {INT64_MIN, INT64_MAX, false};
    ensure_ra();
    if (RA.ok && b < RA.nb && tm.a < RA.nv) {
      std::vector<Iv> st(RA.entry.begin() + (size_t)b * RA.nv,
                         RA.entry.begin() + (size_t)(b + 1) * RA.nv);
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
        if (fn.code[i].k != K::Phi)
          iv_transfer(fn, i, RA.nv, st);
      cond = st[tm.a];
    }
    if (cond.bot || cond.lo < 0 || cond.hi > 1) continue;
    // arms: X_t / X_f: one Const def + br N (same N), no phis at N feeding...
    auto arm = [&](uint32_t xb, val_t &K, uint32_t &nx) -> bool {
      if (xb >= nb) return false;
      const Instr &at = fn.code[fn.blocks[xb].end - 1];
      if (at.k != K::Br || at.t == NONE) return false;
      nx = at.t;
      uint32_t nconst = 0;
      val_t kk = 0;
      for (uint32_t i = fn.blocks[xb].begin; i + 1 < fn.blocks[xb].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k == K::Const) {
          ++nconst;
          kk = I.c;
        } else
          return false;
      }
      if (nconst != 1) return false;
      K = kk;
      return true;
    };
    val_t Kt = 0, Kf = 0;
    uint32_t nt_ = NONE, nf = NONE;
    if (!arm(tm.t, Kt, nt_)) continue;
    if (!arm(tm.f, Kf, nf)) continue;
    if (nt_ != nf) continue; // must converge immediately
    // SOUNDNESS: each arm block must be reachable ONLY through this select.
    // The convergence phi's args are keyed by the ARM BLOCKS, so any OTHER
    // predecessor of an arm block also contributes to the phi (with the
    // same const, but through a path that never executes this select); the
    // arithmetic below is defined only on the select's own paths, so such
    // a block would leave the value undefined there (this exact shape
    // occurs when one short-circuit && feeds another: the inner select's
    // false arm doubles as the outer select's false target).
    {
      const auto &pt_ = fn.preds[tm.t];
      const auto &pf_ = fn.preds[tm.f];
      if (pt_.size() != 1 || pt_[0] != b) continue;
      if (pf_.size() != 1 || pf_[0] != b) continue;
    }
    const uint32_t N = nt_;
    // N's first phi (if any) must merge EXACTLY the two arm consts — its
    // dst is the select result. If N has no phi, the arms' consts are
    // dead and the branch is pure control: still convertible only if both
    // arms' consts are unused... keep it simple: require the phi.
    uint32_t pi = fn.blocks[N].begin;
    if (pi >= fn.blocks[N].end || fn.code[pi].k != K::Phi) continue;
    const Instr &P = fn.code[pi];
    // args keyed from the arm blocks
    auto argfrom = [&](uint32_t key) -> uint32_t {
      if (P.t == key) return P.a;
      if (P.f == key) return P.b2;
      return NONE;
    };
    uint32_t at_ = argfrom(tm.t), af_ = argfrom(tm.f);
    if (at_ == NONE || af_ == NONE) continue;
    // the phi args must be exactly the arms' Const vregs
    if (defi[at_] == NONE || defi[af_] == NONE) continue;
    if (fn.code[defi[at_]].k != K::Const || fn.code[defi[af_]].k != K::Const)
      continue;
    // (defb of the consts must be the arm blocks)
    if (defb[at_] != tm.t || defb[af_] != tm.f) continue;
    // ---- convert: phiDst = Kf + (Kt - Kf) * c ------------------------------
    std::vector<Instr> pre{};
    uint32_t d = P.dst;
    uint32_t kf = new_anon_vreg(fn, 0);
    pre.push_back({.k = K::Const, .dst = kf, .c = Kf});
    uint32_t diff = new_anon_vreg(fn, 0);
    pre.push_back({.k = K::Const, .dst = diff,
                   .c = (val_t)((uint64_t)Kt - (uint64_t)Kf)});
    uint32_t m = new_anon_vreg(fn, 0);
    pre.push_back(
        {.k = K::Bin, .b = B::Mul, .dst = m, .a = tm.a, .b2 = diff});
    pre.push_back({.k = K::Bin, .b = B::Add, .dst = d, .a = kf, .b2 = m});
    // insert before b's terminator; terminator becomes br N
    uint32_t be = fn.blocks[b].end - 1;
    fn.code.insert(fn.code.begin() + (long)be, pre.begin(), pre.end());
    const uint32_t add = (uint32_t)pre.size();
    for (uint32_t xb = 0; xb < nb; ++xb) {
      if (xb == b) {
        fn.blocks[xb].end += add;
        continue;
      }
      if (fn.blocks[xb].begin >= be + 1) fn.blocks[xb].begin += add;
      if (fn.blocks[xb].end > be) fn.blocks[xb].end += add;
    }
    {
      Instr &bt = fn.code[fn.blocks[b].end - 1];
      bt.k = K::Br;
      bt.a = NONE;
      bt.f = NONE;
      bt.t = N;
      fn.succ[2 * b] = N;
      fn.succ[2 * b + 1] = NONE;
    }
    // DELETE the phi at N: its dst is now defined by the arithmetic in b
    // (leaving it would give the vreg TWO defs, and once the old arms go
    // unreachable the phi's pred keys all die and unreach would "degenerate"
    // it to an undef Const 0 -- clobbering the select result)
    {
      uint32_t del = fn.blocks[N].begin;
      fn.code.erase(fn.code.begin() + (long)del);
      for (uint32_t xb = 0; xb < fn.nblocks(); ++xb) {
        if (xb == N) {
          fn.blocks[xb].end -= 1;
          continue;
        }
        if (fn.blocks[xb].begin > del) fn.blocks[xb].begin -= 1;
        if (fn.blocks[xb].end > del) fn.blocks[xb].end -= 1;
      }
    }
    os.ifconvs++;
    changed = true;
    break; // one per call: the fixpoint driver re-runs
  }
  return changed;
}


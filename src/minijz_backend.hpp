// ============================================================================
// MiniJZ compiler — §13.5 backends: C, x86-64 asm, bytecode listing, emission plan, register allocation
// ----------------------------------------------------------------------------
// Ordered-include header: this file is a slice of the original single
// translation unit and relies on being included after its predecessors
// (see src/minijz.hpp for the canonical order). Do not include it
// directly in an arbitrary order.
// ============================================================================
#pragma once

// 13.5 BACKENDS (runtime only): C, x86-64 asm (GAS), bytecode listing,
//      and the native build&run harness used by --run.
//      All backends consume the DESTROYED IR (no phis).
//      Output text contract shared by every runner (incl. interpreters):
//        one "%lld\n" line per print; "TRAP\n" appended if a div/mod traps;
//        "TIMEOUT\n" if a step-limited runner hit its limit.
// ============================================================================

// canonical text of an in-process RunOut — comparable with native stdout
static std::string text_of(const RunOut &R) {
  std::string s{};
  char buf[32];
  for (val_t v : R.out) {
    std::snprintf(buf, sizeof buf, "%lld\n", (long long)v);
    s += buf;
  }
  if (R.trapped) s += "TRAP\n";
  else if (R.timeout) s += "TIMEOUT\n";
  return s;
}

// ---- 13.5z. EMISSION PLAN -----------------------------------------------------
// Everything the native backends need to emit non-redundant code, computed
// once from the destroyed IR:
//   * block layout: reachable blocks in RPO order, extended by "traces" that
//     chain a block with its preferred unplaced successor (fewer preds wins;
//     ties prefer the else/false arm). With a successor emitted right after,
//     `goto`/`jmp` to it is dropped (fallthrough) in both backends.
//   * constants: every use of a Const-defined vreg is inlined as a literal;
//     the Const def and its C declaration disappear.
//   * branch fusion: a cbr whose condition is a single-use comparison
//     becomes `if (a < b) goto L;` / `cmp+jcc` — no setcc/movzbq/roundtrip.
//   * dead flags: pure instrs whose dst has no remaining textual use.
struct EmitPlan {
  std::vector<uint32_t> order{};  // reachable blocks, emission order
  std::vector<uint32_t> next{};   // per block: block emitted right after
  std::vector<char> kc{};         // per vreg: defined by Const
  std::vector<val_t> kv{};        // per vreg: that constant
  std::vector<uint32_t> uses{};   // per vreg: textual uses after inlining
  std::vector<uint32_t> fuse{};   // per instr: Cbr -> fused compare def idx
  std::vector<char> dead{};       // per instr: skip (pure & unused / inlined)
};

static EmitPlan make_emit_plan(const Fn &fn) {
  EmitPlan P{};
  const uint32_t nb = fn.nblocks();
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  // reachability from entry
  std::vector<char> reach(nb, 0);
  {
    std::vector<uint32_t> st{0};
    reach[0] = 1;
    while (!st.empty()) {
      uint32_t b = st.back();
      st.pop_back();
      for (uint32_t s : {fn.succ0(b), fn.succ1(b)})
        if (s != NONE && !reach[s]) {
          reach[s] = 1;
          st.push_back(s);
        }
    }
  }
  // reverse postorder (succ0 first => loop headers precede their bodies)
  std::vector<uint32_t> rpo{};
  {
    std::vector<uint32_t> post{};
    post.reserve(nb);
    std::vector<char> seen(nb, 0);
    std::vector<std::pair<uint32_t, uint32_t>> st{};
    if (reach[0]) {
      seen[0] = 1;
      st.push_back({0, 0});
    }
    while (!st.empty()) {
      uint32_t b = st.back().first, ci = st.back().second;
      if (ci < 2) {
        uint32_t s = (ci == 0) ? fn.succ0(b) : fn.succ1(b);
        ++st.back().second;
        if (s != NONE && reach[s] && !seen[s]) {
          seen[s] = 1;
          st.push_back({s, 0});
        }
      } else {
        post.push_back(b);
        st.pop_back();
      }
    }
    rpo.assign(post.rbegin(), post.rend());
  }
  // trace layout: chain each block with its preferred unplaced successor
  std::vector<char> placed(nb, 0);
  P.next.assign(nb, NONE);
  for (uint32_t b : rpo) {
    if (!reach[b] || placed[b]) continue;
    uint32_t cur = b;
    for (;;) {
      placed[cur] = 1;
      P.order.push_back(cur);
      uint32_t c0 = fn.succ0(cur), c1 = fn.succ1(cur);
      auto cand = [&](uint32_t s) -> bool {
        return s != NONE && s < nb && reach[s] && !placed[s];
      };
      uint32_t pick = NONE;
      if (cand(c0) && cand(c1))
        pick = (fn.preds[c1].size() < fn.preds[c0].size()) ? c1 : c0;
      else if (cand(c1))
        pick = c1; // else/false arm: `if (c) goto T; <else falls through>`
      else if (cand(c0))
        pick = c0;
      if (pick == NONE) break;
      cur = pick;
    }
  }
  for (size_t k = 0; k + 1 < P.order.size(); ++k)
    P.next[P.order[k]] = P.order[k + 1];
  // constants: a vreg is literal-inlinable ONLY when it has exactly ONE def
  // and that def is a Const. Post-destruction phi slots can hold several
  // defs, one of which post_destruct_peephole may have folded into a Const
  // (e.g. `c3 = 5` on the entry edge, `c3 = t` on the back edge): inlining
  // the one Const def at every use would freeze the slot's other-path
  // values to that constant and miscompile the loop test.
  P.kc.assign(nv, 0);
  P.kv.assign(nv, 0);
  {
    std::vector<uint32_t> ndefs(nv, 0);
    for (const Instr &I : fn.code)
      if (I.dst != NONE && I.dst < nv) ++ndefs[I.dst];
    for (const Instr &I : fn.code)
      if (I.k == K::Const && I.dst != NONE && I.dst < nv &&
          ndefs[I.dst] == 1) {
        P.kc[I.dst] = 1;
        P.kv[I.dst] = I.c;
      }
  }
  // raw use counts + def index (for fusion decisions); ndefs also gates
  // fusion (see the comment at the fuse loop below)
  std::vector<uint32_t> raw(nv, 0);
  std::vector<uint32_t> def_idx(nv, NONE);
  std::vector<uint32_t> ndefs(nv, 0);
  for (uint32_t i = 0; i < fn.code.size(); ++i)
    if (fn.code[i].dst != NONE && fn.code[i].dst < nv) ++ndefs[fn.code[i].dst];
  for (uint32_t i = 0; i < fn.code.size(); ++i) {
    const Instr &I = fn.code[i];
    if (I.a != NONE && I.a < nv) raw[I.a]++;
    if (I.b2 != NONE && I.b2 < nv) raw[I.b2]++;
    if (I.c2 != NONE && I.c2 < nv) raw[I.c2]++;
    for (uint32_t v : I.args)
      if (v < nv) raw[v]++;
    if (I.dst != NONE && I.dst < nv) def_idx[I.dst] = i;
  }
  // branch fusion: cbr on a single-use comparison
  P.fuse.assign(fn.code.size(), NONE);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k != K::Cbr || I.a == NONE || I.a >= nv) continue;
      uint32_t d = def_idx[I.a];
      // single USE is not enough post-destruction: a phi slot has one use
      // (this branch) but one def per incoming edge; fusing "the" def would
      // pick an arbitrary edge's comparison. Require a single DEF too.
      if (d == NONE || raw[I.a] != 1 || ndefs[I.a] != 1) continue;
      const Instr &D = fn.code[d];
      bool is_cmp = D.k == K::Bin &&
                    (D.b == B::Lt || D.b == B::Le || D.b == B::Gt ||
                     D.b == B::Ge || D.b == B::Eq || D.b == B::Ne);
      if (is_cmp) P.fuse[i] = d;
    }
  // textual uses (consts inlined, fused cbrs read the comparison operands)
  P.uses.assign(nv, 0);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k == K::Const) continue; // inlined at every use
      if (P.fuse[i] != NONE) {
        const Instr &D = fn.code[P.fuse[i]];
        if (D.a != NONE && D.a < nv && !P.kc[D.a]) P.uses[D.a]++;
        if (D.b2 != NONE && D.b2 < nv && !P.kc[D.b2]) P.uses[D.b2]++;
        continue;
      }
      for (uint32_t v : {I.a, I.b2, I.c2})
        if (v != NONE && v < nv && !P.kc[v]) P.uses[v]++;
      for (uint32_t v : I.args)
        if (v != NONE && v < nv && !P.kc[v]) P.uses[v]++;
    }
  // dead: pure instrs whose dst is never textually read
  P.dead.assign(fn.code.size(), 0);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k == K::Const) {
        // dead (inlined at every use) only when the slot is const-inlinable,
        // i.e. single-def; a multi-def slot's Const edge write is real code
        P.dead[i] =
            (I.dst != NONE && I.dst < nv && P.kc[I.dst]) ? 1 : 0;
        continue;
      }
      if (I.dst == NONE || I.dst >= nv) continue;
      if (P.uses[I.dst] != 0) continue;
      bool pure = I.k == K::Copy || I.k == K::Len ||
                  (I.k == K::Bin && I.b != B::Div && I.b != B::Mod);
      if (pure) P.dead[i] = 1;
    }
  for (uint32_t d : P.fuse)
    if (d != NONE) P.dead[d] = 1; // consumed by the branch text
  return P;
}


// ---- 13.5a. C backend --------------------------------------------------------
// Emits C valid as both C and C++. One function per FnUnit: helpers get
// `static int64_t mjz_fN(...)` signatures (params are vregs v0..vP-1), a
// depth guard (runaway recursion TRAPs exactly like the interpreters/VM),
// and MJZ_RET epilogues; main is `int main(void)`. Arithmetic goes through
// uint64_t casts: the wraps the interpreters produce are UB-free here;
// div/mod are guarded so the trap fires BEFORE the UB division would.
static std::string emit_c_fn(const Fn &fn, uint32_t fidx, bool is_main,
                            bool multi_fn) {
  EmitPlan P = make_emit_plan(fn);
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  // def index (for the unsigned-compare pretty-printer below)
  std::vector<uint32_t> cdefi(nv, NONE);
  std::vector<uint32_t> nuse(nv, 0);
  for (uint32_t b0 = 0; b0 < fn.nblocks(); ++b0)
    for (uint32_t i0 = fn.blocks[b0].begin; i0 < fn.blocks[b0].end; ++i0)
      if (fn.code[i0].dst != NONE && fn.code[i0].dst < nv)
        cdefi[fn.code[i0].dst] = i0;
  for (uint32_t b0 = 0; b0 < fn.nblocks(); ++b0)
    for (uint32_t i0 = fn.blocks[b0].begin; i0 < fn.blocks[b0].end; ++i0) {
      for (uint32_t o : {fn.code[i0].a, fn.code[i0].b2, fn.code[i0].c2})
        if (o != NONE && o < nv) ++nuse[o];
      for (uint32_t o : fn.code[i0].args)
        if (o != NONE && o < nv) ++nuse[o];
    }
  std::vector<char> elide(fn.code.size(), 0); // xor elided by the pretty form
  { // pre-pass: every fused Cbr whose compare matches the (v^MIN vs const)
    // unsigned pattern and whose xor operand has this compare as its ONLY
    // use -> the xor assignment is elided (the if reads v directly)
    auto xorbase = [&](uint32_t o) -> uint32_t {
      if (o == NONE || o >= nv || cdefi[o] == NONE) return NONE;
      const Instr &X = fn.code[cdefi[o]];
      if (X.k != K::Bin || X.b != B::Xor) return NONE;
      if (X.a == NONE || X.a >= nv || X.b2 == NONE || X.b2 >= nv)
        return NONE;
      if (P.kc[X.b2] && P.kv[X.b2] == INT64_MIN) return X.a;
      if (P.kc[X.a] && P.kv[X.a] == INT64_MIN) return X.b2;
      return NONE;
    };
    for (uint32_t b0 = 0; b0 < fn.nblocks(); ++b0)
      for (uint32_t i0 = fn.blocks[b0].begin; i0 < fn.blocks[b0].end; ++i0) {
        const Instr &C = fn.code[i0];
        if (C.k != K::Cbr || P.fuse[i0] == NONE) continue;
        const Instr &D = fn.code[P.fuse[i0]];
        uint32_t xv = xorbase(D.a), yv = xorbase(D.b2);
        bool left = xv != NONE && yv == NONE && D.b2 < nv && P.kc[D.b2];
        bool right = yv != NONE && xv == NONE && D.a < nv && P.kc[D.a];
        if (!left && !right) continue;
        uint32_t xo = left ? D.a : D.b2;
        if (xo < nv && nuse[xo] == 1 && cdefi[xo] != NONE)
          elide[cdefi[xo]] = 1;
      }
  }
  std::string s{};
  char buf[192];
  // home naming: r%u (allocated slot) when register allocation ran,
  // v%u (one local per vreg, "SSA-like") with --no-regalloc / -O0
  const bool ra = !fn.slot_of.empty();
  auto D = [&](uint32_t v) -> std::string { // home of a DEF/write (vreg id)
    if (ra) std::snprintf(buf, sizeof buf, "r%u", (unsigned)slot_id(fn, v));
    else std::snprintf(buf, sizeof buf, "v%u", (unsigned)v);
    return std::string(buf);
  };
  auto SN = [&](uint32_t slot) -> std::string { // name of a SLOT index
    std::snprintf(buf, sizeof buf, ra ? "r%u" : "v%u", (unsigned)slot);
    return std::string(buf);
  };
  // stack-local arrays (analysis ran on the SSA form): vreg -> entry index
  std::vector<int32_t> arr_of(nv, -1);
  for (size_t k = 0; k < fn.stk_arrays.size(); ++k)
    for (uint32_t v : fn.stk_arrays[k].vregs)
      if (v < nv) arr_of[v] = (int32_t)k;
  auto arr_name = [&](size_t k) -> std::string {
    return "mjz_s" + std::to_string(fn.stk_arrays[k].root);
  };
  // operand text: inlined literal or variable (consts are always inlined)
  auto V = [&](uint32_t v) -> std::string {
    if (v == NONE) return "0";
    if (v < nv && P.kc[v]) {
      val_t c = P.kv[v];
      if (c >= -2147483647 && c <= 2147483647)
        std::snprintf(buf, sizeof buf, "%lld", (long long)c);
      else
        std::snprintf(buf, sizeof buf, "UINT64_C(0x%016llx)",
                      (unsigned long long)(uint64_t)c);
      return std::string(buf);
    }
    return D(v);
  };
  auto cmp_txt = [](B b) -> const char * {
    switch (b) {
    case B::Lt: return "<";
    case B::Le: return "<=";
    case B::Gt: return ">";
    case B::Ge: return ">=";
    case B::Eq: return "==";
    default: return "!=";
    }
  };
  // negation of a comparison (for branches that fall through on the true
  // arm): `if (!(a < b)) goto L;` becomes `if (a >= b) goto L;` — the NOT of
  // a compare is trivially combinable into the compare itself
  auto neg_txt = [](B b) -> const char * {
    switch (b) {
    case B::Lt: return ">=";
    case B::Le: return ">";
    case B::Gt: return "<=";
    case B::Ge: return "<";
    case B::Eq: return "!=";
    default: return "==";
    }
  };
  // signature (non-main: parameters are the first vregs, v0..vP-1)
  if (is_main) {
    s += "int main(void) {\n";
  } else {
    s += "static int64_t mjz_f" + std::to_string(fidx) + "(";
    for (uint32_t p = 0; p < fn.nparams; ++p)
      s += p ? ", int64_t " + D(p) : "int64_t " + D(p);
    s += ") {\n";
    if (multi_fn)
      s += "  if (++mjz_depth > 512) { printf(\"TRAP\\n\"); exit(1); }\n";
  }
  // declarations: every slot WRITTEN by a non-dead instruction needs one
  // (written-but-unread slots still exist: e.g. a kept-for-trap Alloc whose
  // handle is never used). Read-only slots cannot exist post-SSA.
  // No initializers: SSA dominance (+ the explicit `Const 0` materialization
  // of undef phi args during destruction) guarantees every slot is written
  // before any read on every path — the same property the asm backend has
  // relied on since it skipped slot zero-init. C compilers at -O2 can now
  // keep these in registers.
  {
    const uint32_t ns = slot_count(fn);
    std::vector<char> written(ns, 0);
    for (uint32_t b : P.order)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        if (P.dead[i]) continue;
        uint32_t dst = fn.code[i].dst;
        if (dst != NONE && dst < nv) written[slot_id(fn, dst)] = 1;
      }
    for (uint32_t sl = fn.nparams; sl < ns; ++sl) {
      if (!written[sl]) continue;
      std::snprintf(buf, sizeof buf, "  int64_t %s;\n", SN(sl).c_str());
      s += buf;
    }
  }
  for (size_t k = 0; k < fn.stk_arrays.size(); ++k) { // local arrays
    std::snprintf(buf, sizeof buf, "  int64_t mjz_s%u[%lld];\n",
                  (unsigned)fn.stk_arrays[k].root,
                  (long long)fn.stk_arrays[k].len);
    s += buf;
  }
  // referenced labels (so fallthrough-only blocks need no label at all)
  std::vector<char> refd(fn.nblocks(), 0);
  for (uint32_t b : P.order)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (P.dead[i]) continue;
      if (I.k == K::Br && I.t != P.next[b]) refd[I.t] = 1;
      if (I.k == K::Cbr) {
        if (P.fuse[i] == NONE && I.a < nv && P.kc[I.a]) {
          uint32_t tgt = (P.kv[I.a] != 0) ? I.t : I.f; // constant condition
          if (tgt != P.next[b]) refd[tgt] = 1;
        } else if (I.f == P.next[b]) {
          refd[I.t] = 1; // `if (cond) goto L_t;` false arm falls through
        } else if (I.t == P.next[b]) {
          refd[I.f] = 1; // `if (!(cond)) goto L_f;` true arm falls through
        } else {
          refd[I.t] = 1;
          refd[I.f] = 1;
        }
      }
    }
  bool saw_exit = false;
  // body, in layout order: one successor falls through, goto elided
  for (uint32_t b : P.order) {
    if (refd[b]) {
      std::snprintf(buf, sizeof buf, "L%u:;\n", (unsigned)b);
      s += buf;
    }
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (P.dead[i] || elide[i]) continue;
      switch (I.k) {
      case K::Phi:
        break; // destroyed input has none
      case K::Const:
        if (!P.dead[i]) { // multi-def slot: a real write of THIS constant
          val_t c = I.c;
          if (c >= -2147483647 && c <= 2147483647)
            std::snprintf(buf, sizeof buf, "  %s = %lld;\n", D(I.dst).c_str(),
                          (long long)c);
          else
            std::snprintf(buf, sizeof buf, "  %s = UINT64_C(0x%016llx);\n",
                          D(I.dst).c_str(),
                          (unsigned long long)(uint64_t)c);
          s += buf;
        }
        break; // dead ones are inlined at their uses
      case K::Copy:
        if (D(I.dst) == V(I.a))
          break; // same slot after coalescing: a no-op self-assignment
        std::snprintf(buf, sizeof buf, "  %s = %s;\n", D(I.dst).c_str(),
                      V(I.a).c_str());
        s += buf;
        break;
      case K::Bin: {
        if (I.b == B::Add || I.b == B::Sub || I.b == B::Mul) {
          const char *op = I.b == B::Add ? "+" : (I.b == B::Sub ? "-" : "*");
          std::snprintf(buf, sizeof buf,
                        "  %s = (int64_t)((uint64_t)%s %s (uint64_t)%s);\n",
                        D(I.dst).c_str(), V(I.a).c_str(), op, V(I.b2).c_str());
          s += buf;
        } else if (I.b == B::Div || I.b == B::Mod) {
          const char *op = I.b == B::Div ? "/" : "%";
          bool bc = I.b2 < nv && P.kc[I.b2];
          val_t bv = bc ? P.kv[I.b2] : 0;
          bool ac = I.a < nv && P.kc[I.a];
          val_t av = ac ? P.kv[I.a] : 0;
          bool always =
              bc && (bv == 0 || (bv == -1 && ac && av == INT64_MIN));
          // a trap must terminate the WHOLE program, exactly like the
          // oracle/VM/asm backends (mjz_trap: puts+exit). `return 1` would
          // only leave the helper and let the caller continue running.
          if (always) {
            s += "  printf(\"TRAP\\n\"); exit(1);\n";
            break;
          }
          if (bc && bv == -1)
            s += "  if (" + V(I.a) +
                 " == INT64_MIN) { printf(\"TRAP\\n\"); exit(1); }\n";
          else if (!bc)
            s += "  if (" + V(I.b2) + " == 0 || (" + V(I.a) +
                 " == INT64_MIN && " + V(I.b2) +
                 " == -1)) { printf(\"TRAP\\n\"); exit(1); }\n";
          std::snprintf(buf, sizeof buf, "  %s = %s %s %s;\n",
                        D(I.dst).c_str(), V(I.a).c_str(), op, V(I.b2).c_str());
          s += buf;
        } else if (I.b == B::And || I.b == B::Or || I.b == B::Xor) {
          const char *op = I.b == B::And ? "&" : (I.b == B::Or ? "|" : "^");
          std::snprintf(buf, sizeof buf,
                        "  %s = (int64_t)((uint64_t)%s %s (uint64_t)%s);\n",
                        D(I.dst).c_str(), V(I.a).c_str(), op, V(I.b2).c_str());
          s += buf;
        } else if (I.b == B::MulHi) {
          std::snprintf(buf, sizeof buf,
                        "  %s = (int64_t)(((unsigned __int128)(uint64_t)%s * "
                        "(uint64_t)%s) >> 64);\n",
                        D(I.dst).c_str(), V(I.a).c_str(), V(I.b2).c_str());
          s += buf;
        } else if (I.b == B::Shl || I.b == B::LShr || I.b == B::AShr) {
          // amounts masked to 6 bits, matching the interpreters/VM exactly
          if (I.b == B::Shl)
            std::snprintf(buf, sizeof buf,
                          "  %s = (int64_t)((uint64_t)%s << ((uint64_t)%s & 63));\n",
                          D(I.dst).c_str(), V(I.a).c_str(), V(I.b2).c_str());
          else if (I.b == B::LShr)
            std::snprintf(buf, sizeof buf,
                          "  %s = (int64_t)((uint64_t)%s >> ((uint64_t)%s & 63));\n",
                          D(I.dst).c_str(), V(I.a).c_str(), V(I.b2).c_str());
          else
            std::snprintf(buf, sizeof buf,
                          "  %s = %s >> (int64_t)((uint64_t)%s & 63);\n",
                          D(I.dst).c_str(), V(I.a).c_str(), V(I.b2).c_str());
          s += buf;
        } else {
          std::snprintf(buf, sizeof buf, "  %s = (%s %s %s);\n",
                        D(I.dst).c_str(), V(I.a).c_str(), cmp_txt(I.b),
                        V(I.b2).c_str());
          s += buf;
        }
        break;
      }
      case K::Br:
        if (I.t != P.next[b]) { // else: fall through
          std::snprintf(buf, sizeof buf, "  goto L%u;\n", (unsigned)I.t);
          s += buf;
        }
        break;
      case K::Cbr: {
        uint32_t tgt = NONE;
        if (P.fuse[i] != NONE) { // fused: branch on the comparison itself;
          // when the TRUE arm is the fallthrough, the NOT of the compare is
          // folded INTO the compare: `if (!(a<b)) goto L;` -> `if (a>=b) goto L;`
          const Instr &D = fn.code[P.fuse[i]];
          std::string pos, neg;
          { // unsigned-compare pretty-printer: (v ^ INT64_MIN) < (K ^ MIN)
            // as SIGNED compare on the xor'd values is exactly the UNSIGNED
            // compare v < K — emit it in that readable form
            auto xorbase = [&](uint32_t o) -> uint32_t {
              if (o == NONE || o >= nv || cdefi[o] == NONE) return NONE;
              const Instr &X = fn.code[cdefi[o]];
              if (X.k != K::Bin || X.b != B::Xor) return NONE;
              if (X.a == NONE || X.a >= nv || X.b2 == NONE || X.b2 >= nv)
                return NONE;
              if (P.kc[X.b2] && P.kv[X.b2] == INT64_MIN) return X.a;
              if (P.kc[X.a] && P.kv[X.a] == INT64_MIN) return X.b2;
              return NONE;
            };
            auto flip = [](B o) -> B {
              switch (o) {
              case B::Lt: return B::Gt;
              case B::Le: return B::Ge;
              case B::Gt: return B::Lt;
              case B::Ge: return B::Le;
              default: return o;
              }
            };
            uint32_t xv = xorbase(D.a), yv = xorbase(D.b2);
            bool left = xv != NONE && yv == NONE && D.b2 < nv && P.kc[D.b2];
            bool right = yv != NONE && xv == NONE && D.a < nv && P.kc[D.a];
            if (left || right) {
              uint32_t v = left ? xv : yv;
              val_t bound = (val_t)((uint64_t)(left ? P.kv[D.b2] : P.kv[D.a]) ^
                                    (uint64_t)INT64_MIN);
              B op = left ? D.b : flip(D.b); // normalize: v on the left
              char btxt[40];
              if (bound >= 0 && bound <= 3037000499ll)
                std::snprintf(btxt, sizeof btxt, "(uint64_t)%lld",
                              (long long)bound);
              else
                std::snprintf(btxt, sizeof btxt, "UINT64_C(0x%016llx)",
                              (unsigned long long)(uint64_t)bound);
              pos = "((uint64_t)" + V(v) + " " + cmp_txt(op) + " " + btxt +
                    ")";
              neg = "((uint64_t)" + V(v) + " " + neg_txt(op) + " " + btxt +
                    ")";
            } else {
              pos = "(" + V(D.a) + " " + cmp_txt(D.b) + " " + V(D.b2) + ")";
              neg = "(" + V(D.a) + " " + neg_txt(D.b) + " " + V(D.b2) + ")";
            }
          }
          if (I.f == P.next[b]) { // false arm falls through
            std::snprintf(buf, sizeof buf, "  if %s goto L%u;\n", pos.c_str(),
                          (unsigned)I.t);
            s += buf;
          } else if (I.t == P.next[b]) { // true arm falls through: negated
            std::snprintf(buf, sizeof buf, "  if %s goto L%u;\n", neg.c_str(),
                          (unsigned)I.f);
            s += buf;
          } else {
            std::snprintf(buf, sizeof buf, "  if %s goto L%u;\n", pos.c_str(),
                          (unsigned)I.t);
            s += buf;
            std::snprintf(buf, sizeof buf, "  goto L%u;\n", (unsigned)I.f);
            s += buf;
          }
          break;
        }
        if (I.a < nv && P.kc[I.a]) { // constant condition
          tgt = (P.kv[I.a] != 0) ? I.t : I.f;
          if (tgt != P.next[b]) {
            std::snprintf(buf, sizeof buf, "  goto L%u;\n", (unsigned)tgt);
            s += buf;
          }
          break;
        }
        std::string cond = "(" + V(I.a) + " != 0)";
        if (I.f == P.next[b]) { // false arm falls through
          std::snprintf(buf, sizeof buf, "  if %s goto L%u;\n", cond.c_str(),
                        (unsigned)I.t);
          s += buf;
        } else if (I.t == P.next[b]) { // true arm falls through: invert
          std::snprintf(buf, sizeof buf, "  if (!(%s)) goto L%u;\n",
                        cond.c_str(), (unsigned)I.f);
          s += buf;
        } else {
          std::snprintf(buf, sizeof buf, "  if %s goto L%u;\n", cond.c_str(),
                        (unsigned)I.t);
          s += buf;
          std::snprintf(buf, sizeof buf, "  goto L%u;\n", (unsigned)I.f);
          s += buf;
        }
        break;
      }
      case K::Print:
        std::snprintf(buf, sizeof buf,
                      "  printf(\"%%lld\\n\", (long long)%s);\n",
                      V(I.a).c_str());
        s += buf;
        break;
      case K::Exit:
        saw_exit = true;
        s += "  return 0;\n";
        break;
      case K::Alloc:
        if (I.dst < nv && arr_of[I.dst] >= 0) { // local: zero-fill a stack array
          std::snprintf(buf, sizeof buf, "  memset(%s, 0, sizeof %s);\n",
                        arr_name(arr_of[I.dst]).c_str(),
                        arr_name(arr_of[I.dst]).c_str());
          s += buf;
        } else {
          std::snprintf(buf, sizeof buf, "  %s = mjz_alloc(%s);\n",
                        D(I.dst).c_str(), V(I.a).c_str());
          s += buf;
        }
        break;
      case K::Load:
        if (I.a < nv && arr_of[I.a] >= 0) { // local array: direct + bounds check
          std::snprintf(buf, sizeof buf,
                        "  if (%s < 0 || %s >= %lld) { printf(\"TRAP\\n\"); "
                        "exit(1); }\n",
                        V(I.b2).c_str(), V(I.b2).c_str(),
                        (long long)fn.stk_arrays[arr_of[I.a]].len);
          s += buf;
          std::snprintf(buf, sizeof buf, "  %s = %s[%s];\n", D(I.dst).c_str(),
                        arr_name(arr_of[I.a]).c_str(), V(I.b2).c_str());
          s += buf;
        } else {
          std::snprintf(buf, sizeof buf, "  %s = mjz_load(%s, %s);\n",
                        D(I.dst).c_str(), V(I.a).c_str(), V(I.b2).c_str());
          s += buf;
        }
        break;
      case K::Store:
        if (I.a < nv && arr_of[I.a] >= 0) { // local array: direct + bounds check
          std::snprintf(buf, sizeof buf,
                        "  if (%s < 0 || %s >= %lld) { printf(\"TRAP\\n\"); "
                        "exit(1); }\n",
                        V(I.b2).c_str(), V(I.b2).c_str(),
                        (long long)fn.stk_arrays[arr_of[I.a]].len);
          s += buf;
          std::snprintf(buf, sizeof buf, "  %s[%s] = %s;\n",
                        arr_name(arr_of[I.a]).c_str(), V(I.b2).c_str(),
                        V(I.c2).c_str());
          s += buf;
        } else {
          std::snprintf(buf, sizeof buf, "  mjz_store(%s, %s, %s);\n",
                        V(I.a).c_str(), V(I.b2).c_str(), V(I.c2).c_str());
          s += buf;
        }
        break;
      case K::Len:
        if (I.a < nv && arr_of[I.a] >= 0) { // local: length is a constant
          std::snprintf(buf, sizeof buf, "  %s = %lld;\n", D(I.dst).c_str(),
                        (long long)fn.stk_arrays[arr_of[I.a]].len);
          s += buf;
        } else {
          std::snprintf(buf, sizeof buf, "  %s = mjz_len(%s);\n",
                        D(I.dst).c_str(), V(I.a).c_str());
          s += buf;
        }
        break;
      case K::Input:
        std::snprintf(buf, sizeof buf, "  %s = mjz_input();\n",
                      D(I.dst).c_str());
        s += buf;
        break;
      case K::Depth: // frames incl. main: mjz_depth counts CALLEES (main=0),
        // so +1; in main it is provably 1 (nothing below the entry frame)
        if (is_main) {
          std::snprintf(buf, sizeof buf, "  %s = 1;\n", D(I.dst).c_str());
        } else {
          std::snprintf(buf, sizeof buf, "  %s = mjz_depth + 1;\n",
                        D(I.dst).c_str());
        }
        s += buf;
        break;
      case K::Call: {
        std::string as{};
        for (size_t k = 0; k < I.args.size(); ++k)
          as += k ? ", " + V(I.args[k]) : V(I.args[k]);
        std::snprintf(buf, sizeof buf, "  %s = mjz_f%u(%s);\n",
                      D(I.dst).c_str(), (unsigned)I.c, as.c_str());
        s += buf;
        break;
      }
      case K::Ret:
        if (multi_fn)
          s += "  MJZ_RET(" + V(I.a) + ");\n";
        else // main has no Ret; this is defensive
          s += "  return " + V(I.a) + ";\n";
        break;
      }
    }
  }
  // did the LAST emitted block end in a Ret? then control can never fall
  // off the end of the function and the safety epilogue is dead text
  bool ends_ret = false;
  if (!P.order.empty()) {
    uint32_t lb = P.order.back();
    for (uint32_t i = fn.blocks[lb].end; i-- > fn.blocks[lb].begin;) {
      if (P.dead[i]) continue;
      ends_ret = (fn.code[i].k == K::Ret);
      break;
    }
  }
  if (is_main)
    s += saw_exit ? "}\n" : "  return 0;\n}\n";
  else if (ends_ret)
    s += "}\n"; // every emitted path returned: nothing can fall through
  else           // unreachable-fallthrough safety for non-void fns
    s += "  --mjz_depth; return 0;\n}\n";
  return s;
}

// whole-program C: shared helpers, prototypes (mutual recursion is legal),
// callee bodies first, then main.
static std::string emit_c(const std::vector<Fn> &fns,
                          const std::vector<std::string_view> &names) {
  (void)names;
  std::string s{};
  bool use_arr = false, use_in = false, use_stk = false;
  auto stk_arr_of = [](const Fn &fn, uint32_t v) -> int {
    for (size_t k = 0; k < fn.stk_arrays.size(); ++k)
      for (uint32_t u : fn.stk_arrays[k].vregs)
        if (u == v) return (int)k;
    return -1;
  };
  for (const Fn &fn : fns) {
    if (!fn.stk_arrays.empty()) use_stk = true;
    for (const Instr &I : fn.code) {
      const uint32_t nv =
          fn.nvars + (uint32_t)fn.version_base.size();
      if (I.k == K::Alloc) {
        if (I.dst < nv && stk_arr_of(fn, I.dst) >= 0) continue; // stack
        use_arr = true;
      } else if (I.k == K::Load || I.k == K::Store || I.k == K::Len) {
        if (I.a < nv && stk_arr_of(fn, I.a) >= 0) continue; // stack
        use_arr = true;
      } else if (I.k == K::Input)
        use_in = true;
    }
  }
  const bool multi = fns.size() > 1;
  s += "/* generated by minijz */\n";
  s += "#include <stdio.h>\n#include <stdint.h>\n";
  s += "#include <stdlib.h>\n"; // exit(): div/mod traps can fire anywhere,
  //                              including single-fn programs without arrays
  if (use_stk) s += "#include <string.h>\n";                    // memset()
  if (use_arr) {
    // bump arena, bit-identical to the interpreters/VM/asm backends:
    // handle = index of first element; len[] is 0 for non-handles
    s += "#define MJZ_ARENA_CAP 2097152\n";
    s += "static int64_t mjz_arena[MJZ_ARENA_CAP + 1];\n";
    s += "static int64_t mjz_alen[MJZ_ARENA_CAP + 1];\n";
    s += "static int64_t mjz_atop = 0;\n";
    s += "static int64_t mjz_alloc(int64_t n) {\n";
    s += "  if (n < 0 || n > MJZ_ARENA_CAP ||\n";
    s += "      mjz_atop > MJZ_ARENA_CAP - n) { printf(\"TRAP\\n\"); exit(1); }\n";
    s += "  int64_t h = mjz_atop;\n";
    s += "  mjz_atop += n;\n";
    s += "  mjz_alen[h] = n;\n";
    s += "  return h;\n";
    s += "}\n";
    s += "static int64_t mjz_len(int64_t h) {\n";
    s += "  if (h < 0 || h > MJZ_ARENA_CAP) return 0;\n";
    s += "  return mjz_alen[h];\n";
    s += "}\n";
    s += "static int64_t mjz_load(int64_t h, int64_t i) {\n";
    s += "  if (i < 0 || i >= mjz_len(h)) { printf(\"TRAP\\n\"); exit(1); }\n";
    s += "  return mjz_arena[h + i];\n";
    s += "}\n";
    s += "static void mjz_store(int64_t h, int64_t i, int64_t v) {\n";
    s += "  if (i < 0 || i >= mjz_len(h)) { printf(\"TRAP\\n\"); exit(1); }\n";
    s += "  mjz_arena[h + i] = v;\n";
    s += "}\n";
  }
  if (use_in) {
    s += "static int64_t mjz_input(void) {\n";
    s += "  long long v = 0;\n";
    s += "  if (scanf(\"%lld\", &v) != 1) return 0;\n"; // EOF/parse fail -> 0
    s += "  return (int64_t)v;\n";
    s += "}\n";
  }
  s += "\n";

  if (multi) {
    s += "static int64_t mjz_depth = 0;\n";
    s += "#define MJZ_RET(x) do { --mjz_depth; "
         "return (int64_t)(x); } while (0)\n";
  }
  s += "\n";
  for (uint32_t f = 1; f < fns.size(); ++f) { // prototypes first
    const Fn &fn = fns[f];
    const bool ra = !fn.slot_of.empty();
    s += "static int64_t mjz_f" + std::to_string(f) + "(";
    for (uint32_t p = 0; p < fn.nparams; ++p) {
      char pb[32];
      if (ra) std::snprintf(pb, sizeof pb, "r%u", (unsigned)slot_id(fn, p));
      else std::snprintf(pb, sizeof pb, "v%u", (unsigned)p);
      s += p ? ", int64_t " + std::string(pb) : "int64_t " + std::string(pb);
    }
    s += ");\n";
  }
  if (fns.size() > 1) s += "\n";
  for (uint32_t f = 1; f < fns.size(); ++f) // callees first, main last
    s += emit_c_fn(fns[f], f, false, multi);
  s += emit_c_fn(fns[0], 0, true, multi);
  return s;
}

// ---- 13.5b.0 register allocation (chordal graph coloring, via mjz_lib) -----
// Live ranges are computed on the destroyed code (backward dataflow to a
// fixpoint); at every definition the def interferes with everything live
// after it. That interference graph is handed to mjz::graph_ns::
// color_chordal_graph — greedy coloring in maximum-cardinality-search order,
// which is OPTIMAL for chordal graphs, and SSA-shaped interference graphs are
// chordal. The pool is the five callee-saved registers rbx/r12-r15, so any
// value live across a call is safe with no extra saving; callers only need
// the (counted) push/pop of the subset actually used. Anything the coloring
// cannot house (color >= 5) spills to the classic -N(%rbp) slot.
struct RaPlan {
  std::vector<int32_t> color{};  // -1: no location; 0..4: register; 5: spill
  std::vector<uint32_t> spill{}; // per vreg: spill slot ordinal
  uint32_t nspill{};
  uint32_t usedmask{};
};
static RaPlan ra_plan(const Fn &fn, const EmitPlan &P) {
  const uint32_t nb = fn.nblocks();
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  RaPlan R{};
  R.color.assign(nv, -1);
  R.spill.assign(nv, 0);
  if (!nb || !nv) return R;
  // textual-use reader honoring inlined consts and fused branches
  auto each_use = [&](uint32_t i, auto &&f) {
    const Instr &I = fn.code[i];
    if (P.fuse[i] != NONE) { // the branch reads the comparison's operands
      const Instr &D = fn.code[P.fuse[i]];
      auto one = [&](uint32_t v) {
        if (v != NONE && v < nv && !P.kc[v]) f(v);
      };
      one(D.a);
      one(D.b2);
      return;
    }
    auto one = [&](uint32_t v) {
      if (v != NONE && v < nv && !P.kc[v]) f(v);
    };
    one(I.a);
    one(I.b2);
    one(I.c2);
    for (uint32_t v : I.args) one(v);
  };
  auto is_def = [&](uint32_t i) -> uint32_t { // live def of instr i (or NONE)
    const Instr &I = fn.code[i];
    if (P.dead[i]) return NONE;
    if (I.dst == NONE || I.dst >= nv || P.kc[I.dst]) return NONE;
    return I.dst;
  };
  // ---- block liveness (backward scan, fixpoint over successors) ----------
  std::vector<std::vector<char>> lin(nb, std::vector<char>(nv, 0)),
      lout(nb, std::vector<char>(nv, 0));
  for (int round = 0; round < 64; ++round) {
    bool changed = false;
    for (int32_t b = (int32_t)nb - 1; b >= 0; --b) {
      std::vector<char> &lo = lout[(uint32_t)b];
      std::fill(lo.begin(), lo.end(), 0);
      for (uint32_t s : {fn.succ0((uint32_t)b), fn.succ1((uint32_t)b)})
        if (s != NONE && s < nb)
          for (uint32_t v = 0; v < nv; ++v) lo[v] |= lin[s][v];
      std::vector<char> live = lo;
      for (uint32_t i = fn.blocks[(uint32_t)b].end;
           i-- > fn.blocks[(uint32_t)b].begin;) {
        if (P.dead[i]) continue;
        uint32_t d = is_def(i);
        if (d != NONE) live[d] = 0;
        each_use(i, [&](uint32_t v) { live[v] = 1; });
      }
      for (uint32_t v = 0; v < nv; ++v)
        if (live[v] != lin[(uint32_t)b][v]) {
          lin[(uint32_t)b][v] = live[v];
          changed = true;
        }
    }
    if (!changed) break;
  }
  // ---- interference edges ------------------------------------------------
  std::vector<std::vector<ulen>> adj(nv);
  auto edge = [&](uint32_t a, uint32_t b) {
    if (a == b || a >= nv || b >= nv) return;
    for (uint32_t x : adj[a])
      if (x == b) return;
    adj[a].push_back(b);
    adj[b].push_back(a);
  };
  { // parameters are "defined at entry": they conflict with each other and
    // with anything live into the entry block
    std::vector<char> live = lin[0];
    for (uint32_t p = 0; p < fn.nparams && p < nv; ++p) {
      for (uint32_t u = 0; u < nv; ++u)
        if (live[u] && u != p) edge(p, u);
      live[p] = 0;
    }
    // ... and PAIRWISE, always: the prologue unconditionally moves every
    // incoming argument into its home, so even a param whose only uses sit
    // in unreachable code (not live-in, no edges above) must not share a
    // home with a live sibling — the entry move would clobber it (fuzz
    // seed 804: p0 and p1 both colored rbx, `mov %rsi,%rbx` destroyed p0)
    for (uint32_t a = 0; a < fn.nparams && a < nv; ++a)
      for (uint32_t b = a + 1; b < fn.nparams && b < nv; ++b)
        edge(a, b);
  }
  for (uint32_t b = 0; b < nb; ++b) {
    std::vector<char> live = lout[b];
    for (uint32_t i = fn.blocks[b].end; i-- > fn.blocks[b].begin;) {
      if (P.dead[i]) continue;
      uint32_t d = is_def(i);
      if (d != NONE) {
        for (uint32_t u = 0; u < nv; ++u)
          if (live[u]) edge(d, u);
        live[d] = 0;
      }
      each_use(i, [&](uint32_t v) { live[v] = 1; });
    }
  }
  // ---- the lib does the coloring ------------------------------------------
  auto forest = gns::make_basic_forest<V>(adj);
  auto colored = gns::color_chordal_graph<V>(forest);
  // ---- assignment: colors 0..4 -> rbx,r12..r15; the rest spill ------------
  std::vector<char> hasloc(nv, 0);
  for (uint32_t v = 0; v < fn.nparams && v < nv; ++v) hasloc[v] = 1;
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
      if (is_def(i) != NONE) hasloc[fn.code[i].dst] = 1;
  for (uint32_t b = 0; b < nb; ++b) // defensive: used-but-undefined
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
      each_use(i, [&](uint32_t v) { hasloc[v] = 1; });
  uint32_t spills = 0;
  for (uint32_t v = 0; v < nv; ++v) {
    if (!hasloc[v]) continue;
    uint64_t c = (v < colored.first.size()) ? (uint64_t)colored.first[v] : 0;
    if (c < 5) {
      R.color[v] = (int32_t)c;
      R.usedmask |= (uint32_t)1 << c;
    } else {
      R.color[v] = 5;
      R.spill[v] = spills++;
    }
  }
  R.nspill = spills;
  return R;
}

static std::string emit_x64_fn(const Fn &fn, uint32_t fidx, bool is_main,
                              bool multi) {
  EmitPlan P = make_emit_plan(fn);
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  char buf[192];
  std::string s{};
  auto fits32 = [](val_t c) {
    return c >= INT64_C(-2147483648) && c <= INT64_C(2147483647);
  };
  RaPlan RP = ra_plan(fn, P);
  static const char *const RREGS[5] = {"rbx", "r12", "r13", "r14", "r15"};
  auto hascolor = [&](uint32_t v) -> bool {
    return v != NONE && v < nv && RP.color[v] >= 0 && RP.color[v] < 5;
  };
  auto rname = [&](uint32_t v) -> const char * {
    return RREGS[RP.color[v]];
  };
  auto slot = [&](uint32_t v) -> std::string { // spill home of v
    std::snprintf(buf, sizeof buf, "-%u(%%rbp)",
                  (unsigned)(8 * (RP.spill[v] + 1)));
    return std::string(buf);
  };
  auto loc = [&](uint32_t v) -> std::string { // operand text: %reg or slot
    if (hascolor(v)) return std::string("%") + rname(v);
    return slot(v);
  };
  auto is_c = [&](uint32_t v) -> bool {
    return v != NONE && v < nv && P.kc[v];
  };
  auto cv = [&](uint32_t v) -> val_t { return P.kv[v]; };
  // load an operand into a SCRATCH register (rax/rcx/rdx/rsi/rdi/r8/r9)
  auto ld = [&](const char *reg, uint32_t v) {
    if (is_c(v)) {
      if (fits32(cv(v)))
        std::snprintf(buf, sizeof buf, "  movq $%lld, %%%s\n",
                      (long long)cv(v), reg);
      else
        std::snprintf(buf, sizeof buf, "  movabsq $%lld, %%%s\n",
                      (long long)cv(v), reg);
    } else if (hascolor(v)) {
      std::snprintf(buf, sizeof buf, "  movq %%%s, %%%s\n", rname(v), reg);
    } else {
      std::snprintf(buf, sizeof buf, "  movq %s, %%%s\n", slot(v).c_str(),
                    reg);
    }
    s += buf;
  };
  // store %rax into a destination (register in place, or spill slot)
  auto st_rax = [&](uint32_t v) {
    if (v == NONE || v >= nv) return;
    if (hascolor(v)) {
      std::snprintf(buf, sizeof buf, "  movq %%rax, %%%s\n", rname(v));
      s += buf;
    } else {
      s += "  movq %rax, " + slot(v) + "\n";
    }
  };
  // move operand x into dst's register (no-op when already there)
  auto src_into = [&](const char *dr, uint32_t x) {
    if (is_c(x)) {
      if (fits32(cv(x))) {
        std::snprintf(buf, sizeof buf, "  movq $%lld, %%%s\n",
                      (long long)cv(x), dr);
        s += buf;
      } else {
        std::snprintf(buf, sizeof buf, "  movabsq $%lld, %%rax\n",
                      (long long)cv(x));
        s += buf;
        std::snprintf(buf, sizeof buf, "  movq %%rax, %%%s\n", dr);
        s += buf;
      }
    } else if (hascolor(x) && !std::strcmp(rname(x), dr)) {
      // already in place: no code
    } else {
      std::snprintf(buf, sizeof buf, "  movq %s, %%%s\n", loc(x).c_str(),
                    dr);
      s += buf;
    }
  };
  auto jcc_of = [](B b) -> const char * {
    switch (b) {
    case B::Lt: return "jl";
    case B::Le: return "jle";
    case B::Gt: return "jg";
    case B::Ge: return "jge";
    case B::Eq: return "je";
    default: return "jne";
    }
  };
  auto jcc_inv = [](const char *j) -> const char * {
    if (!std::strcmp(j, "jl")) return "jge";
    if (!std::strcmp(j, "jle")) return "jg";
    if (!std::strcmp(j, "jg")) return "jle";
    if (!std::strcmp(j, "jge")) return "jl";
    if (!std::strcmp(j, "je")) return "jne";
    return "je";
  };
  // frame: spill bytes, 16-aligned, padded so calls stay aligned given the
  // (possibly odd) number of pushed callee-saved registers
  const uint32_t npush = (uint32_t)__builtin_popcount(RP.usedmask);
  uint64_t frame = ((uint64_t)RP.nspill * 8 + 15) / 16 * 16;
  if (npush & 1) frame += 8;
  // block labels are file-scope in asm: prefix per function
  auto lbl = [&](uint32_t b) -> std::string {
    char l[48];
    if (is_main) std::snprintf(l, sizeof l, ".Lb%u", (unsigned)b);
    else std::snprintf(l, sizeof l, ".LF%u_%u", (unsigned)fidx, (unsigned)b);
    return std::string(l);
  };
  if (is_main) {
    s += "  .globl main\n  .type main, @function\nmain:\n";
  } else {
    s += "mjz_f" + std::to_string(fidx) + ":\n";
  }
  s += "  pushq %rbp\n  movq %rsp, %rbp\n";
  for (uint32_t c = 0; c < 5; ++c)
    if (RP.usedmask & ((uint32_t)1 << c))
      s += std::string("  pushq %") + RREGS[c] + "\n";
  if (frame) {
    std::snprintf(buf, sizeof buf, "  subq $%llu, %%rsp\n",
                  (unsigned long long)frame);
    s += buf;
  }
  if (!is_main) { // recursion depth guard (matches the VM/interpreters)
    s += "  incq mjz_depth(%rip)\n";
    s += "  cmpq $512, mjz_depth(%rip)\n  jg mjz_trap\n";
    static const char *argregs[6] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
    for (uint32_t p = 0; p < fn.nparams && p < 6; ++p)
      s += std::string("  movq %") + argregs[p] + ", " + loc(p) + "\n";
  }
  // NOTE: no slot zero-init — SSA dominance guarantees every read of a slot
  // is preceded (on its path) by the write of its def; undef phi args were
  // materialized as explicit `Const 0` copies during SSA destruction.
  // referenced labels (fallthrough-only blocks need none)
  std::vector<char> refd(fn.nblocks(), 0);
  for (uint32_t b : P.order)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (P.dead[i]) continue;
      if (I.k == K::Br && I.t != P.next[b]) refd[I.t] = 1;
      if (I.k == K::Cbr) {
        if (P.fuse[i] == NONE && I.a < nv && P.kc[I.a]) {
          uint32_t tgt = (P.kv[I.a] != 0) ? I.t : I.f; // constant condition
          if (tgt != P.next[b]) refd[tgt] = 1;
        } else if (I.f == P.next[b]) {
          refd[I.t] = 1; // `jcc .Lb_t` false arm falls through
        } else if (I.t == P.next[b]) {
          refd[I.f] = 1; // inverted `jcc .Lb_f` true arm falls through
        } else {
          refd[I.t] = 1;
          refd[I.f] = 1;
        }
      }
    }
  bool saw_exit = false;
  for (uint32_t b : P.order) {
    if (refd[b]) s += lbl(b) + ":\n";
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (P.dead[i]) continue;
      switch (I.k) {
      case K::Phi:
        break; // destroyed input has none
      case K::Const: { // multi-def slot: a real write of THIS constant
        if (hascolor(I.dst)) {
          if (fits32(I.c)) {
            std::snprintf(buf, sizeof buf, "  movq $%lld, %%%s\n",
                          (long long)I.c, rname(I.dst));
            s += buf;
          } else {
            std::snprintf(buf, sizeof buf, "  movabsq $%lld, %%rax\n",
                          (long long)I.c);
            s += buf;
            std::snprintf(buf, sizeof buf, "  movq %%rax, %%%s\n",
                          rname(I.dst));
            s += buf;
          }
        } else {
          if (fits32(I.c))
            s += "  movq $" + std::to_string((long long)I.c) + ", " +
                 slot(I.dst) + "\n";
          else {
            std::snprintf(buf, sizeof buf, "  movabsq $%lld, %%rax\n",
                          (long long)I.c);
            s += buf;
            s += "  movq %rax, " + slot(I.dst) + "\n";
          }
        }
        break;
      }
      case K::Copy:
        if (hascolor(I.dst)) {
          src_into(rname(I.dst), I.a);
        } else if (is_c(I.a)) {
          if (fits32(cv(I.a)))
            s += "  movq $" + std::to_string((long long)cv(I.a)) + ", " +
                 slot(I.dst) + "\n";
          else {
            std::snprintf(buf, sizeof buf, "  movabsq $%lld, %%rax\n",
                          (long long)cv(I.a));
            s += buf;
            s += "  movq %rax, " + slot(I.dst) + "\n";
          }
        } else {
          s += "  movq " + loc(I.a) + ", %rax\n";
          s += "  movq %rax, " + slot(I.dst) + "\n";
        }
        break;
      case K::Bin: {
        // normalize: commutative ops put the immediate (if any) on the right
        uint32_t x = I.a, y = I.b2;
        if (is_c(x) && !is_c(y) &&
            (I.b == B::Add || I.b == B::Mul || I.b == B::Eq || I.b == B::Ne))
          std::swap(x, y);
        bool yimm = is_c(y);
        const bool inreg_dst = hascolor(I.dst);
        const char *dr = inreg_dst ? rname(I.dst) : "rax";
        switch (I.b) {
        case B::Add:
        case B::Sub:
        case B::Mul:
        case B::And:
        case B::Or:
        case B::Xor: {
          const char *op = I.b == B::Add    ? "addq"
                           : I.b == B::Sub  ? "subq"
                           : I.b == B::Mul  ? "imulq"
                           : I.b == B::And  ? "andq"
                           : I.b == B::Or   ? "orq"
                                            : "xorq";
          const bool comm = (I.b != B::Sub);
          if (!yimm && hascolor(y) && !std::strcmp(rname(y), dr)) {
            // TWO-ADDRESS HAZARD: y is coalesced into the destination
            // register (legal: y dies here). Moving x into dr first would
            // destroy y before the op reads it.
            if (comm) { // dst = x OP y == y OP x: accumulate onto y in place
              std::snprintf(buf, sizeof buf, "  %s %s, %%%s\n", op,
                            loc(x).c_str(), dr);
              s += buf;
            } else { // Sub: stash y, move x in, subtract the stash
              std::snprintf(buf, sizeof buf, "  movq %%%s, %%rcx\n", dr);
              s += buf;
              src_into(dr, x);
              s += "  subq %rcx, %" + std::string(dr) + "\n";
            }
            break;
          }
          src_into(dr, x); // dst register holds x (mov elided when equal)
          if (yimm && fits32(cv(y))) {
            std::snprintf(buf, sizeof buf, "  %s $%lld, %%%s\n", op,
                          (long long)cv(y), dr);
            s += buf;
          } else if (yimm) {
            ld("rcx", y);
            std::snprintf(buf, sizeof buf, "  %s %%rcx, %%%s\n", op, dr);
            s += buf;
          } else {
            std::snprintf(buf, sizeof buf, "  %s %s, %%%s\n", op,
                          loc(y).c_str(), dr);
            s += buf;
          }
          break;
        }
        case B::Shl:
        case B::LShr:
        case B::AShr: {
          // x86 masks shift counts to 6 bits itself -- same semantics as the
          // &63 the other runners apply, so const and variable counts agree
          const char *op = I.b == B::Shl    ? "salq"
                           : I.b == B::LShr ? "shrq"
                                            : "sarq";
          src_into(dr, x);
          if (yimm) {
            std::snprintf(buf, sizeof buf, "  %s $%llu, %%%s\n", op,
                          (unsigned long long)((uint64_t)cv(y) & 63), dr);
            s += buf;
          } else {
            ld("rcx", y);
            std::snprintf(buf, sizeof buf, "  %s %%cl, %%%s\n", op, dr);
            s += buf;
          }
          break;
        }
        case B::MulHi: { // unsigned high half: rdx:rax = rax * rcx
          ld("rax", x);
          ld("rcx", y);
          s += "  mulq %rcx\n";
          if (inreg_dst) {
            std::snprintf(buf, sizeof buf, "  movq %%rdx, %%%s\n", dr);
            s += buf;
          } else {
            s += "  movq %rdx, %rax\n";
          }
          break;
        }
        case B::Div:
        case B::Mod: {
          bool bc = yimm;            // divisor known?
          val_t bv = bc ? cv(y) : 0;
          bool ac = is_c(x);         // dividend known?
          val_t av = ac ? cv(x) : 0;
          bool always = bc && (bv == 0 || (bv == -1 && ac && av == INT64_MIN));
          if (always) {
            s += "  jmp mjz_trap\n";
            break;
          }
          if (bc && bv == -1) { // x / -1 == -x (trap only on INT64_MIN)
            src_into("rax", x);
            s += "  movabsq $0x8000000000000000, %rdx\n";
            s += "  cmpq %rdx, %rax\n  je mjz_trap\n";
            s += I.b == B::Div ? "  negq %rax\n" : "  xorl %eax, %eax\n";
            break;
          }
          src_into("rax", x);
          if (bc) { // known safe divisor: no guard at all
            ld("rcx", y);
          } else { // unknown divisor: full guard, then divide
            ld("rcx", y);
            s += "  testq %rcx, %rcx\n  jz mjz_trap\n";
            s += "  movabsq $0x8000000000000000, %rdx\n";
            s += "  cmpq %rdx, %rax\n  jne 1f\n";
            s += "  cmpq $-1, %rcx\n  je mjz_trap\n";
            s += "1:\n";
          }
          s += "  cqto\n  idivq %rcx\n";
          if (I.b == B::Mod) s += "  movq %rdx, %rax\n";
          st_rax(I.dst); // register in place, spill slot otherwise
          break;
        }
        default: { // comparisons
          src_into("rax", x);
          if (yimm && fits32(cv(y))) {
            std::snprintf(buf, sizeof buf, "  cmpq $%lld, %%rax\n",
                          (long long)cv(y));
            s += buf;
          } else {
            ld("rcx", y);
            s += "  cmpq %rcx, %rax\n";
          }
          const char *set = I.b == B::Lt   ? "setl"
                            : I.b == B::Le ? "setle"
                            : I.b == B::Gt ? "setg"
                            : I.b == B::Ge ? "setge"
                            : I.b == B::Eq ? "sete"
                                           : "setne";
          if (inreg_dst) {
            std::snprintf(buf, sizeof buf, "  %s %%dl\n  movzbq %%dl, %%%s\n",
                          set, dr);
            s += buf;
          } else {
            std::snprintf(buf, sizeof buf,
                          "  %s %%dl\n  movzbq %%dl, %%rax\n", set);
            s += buf;
          }
          break;
        }
        }
        if (!inreg_dst && I.b != B::Div && I.b != B::Mod)
          s += "  movq %rax, " + slot(I.dst) + "\n";
        break;
      }
      case K::Br:
        if (I.t != P.next[b]) // else: fall through
          s += "  jmp " + lbl(I.t) + "\n";
        break;
      case K::Cbr: {
        // emit a compare + conditional jump (fused when possible)
        auto emit_cmp = [&]() -> B {
          uint32_t x = I.a, y = I.b2;
          B op = B::Ne; // plain truth test: jne = jump when cond != 0
          if (P.fuse[i] != NONE) {
            const Instr &D = fn.code[P.fuse[i]];
            x = D.a;
            y = D.b2;
            op = D.b;
            if (is_c(x) && !is_c(y) && (op == B::Eq || op == B::Ne))
              std::swap(x, y); // commutative: immediate on the right
          }
          if (P.fuse[i] == NONE) { // truth test on the condition value
            if (hascolor(x)) {
              std::snprintf(buf, sizeof buf, "  testq %%%s, %%%s\n",
                            rname(x), rname(x));
              s += buf;
            } else {
              ld("rax", x);
              s += "  testq %rax, %rax\n";
            }
            return op;
          }
          if (hascolor(x)) { // compare in the operand's own register
            if (is_c(y) && fits32(cv(y))) {
              std::snprintf(buf, sizeof buf, "  cmpq $%lld, %%%s\n",
                            (long long)cv(y), rname(x));
              s += buf;
            } else if (is_c(y)) {
              ld("rcx", y);
              std::snprintf(buf, sizeof buf, "  cmpq %%rcx, %%%s\n",
                            rname(x));
              s += buf;
            } else {
              std::snprintf(buf, sizeof buf, "  cmpq %s, %%%s\n",
                            loc(y).c_str(), rname(x));
              s += buf;
            }
            return op;
          }
          ld("rax", x);
          if (is_c(y) && fits32(cv(y))) {
            std::snprintf(buf, sizeof buf, "  cmpq $%lld, %%rax\n",
                          (long long)cv(y));
            s += buf;
          } else {
            ld("rcx", y);
            s += "  cmpq %rcx, %rax\n";
          }
          return op;
        };
        if (P.fuse[i] == NONE && is_c(I.a)) { // constant condition
          uint32_t tgt = (cv(I.a) != 0) ? I.t : I.f;
          if (tgt != P.next[b]) s += "  jmp " + lbl(tgt) + "\n";
          break;
        }
        B op = emit_cmp();
        const char *jcc = jcc_of(op);
        if (I.f == P.next[b]) // false arm falls through
          s += std::string("  ") + jcc + " " + lbl(I.t) + "\n";
        else if (I.t == P.next[b]) // true arm falls through: invert
          s += std::string("  ") + jcc_inv(jcc) + " " + lbl(I.f) + "\n";
        else {
          s += std::string("  ") + jcc + " " + lbl(I.t) + "\n";
          s += "  jmp " + lbl(I.f) + "\n";
        }
        break;
      }
      case K::Print:
        ld("rsi", I.a);
        s += "  leaq .Lfmt(%rip), %rdi\n";
        s += "  xorl %eax, %eax\n  call printf\n";
        break;
      case K::Alloc:
        ld("rdi", I.a);
        s += "  call mjz_alloc\n";
        st_rax(I.dst);
        break;
      case K::Load:
        ld("rdi", I.a);
        ld("rsi", I.b2);
        s += "  call mjz_load\n";
        st_rax(I.dst);
        break;
      case K::Store:
        ld("rdi", I.a);
        ld("rsi", I.b2);
        ld("rdx", I.c2);
        s += "  call mjz_store\n";
        break;
      case K::Len:
        ld("rdi", I.a);
        s += "  call mjz_len\n";
        st_rax(I.dst);
        break;
      case K::Input:
        s += "  call mjz_input\n";
        st_rax(I.dst);
        break;
      case K::Depth: // frames incl. main = mjz_depth + 1 (1 in main)
        if (is_main) {
          s += "  movq $1, %rax\n";
        } else {
          s += "  movq mjz_depth(%rip), %rax\n  incq %rax\n";
        }
        st_rax(I.dst);
        break;
      case K::Call: { // SysV: up to six integer registers
        static const char *argregs[6] = {"rdi", "rsi", "rdx",
                                         "rcx", "r8", "r9"};
        for (size_t k = 0; k < I.args.size() && k < 6; ++k)
          ld(argregs[k], I.args[k]);
        s += "  call mjz_f" + std::to_string((unsigned)I.c) + "\n";
        st_rax(I.dst);
        break;
      }
      case K::Ret:
        if (I.a != NONE) ld("rax", I.a);
        else s += "  xorl %eax, %eax\n";
        if (multi) s += "  decq mjz_depth(%rip)\n";
        if (frame) {
          std::snprintf(buf, sizeof buf, "  addq $%llu, %%rsp\n",
                        (unsigned long long)frame);
          s += buf;
        }
        for (int32_t c = 4; c >= 0; --c)
          if (RP.usedmask & ((uint32_t)1 << c))
            s += std::string("  popq %") + RREGS[c] + "\n";
        s += "  popq %rbp\n  ret\n";
        break;
      case K::Exit:
        saw_exit = true;
        s += "  xorl %edi, %edi\n  call exit\n";
        break;
      }
    }
  }
  if (!saw_exit) s += "  xorl %edi, %edi\n  call exit\n";
  return s;
}

// whole-program asm: callee bodies, main, shared trap path + helpers + data.
static std::string emit_x64(const std::vector<Fn> &fns,
                            const std::vector<std::string_view> &names) {
  (void)names;
  std::string s{};
  char buf[160];
  (void)buf;
  // helpers/arena are emitted only when the program uses the feature
  bool use_arr = false, use_in = false;
  for (const Fn &fn : fns)
    for (const Instr &I : fn.code) {
      if (I.k == K::Alloc || I.k == K::Load || I.k == K::Store ||
          I.k == K::Len)
        use_arr = true;
      else if (I.k == K::Input)
        use_in = true;
    }
  const bool multi = fns.size() > 1;
  s += "# generated by minijz\n  .text\n";
  for (uint32_t f = 1; f < fns.size(); ++f) // callees first, main last
    s += emit_x64_fn(fns[f], f, false, multi);
  s += emit_x64_fn(fns[0], 0, true, multi);
  // shared trap path — self-aligning, so it is correct whether reached via
  // `call` (helpers) or via `jz`/`jmp` (main's div/mod and OOB checks)
  s += "mjz_trap:\n  pushq %rbp\n  movq %rsp, %rbp\n  andq $-16, %rsp\n";
  s += "  leaq .Ltraps(%rip), %rdi\n  call puts\n";
  s += "  movl $1, %edi\n  call exit\n";
  if (use_arr) {
    // arena helpers; the bump state must mirror the C/interp backends
    // exactly (CAP = 2^21 slots; handles are element indices)
    s += "# int64 mjz_alloc(int64 n): bump-allocate n slots, return handle\n";
    s += "mjz_alloc:\n";
    s += "  testq %rdi, %rdi\n  js mjz_trap\n";        // n < 0
    s += "  movabsq $2097152, %rdx\n  cmpq %rdx, %rdi\n  jg mjz_trap\n";
    s += "  subq %rdi, %rdx\n";                        // CAP - n (no ovf)
    s += "  movq mjz_atop(%rip), %rax\n";              // h = top
    s += "  cmpq %rdx, %rax\n  jg mjz_trap\n";        // top > CAP - n
    s += "  leaq mjz_alen(%rip), %rdx\n";
    s += "  movq %rdi, (%rdx,%rax,8)\n";               // len[h] = n
    s += "  addq %rax, %rdi\n  movq %rdi, mjz_atop(%rip)\n"; // top = h+n
    s += "  ret\n";
    s += "# int64 mjz_len(int64 h): 0 for any non-handle\n";
    s += "mjz_len:\n";
    s += "  testq %rdi, %rdi\n  js .Llen0\n";
    s += "  movabsq $2097152, %rdx\n  cmpq %rdx, %rdi\n  jg .Llen0\n";
    s += "  leaq mjz_alen(%rip), %rdx\n  movq (%rdx,%rdi,8), %rax\n  ret\n";
    s += ".Llen0:\n  xorl %eax, %eax\n  ret\n";
    s += "# int64 mjz_load(int64 h, int64 i): TRAPs unless 0 <= i < len[h]\n";
    s += "mjz_load:\n";
    s += "  testq %rdi, %rdi\n  js mjz_trap\n";
    s += "  movabsq $2097152, %rdx\n  cmpq %rdx, %rdi\n  jg mjz_trap\n";
    s += "  testq %rsi, %rsi\n  js mjz_trap\n";
    s += "  leaq mjz_alen(%rip), %rdx\n  movq (%rdx,%rdi,8), %rcx\n";
    s += "  cmpq %rcx, %rsi\n  jge mjz_trap\n";
    s += "  leaq mjz_arena(%rip), %rdx\n  addq %rdi, %rsi\n";
    s += "  movq (%rdx,%rsi,8), %rax\n  ret\n";
    s += "# void mjz_store(int64 h, int64 i, int64 v)\n";
    s += "mjz_store:\n";
    s += "  testq %rdi, %rdi\n  js mjz_trap\n";
    s += "  movabsq $2097152, %r8\n  cmpq %r8, %rdi\n  jg mjz_trap\n";
    s += "  testq %rsi, %rsi\n  js mjz_trap\n";
    s += "  leaq mjz_alen(%rip), %r9\n  movq (%r9,%rdi,8), %rcx\n";
    s += "  cmpq %rcx, %rsi\n  jge mjz_trap\n";
    s += "  leaq mjz_arena(%rip), %r9\n  addq %rdi, %rsi\n";
    s += "  movq %rdx, (%r9,%rsi,8)\n  ret\n";
  }
  if (use_in) {
    s += "# int64 mjz_input(void): scanf into stack scratch; 0 on EOF/failure\n";
    s += "mjz_input:\n";
    s += "  pushq %rbx\n  subq $16, %rsp\n";
    s += "  leaq .Lscan(%rip), %rdi\n";
    s += "  movq $0, (%rsp)\n  movq %rsp, %rsi\n";
    s += "  xorl %eax, %eax\n  call scanf\n";
    s += "  movq (%rsp), %rax\n";
    s += "  addq $16, %rsp\n  popq %rbx\n  ret\n";
  }
  s += "  .section .rodata\n";
  s += ".Lfmt:\n  .string \"%lld\\n\"\n";
  s += ".Ltraps:\n  .string \"TRAP\"\n";
  if (use_in) s += ".Lscan:\n  .string \"%lld\"\n";
  if (use_arr) { // (CAP + 1) * 8 bytes each; handles may equal CAP
    s += "  .section .bss\n  .balign 32\n";
    s += "mjz_arena:\n  .zero 16777224\n";
    s += "mjz_alen:\n  .zero 16777224\n";
    s += "mjz_atop:\n  .zero 8\n";
  }
  if (multi) { // recursion depth counter (shared by every non-main fn)
    s += "  .section .bss\n  .balign 8\nmjz_depth:\n  .zero 8\n";
  }
  return s;
}

// ---- 13.5c. bytecode listing (disassembly) -----------------------------------
static std::string emit_bc_listing(const std::vector<uint8_t> &c) {
  std::string s{};
  char buf[160];
  if (c.size() < 4) return s;
  const uint32_t nfns = bc_rd_u32(c, 0);
  std::snprintf(buf, sizeof buf, "; %zu bytes, %u fn(s)\n",
                (size_t)c.size(), (unsigned)nfns);
  s += buf;
  for (uint32_t f = 0; f < nfns && c.size() >= 4 + (size_t)8 * (f + 1); ++f) {
    uint32_t fo = bc_rd_u32(c, 4 + (size_t)8 * f);
    uint32_t sl = bc_rd_u32(c, 4 + (size_t)8 * f + 4);
    uint32_t eo = (f + 1 < nfns) ? bc_rd_u32(c, 4 + (size_t)8 * (f + 1))
                                 : (uint32_t)c.size();
    std::snprintf(buf, sizeof buf, "; fn %u: off=%u slots=%u\n", f, fo, sl);
    s += buf;
    size_t pc = fo;
    while (pc < eo && pc < c.size()) {
      Bc op = (Bc)c[pc];
      std::snprintf(buf, sizeof buf, "%6zu  %-6s", pc, bc_name(op));
      s += buf;
      ++pc;
      switch (op) {
      case Bc::Push:
        std::snprintf(buf, sizeof buf, " %lld\n",
                      (long long)bc_rd_i64(c, pc));
        s += buf;
        pc += 8;
        break;
      case Bc::Load:
      case Bc::Store:
        std::snprintf(buf, sizeof buf, " v%u\n",
                      (unsigned)bc_rd_u32(c, pc));
        s += buf;
        pc += 4;
        break;
      case Bc::Jmp:
      case Bc::Jz:
        std::snprintf(buf, sizeof buf, " %u\n",
                      (unsigned)bc_rd_u32(c, pc));
        s += buf;
        pc += 4;
        break;
      case Bc::Call:
        std::snprintf(buf, sizeof buf, " f%u (%u args)\n",
                      (unsigned)bc_rd_u32(c, pc),
                      (unsigned)bc_rd_u32(c, pc + 4));
        s += buf;
        pc += 8;
        break;
      default: s += "\n"; break;
      }
    }
  }
  return s;
}

// ---- 13.5d. native build & run harness ----------------------------------------
static bool write_file(const std::string &path, const std::string &data) {
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  f << data;
  return bool(f);
}
static bool write_file_bytes(const std::string &path,
                             const std::vector<uint8_t> &data) {
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  f.write((const char *)data.data(), (std::streamsize)data.size());
  return bool(f);
}
static std::string slurp_file(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return std::string((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
}
static std::string find_cc() {
  auto has = [](const char *cc) {
    std::string cmd = std::string("command -v ") + cc + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
  };
  if (has("clang++")) return "clang++"; // preferred: user-requested driver
  if (has("g++")) return "g++";
  if (has("cc")) return "cc";
  return "";
}
// parse whitespace-separated integers; stop at the first non-number (the
// same place scanf in the native builds gives up, so the tape stays in sync)
static std::vector<val_t> parse_inputs(const std::string &text) {
  std::vector<val_t> out{};
  const char *p = text.c_str();
  char *end{};
  for (;;) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (!*p) break;
    long long v = std::strtoll(p, &end, 10);
    if (end == p) break;
    out.push_back((val_t)v);
    p = end;
  }
  return out;
}
// run `cmd`, capturing stdout into `out`; stdin redirected from `in_path`
// (empty = inherit); returns the raw system() status
static int run_capture(const std::string &cmd, std::string &out,
                       const std::string &cap_path,
                       const std::string &in_path = {}) {
  std::string full = cmd;
  if (!in_path.empty()) full += " < " + in_path;
  full += " > " + cap_path + " 2>/dev/null";
  int rc = std::system(full.c_str());
  out = slurp_file(cap_path);
  return rc;
}
static double now_ms() {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// ============================================================================

#pragma once

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
      if (P.dead[i]) continue;
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
          // a trap must abort the WHOLE program. `return 1` only does that
          // from main; inside a callee it would hand control back to the
          // caller, which would keep running past the trap (the arena and
          // stack-array guards below already use exit(1) -- these three
          // div/mod sites must too)
          const std::string trap_stmt =
              is_main ? "return 1" : "exit(1)";
          if (always) {
            s += "  printf(\"TRAP\\n\"); " + trap_stmt + ";\n";
            break;
          }
          if (I.nt) {
            // PROVEN trap-free (range/alias analysis): no guard at all --
            // this is the `h != 0` loop-divisor case, where the guard is
            // dead weight the analysis has already discharged
          } else if (bc && bv == -1)
            s += "  if (" + V(I.a) + " == INT64_MIN) { printf(\"TRAP\\n\"); " +
                 trap_stmt + "; }\n";
          else if (!bc)
            s += "  if (" + V(I.b2) + " == 0 || (" + V(I.a) +
                 " == INT64_MIN && " + V(I.b2) +
                 " == -1)) { printf(\"TRAP\\n\"); " + trap_stmt + "; }\n";
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
          std::string pos =
              "(" + V(D.a) + " " + cmp_txt(D.b) + " " + V(D.b2) + ")";
          std::string neg =
              "(" + V(D.a) + " " + neg_txt(D.b) + " " + V(D.b2) + ")";
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
        break;      case K::Load:
        if (I.a < nv && arr_of[I.a] >= 0) { // local array: direct + bounds check
          if (!I.nt) { // nt: index PROVEN in range (alias analysis)
            std::snprintf(buf, sizeof buf,
                          "  if (%s < 0 || %s >= %lld) { printf(\"TRAP\\n\"); "
                          "exit(1); }\n",
                          V(I.b2).c_str(), V(I.b2).c_str(),
                          (long long)fn.stk_arrays[arr_of[I.a]].len);
            s += buf;
          }
          std::snprintf(buf, sizeof buf, "  %s = %s[%s];\n", D(I.dst).c_str(),
                        arr_name(arr_of[I.a]).c_str(), V(I.b2).c_str());
          s += buf;
        } else if (I.nt) { // arena, PROVEN in bounds: direct access
          std::snprintf(buf, sizeof buf, "  %s = mjz_arena[%s + %s];\n",
                        D(I.dst).c_str(), V(I.a).c_str(), V(I.b2).c_str());
          s += buf;
        } else {
          std::snprintf(buf, sizeof buf, "  %s = mjz_load(%s, %s);\n",
                        D(I.dst).c_str(), V(I.a).c_str(), V(I.b2).c_str());
          s += buf;
        }
        break;      case K::Store:
        if (I.a < nv && arr_of[I.a] >= 0) { // local array: direct + bounds check
          if (!I.nt) { // nt: index PROVEN in range (alias analysis)
            std::snprintf(buf, sizeof buf,
                          "  if (%s < 0 || %s >= %lld) { printf(\"TRAP\\n\"); "
                          "exit(1); }\n",
                          V(I.b2).c_str(), V(I.b2).c_str(),
                          (long long)fn.stk_arrays[arr_of[I.a]].len);
            s += buf;
          }
          std::snprintf(buf, sizeof buf, "  %s[%s] = %s;\n",
                        arr_name(arr_of[I.a]).c_str(), V(I.b2).c_str(),
                        V(I.c2).c_str());
          s += buf;
        } else if (I.nt) { // arena, PROVEN in bounds: direct access
          std::snprintf(buf, sizeof buf, "  mjz_arena[%s + %s] = %s;\n",
                        V(I.a).c_str(), V(I.b2).c_str(), V(I.c2).c_str());
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
  if (use_arr || multi || use_stk) s += "#include <stdlib.h>\n"; // exit()
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

#pragma once
// === emit_c_impl.hpp ========================================
// the C backend codegen body (per-fn emitter + whole-program entry).
// Included by emit_c.hpp in the header-only build (constexpr) and by
// tu/backends.cpp in the multi-TU build (external definition) — include
// it nowhere else.

#include "minijz/ir/instr.hpp"
#include "minijz/back/stack_alloc.hpp"
#include "minijz/back/emit_plan.hpp"

#include <string>
#include <string_view>
#include <vector>

#define MJZ_EMIT_FN // runtime-only (string building): plain in both modes

MJZ_EMIT_FN std::string emit_c_fn(const Fn &fn, uint32_t fidx, bool is_main,
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
MJZ_EMIT_FN std::string emit_c(const std::vector<Fn> &fns,
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

#undef MJZ_EMIT_FN

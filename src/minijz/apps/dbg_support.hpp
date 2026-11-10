#pragma once
// === dbg_support.hpp ========================================
// the --debugger session's SUPPORT layer, split out of debugger.hpp
// (checkpoint 16's "break down the big files"): bytecode disassembly
// pretty printing, the DbgVm machine wrapper, the language-breakpoint
// scan (DbgBk), the slot naming / may-handle provenance over the pre-opt
// SSA, and the pass-explorer snapshot machinery (Snap/SnapColl). No
// session loop, no command dispatch -- that lives in debugger.hpp.
//
// The VM half is built on the steppable VmCtx (see back/vm.hpp): every
// slot vector, the operand stack, the arena, the input position and the
// output tape are member state, so `vm state` shows EVERYTHING and
// `vm step` moves exactly one bytecode op.

#include "cli_parse.hpp"
#include "../prog/program.hpp"
#include "../prog/printers.hpp"
#include "../back/vm.hpp"
#include "../back/bytecode.hpp"
#include "../back/emit_plan.hpp"
#include "../runtime/astrun.hpp"
#include "../opt/optimize_api.hpp"
#include "../front/diag.hpp"

#include <cstdio>
#include <cstring>

// ---- bytecode disassembly helpers (debugger-only pretty printing) ---------
// one op at pc: textual form + the pc just past its operands
static std::string bc_disasm_one(const std::vector<uint8_t> &c, size_t pc,
                                 size_t *next = nullptr) {
  if (pc >= c.size()) {
    if (next)
      *next = pc;
    return "<eof>";
  }
  Bc op = (Bc)c[pc];
  size_t p = pc + 1;
  char buf[160];
  switch (op) {
  case Bc::Push:
    std::snprintf(buf, sizeof buf, "push %lld", (long long)bc_rd_i64(c, p));
    p += 8;
    break;
  case Bc::Load:
    std::snprintf(buf, sizeof buf, "load s%u", bc_rd_u32(c, p));
    p += 4;
    break;
  case Bc::Store:
    std::snprintf(buf, sizeof buf, "store s%u", bc_rd_u32(c, p));
    p += 4;
    break;
  case Bc::Jmp:
  case Bc::Jz:
    std::snprintf(buf, sizeof buf, "%s %u", op == Bc::Jmp ? "jmp" : "jz",
                  bc_rd_u32(c, p));
    p += 4;
    break;
  case Bc::Call:
    std::snprintf(buf, sizeof buf, "call fn%u n%u", bc_rd_u32(c, p),
                  bc_rd_u32(c, p + 4));
    p += 8;
    break;
  case Bc::Bkpt: {
    uint32_t n = bc_rd_u32(c, p);
    p += 4;
    std::string x = "bkpt";
    for (uint32_t k = 0; k < n; ++k, p += 4) {
      if (k >= 8)
        continue; // render at most 8, but ALWAYS advance past the operand
      std::snprintf(buf, sizeof buf, "%ss%u", k ? "," : " ", bc_rd_u32(c, p));
      x += buf;
    }
    if (n > 8)
      x += " ...";
    if (next)
      *next = p;
    return x;
  }
  default:
    std::snprintf(buf, sizeof buf, "%s", bc_name(op));
    break;
  }
  if (next)
    *next = p;
  return std::string(buf);
}

struct DbgVm {
  std::vector<uint8_t> bc{};
  VmCtx S{};
  std::vector<uint32_t> bps{}; // manual breakpoints (pcs)
  bool valid{false};
  size_t resume_pc{(size_t)-1}; // the pc the last stop was at: `continue`
                                // executes it once before re-arming stops
};

// one language breakpoint: a Bc::Bkpt op scanned out of the bytecode at
// startup (they survive optimization — observing a value pins it)
struct DbgBk {
  size_t pc{};
  uint32_t fn{};                    // fn index in the emitted set
  uint32_t src{};                   // source offset (NONE when synthesized)
  std::vector<uint32_t> slots{};    // observed VM slots
  std::vector<std::string> names{}; // observed names, parallel to slots
  std::vector<char> arry{};         // per name: may hold an array handle
};

// one pass snapshot (the `opt` explorer): IR text right after a pass ran
struct Snap {
  std::string name{};
  bool changed{};
  std::string ir{};
  uint32_t instrs{}, blocks{};
};

static void dbg_vm_reset(DbgVm &D, uint64_t limit,
                         const std::vector<val_t> &ins) {
  D.S = VmCtx{};
  (void)vm_init(D.S, D.bc, limit, ins);
}

// the full-state dump: everything the machine knows, on demand
static void dbg_vm_state(DbgVm &D, const Program &P) {
  const VmCtx &S = D.S;
  if (!D.valid) {
    std::printf("(no bytecode — run `vm reset` after a successful compile)\n");
    return;
  }
  std::printf("== VM state ==\n");
  if (!S.started) {
    std::printf("not started\n");
    return;
  }
  std::string fn_name = S.fn_at(S.pc) < P.fn_names.size()
                            ? std::string(P.fn_names[S.fn_at(S.pc)])
                            : "?";
  std::printf("pc=%zu (fn %u '%s') next: %s\n", S.pc, S.fn_at(S.pc),
              fn_name.c_str(), bc_disasm_one(D.bc, S.pc).c_str());
  std::printf("steps=%llu/%llu halted=%d trapped=%d timeout=%d\n",
              (unsigned long long)S.steps, (unsigned long long)S.step_limit,
              (int)S.halted, (int)S.trapped, (int)S.timeout);
  std::printf("frames=%zu:\n", S.frames.size());
  for (size_t f = 0; f < S.frames.size(); ++f) {
    uint32_t fid = f < S.fr_fn.size() ? S.fr_fn[f] : 0;
    // NB: a std::string temporary's c_str() must not outlive the
    // full-expression (the -Wdangling-gsl hit in warnings.txt): keep the
    // string alive until the printf below has consumed it.
    std::string nm = fid < P.fn_names.size() ? std::string(P.fn_names[fid])
                                             : std::string("?");
    std::printf("  #%-2zu fn%-2u (%s) slots(%zu):", f, fid, nm.c_str(),
                S.frames[f].size());
    for (size_t k = 0; k < S.frames[f].size() && k < 24; ++k)
      std::printf(" [%zu]=%lld", k, (long long)S.frames[f][k]);
    if (S.frames[f].size() > 24)
      std::printf(" ...(+%zu)", S.frames[f].size() - 24);
    std::printf("\n");
  }
  std::printf("operand stack (%zu, top last):", S.st.size());
  for (size_t k = S.st.size() > 16 ? S.st.size() - 16 : 0; k < S.st.size(); ++k)
    std::printf(" %lld", (long long)S.st[k]);
  std::printf("\n");
  // arena: contiguous allocations h=0,4,8... per a_lenv entries
  size_t allocs = 0;
  for (val_t L : S.R.a_lenv)
    if (L > 0)
      ++allocs;
  std::printf("arena: %zu allocs, %zu slots:", allocs, S.R.a_data.size());
  val_t h = 0;
  size_t shown = 0;
  for (val_t L : S.R.a_lenv) {
    if (L > 0) {
      std::printf(" h%lld(len %lld)=[", (long long)h, (long long)L);
      for (val_t i = 0; i < L && i < 8; ++i)
        std::printf("%s%lld", i ? " " : "",
                    (long long)S.R.a_data[(size_t)(h + i)]);
      if (L > 8)
        std::printf(" ...+%lld", (long long)(L - 8));
      std::printf("]");
      if (++shown >= 6) {
        std::printf(" ...(+%zu more)", allocs - shown);
        break;
      }
    }
    h += L > 0 ? L : 0;
  }
  std::printf("\n");
  std::printf("input: pos %zu/%zu", S.R.in_pos, S.R.ins.size());
  if (S.R.in_pos < S.R.ins.size())
    std::printf(" (next: %lld)", (long long)S.R.ins[S.R.in_pos]);
  std::printf("\noutput (%zu):", S.R.out.size());
  for (size_t k = 0; k < S.R.out.size() && k < 16; ++k)
    std::printf(" %lld", (long long)S.R.out[k]);
  if (S.R.out.size() > 16)
    std::printf(" ...(+%zu)", S.R.out.size() - 16);
  std::printf("\n");
  if (!D.bps.empty()) {
    std::printf("breakpoints:");
    for (uint32_t b : D.bps)
      std::printf(" %u", b);
    std::printf("\n");
  }
}

static void dbg_vm_dis(DbgVm &D, long before, long after) {
  if (!D.valid) {
    std::printf("(no bytecode)\n");
    return;
  }
  // disassemble from the START of the fn containing pc (op starts stay
  // aligned that way), then show a window of +-before/after bytes around pc
  uint32_t f = D.S.fn_at(D.S.pc);
  size_t p = D.S.fn_off(f);
  size_t hi = D.S.pc + (size_t)after;
  std::vector<std::pair<size_t, std::string>> lines{};
  while (p < D.bc.size() && p <= hi) {
    size_t at = p;
    std::string d = bc_disasm_one(D.bc, p, &p); // p advances past operands
    lines.push_back({at, d});
  }
  size_t lo_pc = D.S.pc > (size_t)before ? D.S.pc - (size_t)before : 0;
  for (const auto &[at, d] : lines)
    if (at >= lo_pc && at <= hi)
      std::printf("%s%6zu: %-18s\n", at == D.S.pc ? "->" : "  ", at, d.c_str());
  std::printf("   (pc=%zu, fn %u)\n", D.S.pc, f);
}

// ---- value rendering --------------------------------------------------------
// A slot value that currently equals a live arena handle shows the array
// ONLY when the observed vreg may really hold one (provenance below): a
// scalar that merely collides with a handle number (0 IS handle 0 when an
// allocation lives at arena offset 0) stays a number.
static std::string dbg_val(const VmCtx &S, val_t v, bool may_handle = true) {
  char buf[96];
  val_t L = may_handle ? S.R.arena_len(v) : 0;
  if (L > 0) {
    std::string s = "arr#" + std::to_string((long long)v) + " len " +
                    std::to_string((long long)L) + " [";
    for (val_t i = 0; i < L && i < 8; ++i)
      s += (i ? " " : "") +
           std::to_string((long long)S.R.a_data[(size_t)(v + i)]);
    if (L > 8)
      s += " ...+" + std::to_string((long long)(L - 8));
    s += "]";
    return s;
  }
  std::snprintf(buf, sizeof buf, "%lld", (long long)v);
  return std::string(buf);
}

// per-vreg "may hold an array handle" provenance over the PRE-OPT SSA fn:
// Alloc results, their copies, phis of those, call results and input()
// values are maybe-handles; arithmetic/len results are scalars. (The
// language is dynamically typed — this is a display heuristic that makes
// the common cases honest; true collisions still show the number.)
static std::vector<char> dbg_may_handle(const Fn &F) {
  const uint32_t nv = F.nvars + (uint32_t)F.version_base.size();
  std::vector<char> mh(nv, 0);
  bool grew = true;
  while (grew) {
    grew = false;
    for (const Instr &I : F.code) {
      char set = 0;
      switch (I.k) {
      case K::Alloc:
      case K::Input:
        set = 1; // a fresh handle / a tape value (could BE a handle)
        break;
      case K::Call:
        set = 1; // the callee may return a handle
        break;
      case K::Copy:
        if (I.a != NONE && I.a < nv && mh[I.a])
          set = 1;
        break;
      case K::Phi:
        if ((I.a != NONE && I.a < nv && mh[I.a]) ||
            (I.b2 != NONE && I.b2 < nv && mh[I.b2]))
          set = 1;
        break;
      case K::Load:
        set = 1; // 2-D arrays: an element may itself be a handle
        break;
      default:
        break; // Const/Bin/Len/Depth: scalar values
      }
      if (set && I.dst != NONE && I.dst < nv && !mh[I.dst]) {
        mh[I.dst] = 1;
        grew = true;
      }
    }
  }
  return mh;
}

// slot id -> variable name for one emitted fn (destroyed + slot_of). Names
// resolve through SSA version bases: any vreg whose slot is `s` and whose
// BASE var has a source name wins (first one); temps keep "".
static std::vector<std::string> dbg_slot_names(const Program &P, size_t fi) {
  if (fi >= P.fns.size())
    return {};
  const Fn &D = P.fns[fi].destroyed;
  const uint32_t nv = D.nvars + (uint32_t)D.version_base.size();
  const uint32_t ns = slot_count(D);
  std::vector<std::string> names(ns, "");
  for (uint32_t v = 0; v < nv; ++v) {
    uint32_t base = D.base_of(v);
    if (base >= D.var_names.size() || D.var_names[base].empty())
      continue; // temp
    uint32_t s = slot_id(D, v);
    if (s < ns && names[s].empty())
      names[s] = std::string(D.var_names[base]);
  }
  return names;
}

// scan the bytecode for language breakpoints, resolving observed slots to
// names. Names come from the PRE-OPT SSA fn (the same statement's Bkpt:
// passes rewrite the observed vregs — copyprop into anonymous temps — but
// never reorder or drop them, so position still matches the source list);
// the slot-name table is the fallback for synthesized stops.
static std::vector<DbgBk> dbg_scan_bkpts(const std::vector<uint8_t> &bc,
                                         const Program &P,
                                         const std::vector<uint32_t> &pcsrc) {
  std::vector<DbgBk> out{};
  if (bc.size() < 4)
    return out;
  const uint32_t nf = bc_rd_u32(bc, 0);
  // per fn: (src, names, maybe-handle) of each pre-opt Bkpt statement, and
  // a slot-level may-handle fold (for the `vars`/watch displays)
  struct NamesMh {
    uint32_t src{};
    std::vector<std::string> names{};
    std::vector<char> arry{};
  };
  std::vector<std::vector<NamesMh>> srcs(nf);
  std::vector<std::vector<char>> slot_mh(nf);
  for (uint32_t f = 0; f < nf && f < P.fns.size(); ++f) {
    const Fn &S = P.fns[f].ssa;
    std::vector<char> mh = dbg_may_handle(S);
    for (const Instr &I : S.code)
      if (I.k == K::Bkpt) {
        NamesMh e{};
        e.src = I.src;
        for (uint32_t v : I.args) {
          uint32_t base = S.base_of(v);
          e.names.push_back(base < S.var_names.size() &&
                                    !S.var_names[base].empty()
                                ? std::string(S.var_names[base])
                                : std::string());
          e.arry.push_back(v < mh.size() ? mh[v] : (char)1);
        }
        srcs[f].push_back(std::move(e));
      }
    const Fn &D = P.fns[f].destroyed;
    const uint32_t nv = D.nvars + (uint32_t)D.version_base.size();
    const uint32_t ns = slot_count(D);
    slot_mh[f].assign(ns, 0);
    for (uint32_t v = 0; v < nv && v < mh.size(); ++v)
      if (mh[v]) {
        uint32_t s = slot_id(D, v);
        if (s < ns)
          slot_mh[f][s] = 1;
      }
  }
  std::vector<std::vector<std::string>> names(nf); // slot-name fallback
  for (uint32_t f = 0; f < nf && f < P.fns.size(); ++f)
    names[f] = dbg_slot_names(P, f);
  size_t pc = 4 + (size_t)8 * nf;
  uint32_t f = 0;
  while (pc < bc.size()) {
    while (f + 1 < nf && pc >= bc_rd_u32(bc, 4 + (size_t)8 * (f + 1)))
      ++f;
    if (bc[pc] == (uint8_t)Bc::Bkpt) {
      DbgBk B{};
      B.pc = pc;
      B.fn = f;
      B.src = pc < pcsrc.size() ? pcsrc[pc] : (uint32_t)NONE;
      uint32_t n = bc_rd_u32(bc, pc + 1);
      // the pre-opt names of THIS statement (matched by source offset;
      // positional fallback for synthesized stops)
      const NamesMh *lst = nullptr;
      if (f < srcs.size())
        for (const NamesMh &e : srcs[f])
          if (e.src != NONE && e.src == B.src)
            lst = &e;
      if (!lst && f < srcs.size() && !srcs[f].empty())
        lst = &srcs[f].front(); // fallback: first statement's list
      for (uint32_t k = 0; k < n; ++k) {
        uint32_t s = bc_rd_u32(bc, pc + 5 + (size_t)4 * k);
        B.slots.push_back(s);
        std::string nm{};
        char mh = 1;
        if (lst && k < lst->names.size()) {
          nm = lst->names[k];
          mh = k < lst->arry.size() ? lst->arry[k] : (char)1;
        }
        if (nm.empty() && f < names.size() && s < names[f].size())
          nm = names[f][s]; // fallback: the slot's own variable name
        if (f < slot_mh.size() && s < slot_mh[f].size() && slot_mh[f][s])
          mh = 1; // the slot itself may hold a handle
        B.names.push_back(std::move(nm));
        B.arry.push_back(mh);
      }
      out.push_back(std::move(B));
      pc += 5 + (size_t)4 * n;
      continue;
    }
    (void)bc_disasm_one(bc, pc, &pc); // advances past operands
  }
  return out;
}

// slot-level may-handle flags for one emitted fn (the `vars` display):
// vreg provenance from the pre-opt SSA, folded through the slot map
static std::vector<char> dbg_slot_may_handle(const Program &P, size_t fi) {
  if (fi >= P.fns.size())
    return {};
  const Fn &S = P.fns[fi].ssa;
  std::vector<char> mh = dbg_may_handle(S);
  const Fn &D = P.fns[fi].destroyed;
  const uint32_t nv = D.nvars + (uint32_t)D.version_base.size();
  const uint32_t ns = slot_count(D);
  std::vector<char> out(ns, 0);
  for (uint32_t v = 0; v < nv && v < mh.size(); ++v)
    if (mh[v]) {
      uint32_t s = slot_id(D, v);
      if (s < ns)
        out[s] = 1;
    }
  return out;
}

// ---- pass snapshots (the `opt` explorer) ------------------------------------
struct SnapColl {
  std::vector<Snap> *out{};
  std::string_view src{};
};
static void dbg_snap_hook(const char *pass, bool changed, const Fn &fn,
                          void *ctx) {
  SnapColl *C = (SnapColl *)ctx;
  Snap s{};
  s.name = pass;
  s.changed = changed;
  char title[64];
  std::snprintf(title, sizeof title, "after %s", pass);
  s.ir = fn_ir_str(fn, title, C->src);
  s.instrs = (uint32_t)fn.code.size();
  s.blocks = fn.nblocks();
  C->out->push_back(std::move(s));
}
// re-run the per-fn pipeline on a fresh copy, collecting a snapshot after
// every pass invocation (baseline first). Returns false when -O0.
static bool dbg_collect_snaps(const Program &P, size_t fi, const Cli &C,
                              std::vector<Snap> &snaps) {
  if (fi >= P.fns.size())
    return false;
  Fn live = P.fns[fi].ssa;
  SnapColl coll{&snaps, P.src};
  Snap base{};
  base.name = "baseline";
  base.changed = false;
  base.ir = fn_ir_str(live, "baseline (pre-opt SSA)", P.src);
  base.instrs = (uint32_t)live.code.size();
  base.blocks = live.nblocks();
  snaps.push_back(std::move(base));
  PassCfg cfg = pass_cfg_of(C);
  cfg.debug = 0; // the explorer prints its own views
  cfg.dbg_name = P.fn_names[fi];
  cfg.snap_hook = dbg_snap_hook;
  cfg.snap_ctx = &coll;
  OptStats os{};
  (void)optimize_light(live, os, cfg);
  return true;
}

static void dbg_print_timeline(const std::vector<Snap> &snaps, size_t fi,
                               const Program &P) {
  std::printf("== pass timeline: fn %zu [%s] — %zu snapshot(s) ==\n", fi,
              std::string(P.fn_names[fi]).c_str(), snaps.size());
  std::printf("  (intra-fn pipeline only: interprocedural passes run "
              "program-wide)\n");
  for (size_t k = 0; k < snaps.size(); ++k) {
    const Snap &s = snaps[k];
    char buf[128];
    std::snprintf(buf, sizeof buf, "  #%-3zu %-10s %-9s %u instrs, %u blocks",
                  k, s.name.c_str(), s.changed ? "changed" : "fixed",
                  (unsigned)s.instrs, (unsigned)s.blocks);
    std::printf("%s", buf);
    if (k > 0 && snaps[k - 1].instrs != s.instrs)
      std::printf("  (%+d)", (int)s.instrs - (int)snaps[k - 1].instrs);
    std::printf("\n");
  }
}

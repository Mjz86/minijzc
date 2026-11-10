#pragma once
// === debugger.hpp ========================================
// --debugger: an interactive, always-answering session over the compiled
// program. No timeouts, no waiting: every command dumps state instantly.
//
//   IR side      ir / cfg / stats / plan / opt   (pipeline inspection)
//   program side vm step/run/until/break/state   (the live machine)
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

static void dbg_help() {
  std::printf(
      "debugger commands (arguments in [] are optional):\n"
      "  help                 this text\n"
      "  src [N]              print the program source (with line numbers;\n"
      "                       N: just that line)\n"
      "  fns                  function table (stages: ssa/opt/dead instrs)\n"
      "  ir [FN] [ssa|opt|dst|all]   dump IR (default: all fns, stage opt)\n"
      "  cfg [FN]             CFG: blocks, preds/succs (main: + dominators)\n"
      "  stats                optimization statistics summary\n"
      "  plan [FN]            emission plan (order, next, dead, kconsts)\n"
      "  bc [FN]              full bytecode listing (or one fn's range)\n"
      "  bkpts                language breakpoints (`breakpoint x;` stmts):\n"
      "                       pc, fn, source line, observed values now\n"
      "  vars [FN]            named slots of the current (or given) fn with\n"
      "                       live values — what survived optimization\n"
      "  watch NAME           add a variable to show at every stop\n"
      "  unwatch NAME|*       remove one watch / all\n"
      "  opt [FN]             pass explorer: re-run the pipeline on a copy,\n"
      "                       snapshot after every pass, print the timeline\n"
      "  opt show [N]         full IR of snapshot N (default: last)\n"
      "  opt diff [A [B]]     unified IR diff between snapshots (names or\n"
      "                       indices; default: first vs last)\n"
      "  opt break PASS       breakpoint BETWEEN passes: show PASS's snapshot\n"
      "                       and its diff against the previous one\n"
      "  opt list             the pass toggle table (on/off for this run)\n"
      "  check                run all five runners and compare\n"
      "  vm reset             re-create the machine (uses --steps, tape)\n"
      "  vm step [N]          execute N bytecode ops (default 1), traced\n"
      "                       with source line annotations\n"
      "  vm run               run to completion (bounded by --steps)\n"
      "  vm continue [N]      run until a breakpoint / halt (max N ops);\n"
      "                       language breakpoints stop here automatically\n"
      "  vm until PC          run until pc == PC (a one-shot breakpoint)\n"
      "  vm break PC          set a breakpoint at bytecode pc\n"
      "  vm line L            set breakpoints at every pc of source line L\n"
      "  vm unbreak PC|*      remove it (vm unbreak * clears all)\n"
      "  vm breaks            list breakpoints\n"
      "  vm state             FULL machine state: pc, frames, slots,\n"
      "                       operand stack, arena, input pos, output\n"
      "  vm dis [N]           disassemble around pc (+/- N bytes)\n"
      "  quit                 leave the debugger\n");
}

int run_debugger(const Cli &C, const std::string &src) {
  Program P = compile(src, pass_cfg_of(C));
  if (!P.ok) {
    print_compile_error(src, P.error, P.errpos, C.file.c_str());
    return 1;
  }
  DbgVm D{};
  std::vector<uint32_t> pcsrc{}; // per-byte source offsets (line bps, steps)
  D.bc = emit_bytecode(fns_of(P, 2), &pcsrc);
  D.valid = true;
  SrcMap SM{P.src};
  std::vector<DbgBk> bkpts = dbg_scan_bkpts(D.bc, P, pcsrc);
  std::vector<std::string> watches{};
  std::vector<Snap> snaps{}; // the `opt` explorer's snapshot stack
  size_t snap_fn = 0;
  dbg_vm_reset(D, C.steps, C.inputs);
  std::printf(
      "=== minijz debugger — %zu fn(s), %zu bytes of bytecode, %llu step "
      "budget ===\n",
      P.fns.size(), D.bc.size(), (unsigned long long)C.steps);
  if (!bkpts.empty())
    std::printf("program has %zu language breakpoint(s): `bkpts` lists them; "
                "`vm continue` stops there\n",
                bkpts.size());
  std::printf("program uses input(): %s (%s); type `help` for commands\n",
              P.uses_input ? "yes" : "no",
              C.stdin_path.empty() ? "no --stdin tape, input() reads 0"
                                   : ("tape: " + C.stdin_path).c_str());

  // ---- little helpers over the session state -------------------------------
  auto line_at = [&](size_t pc) -> uint32_t {
    if (pc >= pcsrc.size() || pcsrc[pc] == NONE)
      return 0;
    return SM.line(pcsrc[pc]);
  };
  auto loc_str = [&](size_t pc) -> std::string {
    uint32_t l = line_at(pc);
    char buf[32];
    if (!l)
      return std::string("?");
    std::snprintf(buf, sizeof buf, "line %u", (unsigned)l);
    return std::string(buf);
  };
  auto bkpt_at = [&](size_t pc) -> const DbgBk * {
    for (const DbgBk &B : bkpts)
      if (B.pc == pc)
        return &B;
    return nullptr;
  };

  // report a stop at a language breakpoint: observed values + watches
  auto report_bkpt = [&](const DbgBk &B) {
    char lb[48];
    const char *loc = SM.at(B.src, lb, sizeof lb) ? lb : "?";
    std::string fnn = B.fn < P.fn_names.size() ? std::string(P.fn_names[B.fn])
                                               : std::string("?");
    std::printf("* breakpoint at %s in fn %u (%s) [pc %zu]\n", loc,
                (unsigned)B.fn, fnn.c_str(), B.pc);
    // the CURRENT frame is B.fn's frame (pc is inside it)
    if (!D.S.frames.empty()) {
      const std::vector<val_t> &fr = D.S.frames.back();
      for (size_t k = 0; k < B.slots.size(); ++k) {
        std::string nm = k < B.names.size() && !B.names[k].empty()
                             ? B.names[k]
                             : ("slot " + std::to_string(B.slots[k]));
        val_t v = B.slots[k] < fr.size() ? fr[B.slots[k]] : (val_t)0;
        std::printf("    %-10s = %s\n", nm.c_str(),
                    dbg_val(D.S, v, k < B.arry.size() && B.arry[k]).c_str());
      }
    }
  };
  auto report_watches = [&]() {
    if (watches.empty() || D.S.frames.empty() || !D.S.started)
      return;
    uint32_t f = D.S.fn_at(D.S.pc);
    std::vector<std::string> names = dbg_slot_names(P, f);
    std::vector<char> mh = dbg_slot_may_handle(P, f);
    const std::vector<val_t> &fr = D.S.frames.back();
    for (const std::string &w : watches) {
      bool found = false;
      for (size_t s = 0; s < names.size() && s < fr.size(); ++s)
        if (names[s] == w) {
          std::printf("    watch %-10s = %s\n", w.c_str(),
                      dbg_val(D.S, fr[s], s < mh.size() && mh[s]).c_str());
          found = true;
          break;
        }
      if (!found)
        std::printf("    watch %-10s = <not live in fn %u>\n", w.c_str(),
                    (unsigned)f);
    }
  };
  // the run/continue stop engine: stops BEFORE executing the op at pc when
  // it is a manual bp, a line bp or a language breakpoint; `resume_pc`
  // exempts the pc we stopped at for exactly one op (classic `continue`)
  auto run_bounded = [&](uint64_t maxops) {
    uint64_t ops = 0;
    auto stop_at = [&](size_t pc) -> int { // 0 none, 1 manual, 2 language
      if (pc >= D.bc.size())
        return 0;
      for (uint32_t bp : D.bps)
        if (pc == bp)
          return 1;
      if (D.bc[pc] == (uint8_t)Bc::Bkpt)
        return 2;
      return 0;
    };
    if (D.S.pc == D.resume_pc)
      D.resume_pc = (size_t)-1; // resuming from THIS stop: run the op once
    else {
      int st = stop_at(D.S.pc);
      if (st == 2) {
        report_bkpt(*bkpt_at(D.S.pc));
        report_watches();
        D.resume_pc = D.S.pc;
        return;
      }
      if (st == 1) {
        std::printf("breakpoint %zu hit (pc=%zu, %s)\n", D.S.pc, D.S.pc,
                    loc_str(D.S.pc).c_str());
        report_watches();
        D.resume_pc = D.S.pc;
        return;
      }
    }
    while (ops < maxops && vm_step_op(D.S)) {
      ++ops;
      if (D.S.halted || D.S.trapped || D.S.timeout)
        break;
      if (D.S.pc == D.resume_pc) { // just executed the resumed stop op
        D.resume_pc = (size_t)-1;
        continue;
      }
      int st = stop_at(D.S.pc);
      if (st == 2) {
        report_bkpt(*bkpt_at(D.S.pc));
        report_watches();
        D.resume_pc = D.S.pc;
        return;
      }
      if (st == 1) {
        std::printf("breakpoint %zu hit after %llu ops (pc=%zu, %s)\n", D.S.pc,
                    (unsigned long long)ops, D.S.pc, loc_str(D.S.pc).c_str());
        report_watches();
        D.resume_pc = D.S.pc;
        return;
      }
    }
    std::printf("%llu ops executed: pc=%zu steps=%llu%s%s%s\n",
                (unsigned long long)ops, D.S.pc, (unsigned long long)D.S.steps,
                D.S.halted ? " HALTED" : "", D.S.trapped ? " TRAPPED" : "",
                D.S.timeout ? " TIMEOUT" : "");
  };

  char line[4096];
  for (;;) {
    std::printf("dbg> ");
    std::fflush(stdout);
    if (!std::fgets(line, sizeof line, stdin))
      break;
    std::string t(line);
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' ||
                          t.back() == ' ' || t.back() == '\t'))
      t.pop_back();
    // tokenize
    std::vector<std::string> tok{};
    {
      std::string cur{};
      for (char ch : t) {
        if (ch == ' ' || ch == '\t') {
          if (!cur.empty())
            tok.push_back(cur);
          cur.clear();
        } else {
          cur += ch;
        }
      }
      if (!cur.empty())
        tok.push_back(cur);
    }
    if (tok.empty())
      continue;
    const std::string &cmd = tok[0];
    auto num = [&](size_t i, long dflt) -> long {
      return i < tok.size() ? atol(tok[i].c_str()) : dflt;
    };
    if (cmd == "quit" || cmd == "q" || cmd == ":q")
      break;
    if (cmd == "help" || cmd == "h" || cmd == "?") {
      dbg_help();
    } else if (cmd == "src") {
      // numbered source (line references for `vm line L`)
      long only = num(1, 0);
      uint32_t ln = 1;
      size_t p0 = 0;
      while (p0 <= src.size()) {
        size_t pe = src.find('\n', p0);
        std::string_view row =
            src.substr(p0, pe == std::string::npos ? src.size() - p0 : pe - p0);
        if (!only || (long)ln == only) {
          std::printf("%s%4u | %.*s\n",
                      (long)ln == line_at(D.S.pc) && !only ? "->" : "  ",
                      (unsigned)ln, (int)row.size(), row.data());
          if (only)
            break;
        }
        if (pe == std::string::npos)
          break;
        p0 = pe + 1;
        ++ln;
      }
    } else if (cmd == "fns") {
      std::printf("%zu fn(s):\n", P.fns.size());
      for (size_t f = 0; f < P.fns.size(); ++f) {
        const FnUnit &U = P.fns[f];
        std::printf("  [%zu] %-12s ssa=%4u opt=%4u dst=%4u phis=%u\n", f,
                    std::string(P.fn_names[f]).c_str(),
                    (unsigned)U.ssa.code.size(), (unsigned)U.opt.code.size(),
                    (unsigned)U.destroyed.code.size(), U.sstats.phis);
      }
    } else if (cmd == "ir") {
      long f = num(1, -1);
      std::string stage = tok.size() > 2 ? tok[2] : "opt";
      bool all_stages = stage == "all";
      auto dump = [&](size_t fi) {
        const FnUnit &U = P.fns[fi];
        char t1[96], t2[96], t3[96];
        std::snprintf(t1, sizeof t1, "SSA (before opt) [%s]",
                      std::string(P.fn_names[fi]).c_str());
        std::snprintf(t2, sizeof t2, "SSA (after opt) [%s]",
                      std::string(P.fn_names[fi]).c_str());
        std::snprintf(t3, sizeof t3, "destroyed [%s]",
                      std::string(P.fn_names[fi]).c_str());
        if (stage == "ssa" || all_stages)
          print_fn(U.ssa, t1, P.src);
        if (stage == "opt" || all_stages)
          print_fn(U.opt, t2, P.src);
        if (stage == "dst" || all_stages)
          print_fn(U.destroyed, t3, P.src);
      };
      if (f < 0 || f >= (long)P.fns.size())
        for (size_t fi = 0; fi < P.fns.size(); ++fi)
          dump(fi);
      else
        dump((size_t)f);
    } else if (cmd == "cfg") {
      long f = num(1, 0);
      if (f < 0 || f >= (long)P.fns.size())
        f = 0;
      const Fn &F = P.fns[f].ssa;
      if (f == 0)
        print_pre_main_analysis(P); // dominators/loops of pre-opt main
      for (uint32_t b = 0; b < F.nblocks(); ++b) {
        std::printf("  b%u: [i%u..i%u) succ:", b, F.blocks[b].begin,
                    F.blocks[b].end);
        if (F.succ0(b) != NONE)
          std::printf(" b%u", F.succ0(b));
        if (F.succ1(b) != NONE)
          std::printf(" b%u", F.succ1(b));
        if (!F.preds.empty() && !F.preds[b].empty()) {
          std::printf(" ; preds:");
          for (uint32_t p : F.preds[b])
            std::printf(" b%u", p);
        }
        std::printf("\n");
      }
    } else if (cmd == "stats") {
      OptStats os = sum_ostats(P);
      std::printf(
          "  fns: %zu | inlined %u | unrolled %u | dce %u | "
          "hoist %u | mem2reg %u | constcalls %u | tces %u | "
          "scctco %u | cdfolds %u | ifconv %u | traps killed %u | "
          "deadargs %u | fnspec %u | icf %u | jthread %u | argpromo %u\n",
          P.fns.size(), (unsigned)os.inlines, (unsigned)os.unrolls,
          (unsigned)os.dce, (unsigned)os.hoisted, (unsigned)os.mem2reg_lifts,
          (unsigned)os.constcalls, (unsigned)os.tces, (unsigned)os.scctco,
          (unsigned)os.cdfolds, (unsigned)os.ifconvs, (unsigned)os.traps_killed,
          (unsigned)os.deadargs, (unsigned)os.fnspecs, (unsigned)os.icf,
          (unsigned)os.jthreads, (unsigned)os.argpromos);
    } else if (cmd == "plan") {
      long f = num(1, 0);
      if (f < 0 || f >= (long)P.fns.size())
        f = 0;
      const Fn &F = P.fns[f].destroyed;
      EmitPlan PL = make_emit_plan(F);
      std::printf("== fn %ld [%s] ==\norder:", f,
                  std::string(P.fn_names[f]).c_str());
      for (uint32_t b2 : PL.order)
        std::printf(" b%u(nxt=b%u)", b2,
                    PL.next[b2] == ~uint32_t(0) ? 999u : PL.next[b2]);
      std::printf("\n");
      for (uint32_t b = 0; b < F.nblocks(); ++b)
        for (uint32_t i = F.blocks[b].begin; i < F.blocks[b].end; ++i) {
          const Instr &I = F.code[i];
          std::printf("i%u b%u k=%d dst=%u dead=%d kc=%d uses=%u\n", i, b,
                      (int)I.k, I.dst, PL.dead[i] ? 1 : 0,
                      (I.dst < PL.kc.size()) ? PL.kc[I.dst] : -1,
                      (I.dst < PL.uses.size()) ? PL.uses[I.dst] : 0u);
        }
    } else if (cmd == "bc") {
      long f = num(1, -1);
      if (f < 0) {
        std::printf("%s", emit_bc_listing(D.bc).c_str());
      } else {
        // list the ops of one fn (from its offset to the next fn's)
        uint32_t nf = D.S.nfns();
        if ((long)nf <= f) {
          std::printf("no fn %ld\n", f);
        } else {
          uint32_t lo = D.S.fn_off((uint32_t)f);
          uint32_t hi = (f + 1 < (long)nf) ? D.S.fn_off((uint32_t)f + 1)
                                           : (uint32_t)D.bc.size();
          size_t p = lo;
          while (p < hi) {
            std::printf("%s%6zu: %-18s\n", "  ", p,
                        bc_disasm_one(D.bc, p, &p).c_str());
          }
        }
      }
    } else if (cmd == "opt") {
      // the pass explorer: subcommands show/diff/break/list run over the
      // LAST collected snapshot set; a bare `opt [FN]` (re)collects it.
      const std::string &sub = tok.size() > 1 ? tok[1] : "";
      auto snap_idx = [&](const std::string &s,
                          long dflt) -> long { // name or index
        if (!s.empty() && (s[0] >= 'a' && s[0] <= 'z')) {
          for (size_t k = 0; k < snaps.size(); ++k)
            if (snaps[k].name == s)
              return (long)k;
          return -1;
        }
        return s.empty() ? dflt : atol(s.c_str());
      };
      if (sub == "show") {
        if (snaps.empty()) {
          std::printf("(no snapshots yet — run `opt [FN]` first)\n");
        } else {
          long n = tok.size() > 2 ? snap_idx(tok[2], (long)snaps.size() - 1)
                                  : (long)snaps.size() - 1;
          if (n < 0 || n >= (long)snaps.size())
            std::printf("no snapshot #%ld (0..%zu)\n", n, snaps.size() - 1);
          else
            std::printf("%s", snaps[(size_t)n].ir.c_str());
        }
      } else if (sub == "diff") {
        if (snaps.size() < 2) {
          std::printf("(need at least 2 snapshots — run `opt [FN]` first)\n");
        } else {
          long a = tok.size() > 2 ? snap_idx(tok[2], 0)
                                  : 0; // default: overall (first vs last)
          long b = tok.size() > 3
                       ? snap_idx(tok[3], (long)snaps.size() - 1)
                       : (tok.size() > 2 ? a + 1 : (long)snaps.size() - 1);
          if (a < 0 || b < 0 || a >= (long)snaps.size() ||
              b >= (long)snaps.size() || a == b) {
            std::printf("usage: opt diff [A [B]] (snapshots 0..%zu)\n",
                        snaps.size() - 1);
          } else {
            std::printf("== diff: #%ld (%s) -> #%ld (%s) ==\n", a,
                        snaps[(size_t)a].name.c_str(), b,
                        snaps[(size_t)b].name.c_str());
            std::string d =
                unified_diff(snaps[(size_t)a].ir, snaps[(size_t)b].ir);
            std::printf("%s", d.empty() ? "  (identical)\n" : d.c_str());
          }
        }
      } else if (sub == "break") {
        // a breakpoint BETWEEN passes: locate PASS, show where the pipeline
        // stands and what that pass itself changed
        if (snaps.empty()) {
          std::printf("(no snapshots yet — run `opt [FN]` first)\n");
        } else if (tok.size() < 3) {
          std::printf("usage: opt break PASS (e.g. opt break sccp)\n");
        } else {
          long k = snap_idx(tok[2], -1);
          if (k < 0) {
            std::printf("no snapshot named '%s' (see `opt` timeline)\n",
                        tok[2].c_str());
          } else {
            std::printf("* stopped after pass #%ld [%s] (fn %zu [%s])\n", k,
                        snaps[(size_t)k].name.c_str(), snap_fn,
                        std::string(P.fn_names[snap_fn]).c_str());
            if (k > 0) {
              std::string d =
                  unified_diff(snaps[(size_t)k - 1].ir, snaps[(size_t)k].ir);
              std::printf("%s", d.empty() ? "  (this pass changed nothing)\n"
                                          : d.c_str());
            } else {
              std::printf("  (#0 is the baseline — nothing precedes it)\n");
            }
            std::printf("(opt show %ld dumps the full IR at this point; "
                        "opt diff for other pairs)\n",
                        k);
          }
        }
      } else if (sub == "list") {
        PassCfg cfg = pass_cfg_of(C);
        const bool states[PASS_TOGGLE_COUNT] = {
            cfg.p_fold,     cfg.p_copyprop, cfg.p_canon,     cfg.p_cse,
            cfg.p_unreach,  cfg.p_splice,   cfg.p_dce,       cfg.p_mem2reg,
            cfg.p_trapelim, cfg.p_licm,     cfg.p_divmagic,  cfg.p_reassoc,
            cfg.p_unroll,   cfg.p_lcf,      cfg.p_constcall, cfg.p_scctco,
            cfg.p_tailrec,  cfg.p_cdfold,   cfg.p_ifconv,    cfg.do_inline,
            cfg.p_sccp,     cfg.p_ldelim,   cfg.p_callcse,   cfg.p_deadargs,
            cfg.p_fnspec,   cfg.p_icf,      cfg.p_jthread,   cfg.p_argpromo};
        std::printf("pass toggle table (%u names, -O%u):\n",
                    (unsigned)PASS_TOGGLE_COUNT, (unsigned)cfg.level);
        for (uint32_t i = 0; i < PASS_TOGGLE_COUNT; ++i)
          std::printf("  %-10s %s%s", PASS_TOGGLE_NAMES[i],
                      states[i] ? "on " : "OFF", (i % 4) == 3 ? "\n" : "   ");
        if (PASS_TOGGLE_COUNT % 4)
          std::printf("\n");
      } else if (!sub.empty() && sub != "run") {
        std::printf("opt subcommand? try: opt [FN] | show | diff | break | "
                    "list\n");
      } else {
        long f = sub.empty() ? num(1, 0) : atol(sub.c_str());
        if (f < 0 || f >= (long)P.fns.size())
          f = 0;
        snaps.clear();
        snap_fn = (size_t)f;
        if (!dbg_collect_snaps(P, (size_t)f, C, snaps)) {
          std::printf("fn %ld: no snapshots (-O0 leaves the IR untouched)\n",
                      f);
        } else {
          dbg_print_timeline(snaps, (size_t)f, P);
          std::printf("(opt show N | opt diff A B | opt break PASS to "
                      "inspect)\n");
        }
      }
    } else if (cmd == "bkpts") {
      if (bkpts.empty())
        std::printf("no language breakpoints (write `breakpoint x;` in the "
                    "source)\n");
      for (size_t k = 0; k < bkpts.size(); ++k) {
        const DbgBk &B = bkpts[k];
        char lb[48];
        const char *loc = SM.at(B.src, lb, sizeof lb) ? lb : "?";
        std::string fnn = B.fn < P.fn_names.size()
                              ? std::string(P.fn_names[B.fn])
                              : std::string("?");
        std::string obs{};
        for (size_t j = 0; j < B.names.size(); ++j) {
          if (j)
            obs += ", ";
          obs += B.names[j].empty() ? ("slot " + std::to_string(B.slots[j]))
                                    : B.names[j];
        }
        std::printf("  #%zu pc=%-6zu fn %-8s %-6s observes: %s\n", k, B.pc,
                    fnn.c_str(), loc, obs.empty() ? "(nothing)" : obs.c_str());
      }
    } else if (cmd == "vars") {
      if (!D.S.started) {
        std::printf("(machine not started — vm reset / vm step first)\n");
      } else {
        uint32_t f = num(1, -1) >= 0 && num(1, -1) < (long)P.fns.size()
                         ? (uint32_t)num(1, -1)
                         : D.S.fn_at(D.S.pc);
        std::vector<std::string> names = dbg_slot_names(P, f);
        std::vector<char> mh = dbg_slot_may_handle(P, f);
        // observed names from language breakpoints enrich the table (a
        // value may live in a temp's slot after copy-propagation)
        for (const DbgBk &B : bkpts)
          if (B.fn == f)
            for (size_t j = 0; j < B.slots.size() && j < B.names.size() &&
                               B.slots[j] < names.size();
                 ++j)
              if (!B.names[j].empty() && names[B.slots[j]].empty())
                names[B.slots[j]] = B.names[j];
        const std::vector<val_t> &fr =
            D.S.frames.empty() ? std::vector<val_t>{} : D.S.frames.back();
        std::string fnn = f < P.fn_names.size() ? std::string(P.fn_names[f])
                                                : std::string("?");
        std::printf("named slots of fn %u (%s) — what survived optimization "
                    "— at %s:\n",
                    (unsigned)f, fnn.c_str(), loc_str(D.S.pc).c_str());
        size_t shown = 0;
        for (size_t s = 0; s < names.size() && s < fr.size(); ++s) {
          if (names[s].empty())
            continue; // temps: not user-visible
          std::printf("    %-12s = %s\n", names[s].c_str(),
                      dbg_val(D.S, fr[s], s < mh.size() && mh[s]).c_str());
          ++shown;
        }
        if (!shown)
          std::printf("    (no named slots — every variable optimized to "
                      "constants / registers)\n");
      }
    } else if (cmd == "watch" || cmd == "unwatch") {
      if (tok.size() < 2) {
        std::printf("usage: %s NAME\n", cmd.c_str());
      } else if (cmd == "watch") {
        for (const std::string &w : watches)
          if (w == tok[1]) {
            std::printf("already watching %s\n", tok[1].c_str());
            goto next_cmd;
          }
        watches.push_back(tok[1]);
        std::printf("watching %s (shown at every stop; try `vm continue`)\n",
                    tok[1].c_str());
      } else if (tok[1] == "*") {
        watches.clear();
        std::printf("all watches cleared\n");
      } else {
        for (size_t i = 0; i < watches.size(); ++i)
          if (watches[i] == tok[1]) {
            watches.erase(watches.begin() + (long)i);
            std::printf("unwatched %s\n", tok[1].c_str());
            break;
          }
      }
    } else if (cmd == "check") {
      RunOut oracle = run_ast(P.P.ast, P.P.root, C.steps, C.inputs);
      RunOut plain = run_prog(fns_of(P, 0), C.steps, C.inputs);
      RunOut optimized = run_prog(fns_of(P, 1), C.steps, C.inputs);
      RunOut lowered = run_prog(fns_of(P, 2), C.steps, C.inputs);
      std::vector<uint8_t> bc = emit_bytecode(fns_of(P, 2));
      RunOut vm = vm_run(bc, C.steps, C.inputs);
      const char *names[] = {"oracle", "ssa", "opt", "lower", "vm"};
      RunOut vals[5] = {oracle, plain, optimized, lowered, vm};
      for (int k = 0; k < 5; ++k) {
        std::printf("%-8s: ", names[k]);
        print_run(vals[k], P.src);
      }
      bool ok = same_run(oracle, plain) && same_run(oracle, optimized) &&
                same_run(oracle, lowered) && same_run(oracle, vm);
      std::printf("%s\n", ok ? "ALL AGREE" : "*** MISMATCH ***");
    } else if (cmd == "vm") {
      const std::string &sub = tok.size() > 1 ? tok[1] : "";
      if (sub == "reset") {
        dbg_vm_reset(D, C.steps, C.inputs);
        std::printf("machine reset: pc=%zu, %u fn(s), %zu slots in main\n",
                    D.S.pc, D.S.nfns(),
                    D.S.frames.empty() ? 0 : D.S.frames[0].size());
      } else if (sub == "step") {
        long n = num(2, 1);
        if (n < 1)
          n = 1;
        for (long k = 0; k < n; ++k) {
          if (D.S.halted || D.S.trapped || D.S.timeout)
            break;
          size_t at = D.S.pc;
          std::string d = bc_disasm_one(D.bc, at);
          bool alive = vm_step_op(D.S);
          std::printf("  %6zu: %-18s ; %s stack(%zu)", at, d.c_str(),
                      loc_str(at).c_str(), D.S.st.size());
          if (!D.S.st.empty())
            std::printf(" top=%lld", (long long)D.S.st.back());
          if (D.bc[at] == (uint8_t)Bc::Bkpt) { // show what it observes
            const DbgBk *B = bkpt_at(at);
            if (B && !D.S.frames.empty()) {
              const std::vector<val_t> &fr = D.S.frames.back();
              std::printf(" ; ");
              for (size_t j = 0; j < B->slots.size(); ++j) {
                if (j)
                  std::printf(", ");
                std::string nm = j < B->names.size() && !B->names[j].empty()
                                     ? B->names[j]
                                     : ("slot " + std::to_string(B->slots[j]));
                val_t v = B->slots[j] < fr.size() ? fr[B->slots[j]] : (val_t)0;
                std::printf(
                    "%s=%s", nm.c_str(),
                    dbg_val(D.S, v, j < B->arry.size() && B->arry[j]).c_str());
              }
            }
          }
          std::printf("\n");
          if (!alive)
            break;
        }
        if (D.S.halted)
          std::printf("  HALTED after %llu steps\n",
                      (unsigned long long)D.S.steps);
        if (D.S.trapped)
          std::printf("  TRAPPED at pc=%zu (%s)\n", D.S.pc,
                      loc_str(D.S.pc).c_str());
        if (D.S.timeout)
          std::printf("  TIMEOUT (step limit)\n");
      } else if (sub == "run" || sub == "continue") {
        long maxops = num(2, 1000000000L);
        run_bounded((uint64_t)maxops);
        if (sub == "run") {
          std::printf("output (%zu):", D.S.R.out.size());
          for (val_t v : D.S.R.out)
            std::printf(" %lld", (long long)v);
          std::printf("\n");
        }
      } else if (sub == "until") {
        long target = num(2, -1);
        if (target < 0) {
          std::printf("usage: vm until PC\n");
        } else {
          uint64_t ops = 0;
          while (vm_step_op(D.S)) {
            ++ops;
            if (D.S.pc == (size_t)target)
              break;
            if (D.S.halted || D.S.trapped || D.S.timeout)
              break;
          }
          std::printf(
              "stopped at pc=%zu (target %ld) after %llu ops%s%s%s\n", D.S.pc,
              target, (unsigned long long)ops, D.S.halted ? " HALTED" : "",
              D.S.trapped ? " TRAPPED" : "", D.S.timeout ? " TIMEOUT" : "");
        }
      } else if (sub == "break") {
        long target = num(2, -1);
        if (target >= 0) {
          D.bps.push_back((uint32_t)target);
          std::printf("breakpoint set at pc %ld (%s)\n", target,
                      loc_str((size_t)target).c_str());
        } else {
          std::printf("usage: vm break PC (or vm line L for a source line)\n");
        }
      } else if (sub == "line") {
        long L = num(2, -1);
        if (L <= 0) {
          std::printf("usage: vm line L (source line — see `src`)\n");
        } else {
          size_t added = 0;
          // walk op starts (operands share their op's source offset)
          if (D.bc.size() >= 4) {
            uint32_t nf = bc_rd_u32(D.bc, 0);
            size_t p = 4 + (size_t)8 * nf;
            while (p < D.bc.size()) {
              if ((long)line_at(p) == L) {
                D.bps.push_back((uint32_t)p);
                ++added;
              }
              (void)bc_disasm_one(D.bc, p, &p);
            }
          }
          if (added)
            std::printf("%zu breakpoint(s) set on line %ld (`vm breaks`, "
                        "`vm continue`)\n",
                        added, L);
          else
            std::printf("no bytecode on line %ld (see `src` for numbering)\n",
                        L);
        }
      } else if (sub == "unbreak") {
        if (tok.size() > 2 && tok[2] == "*") {
          D.bps.clear();
          std::printf("all breakpoints cleared\n");
        } else {
          long target = num(2, -1);
          for (size_t i = 0; i < D.bps.size(); ++i)
            if (D.bps[i] == (uint32_t)target) {
              D.bps.erase(D.bps.begin() + (long)i);
              std::printf("breakpoint %ld removed\n", target);
              break;
            }
        }
      } else if (sub == "breaks") {
        if (D.bps.empty())
          std::printf("no manual breakpoints (%zu language breakpoint(s))\n",
                      bkpts.size());
        for (uint32_t bp : D.bps)
          std::printf("  %u: %s (%s)\n", bp, bc_disasm_one(D.bc, bp).c_str(),
                      loc_str(bp).c_str());
      } else if (sub == "state") {
        dbg_vm_state(D, P);
      } else if (sub == "dis") {
        dbg_vm_dis(D, num(2, 16), num(2, 32));
      } else {
        std::printf("vm subcommand? try: reset step run continue until "
                    "break line unbreak breaks state dis\n");
      }
    } else {
      std::printf("unknown command '%s' (try `help`)\n", cmd.c_str());
    }
  next_cmd:;
  }
  std::printf("(debugger session ended)\n");
  return 0;
}

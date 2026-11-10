#pragma once
// === pass_cfg.hpp ========================================
// PassCfg (-O presets, per-pass toggles), the 29-name toggle table
// shared by CLI/fuzzer/--shrink, apply_pass_flag, describe_cfg.

#include "../config.hpp"

struct Fn; // ir/instr.hpp — fwd for the snapshot hook's signature

// ============================================================================
// 9e0. PASS MANAGER
// ============================================================================
// PassCfg is the single configuration threaded from the CLI through
// compile() -> optimize_prog() -> optimize(). Passes live in three cost
// tiers and the scheduler runs them with a strict discipline:
//   tier 0 "cheap"    : sccp, constprop, copyprop, canon, cse, unreachable,
//                       splice, dce -- always run to a FIXPOINT first
//   tier 1 "middle"   : mem2reg (array lifting + dead stores + dead
//                       allocs), ldelim (store->load forwarding + load
//                       CSE), trapelim(+range analysis + compare
//                       folding), jthread (per-edge elbow branch
//                       threading), licm, divmagic, reassoc -- one round
//                       each, followed by a tier-0 fixpoint, repeated
//                       until global fixpoint / --fp-limit
//   tier 2 "expensive": unroll, lcf -- CFG restructuring, run ONLY after
//                       tiers 0/1 reached their fixpoint; each round is
//                       followed by full tier-0 (+1) fixpoints again.
//                       const-call evaluation and pure-call CSE are
//                       interprocedural (driven from optimize_prog, after
//                       the inlining rounds): pure constant-argument calls
//                       are executed exactly at compile time, recursive
//                       fns included; repeated pure calls are deduped.
//   interprocedural (optimize_prog, before the inliner rounds): scctco
//                       collapses every SCC whose internal edges are all
//                       tail calls into loops (see 50b_scctco.hpp)
// -O levels gate the tiers:
//   -O0: nothing (SSA + destruction only)
//   -O1: tier 0
//   -O2: tiers 0+1
//   -O3: everything incl. interprocedural inlining (default)
// Per-pass overrides (--enable/--disable) are applied on top of the preset,
// so `-O1 --enable=lcf` is legal. --debug=N controls the dump volume:
//   0 silent; 1 one line per pass invocation; 2 + the IR after each pass
//   that changed something; 3 reserved for analysis dumps.
struct PassCfg {
  uint32_t level{3};     // -O0..-O3 preset
  uint32_t unroll{16};   // full-unroll constant-trip loops with N <= this
  uint32_t fp_limit{24}; // fixpoint round cap for every tier loop
  uint8_t debug{0};      // dump level (0..3)
  bool regalloc{true};   // C/bytecode register allocation (coloring)
  // tier 0
  bool p_fold{true}, p_copyprop{true}, p_canon{true}, p_cse{true},
      p_unreach{true}, p_splice{true}, p_dce{true}, p_sccp{true};
  bool p_gvn{true}; // tier 0+: value numbering (consts+phi+bin unification)
  // tier 1
  bool p_mem2reg{true}, p_trapelim{true}, p_licm{true}, p_divmagic{true},
      p_reassoc{true}, p_ldelim{true}, p_jthread{true};
  // tier 2
  bool p_unroll{true}, p_lcf{true}, p_constcall{true}, p_tailrec{true},
      p_cdfold{true}, p_ifconv{true}, p_callcse{true};
  bool p_deadargs{true}; // interprocedural dead-argument elimination
  bool p_fnspec{true};   // interprocedural function specialization
  bool p_icf{true};      // interprocedural identical code folding
  bool p_argpromo{true}; // interprocedural argument promotion
  bool p_scctco{true};   // SCC tail-call collapse (inner-SCC tail calls)
  bool p_unswitch{true}; // trivial loop unswitching (invariant header cbr)
  bool p_lsimp{true};    // loop canonicalization (trivial header phis)
  // interprocedural
  bool do_inline{true};
  bool do_opt{true}; // false: skip the optimizer entirely
  // this fn is main (fns[0]): its frame count is provably 1, so a K::Depth
  // in it constant-folds to 1
  bool is_main{false};
  // program-wide gates (filled in by compile(); see prog_handle_gates)
  bool lift_ok{true};   // mem2reg array promotion allowed
  bool stkarr_ok{true}; // C-backend stack arrays allowed
  // runtime-only debugging context (constexpr path leaves it empty)
  std::string_view dbg_name{};
  // runtime-only pass-snapshot hook (the debugger's `opt` explorer and
  // --opt-diff): invoked after EVERY pass invocation with the post-pass fn.
  // Null on the constexpr path — the branch never touches constant
  // evaluation (same discipline as debug/dbg_name).
  using SnapFn = void (*)(const char *pass, bool changed, const Fn &fn,
                          void *ctx);
  SnapFn snap_hook{};
  void *snap_ctx{};

  constexpr void set_level(uint32_t lv) {
    level = lv;
    bool t0 = lv >= 1, t1 = lv >= 2, t2 = lv >= 3;
    p_fold = p_copyprop = p_canon = p_cse = p_unreach = p_splice = p_dce =
        p_sccp = p_gvn = t0;
    p_mem2reg = p_trapelim = p_licm = p_divmagic = p_reassoc = p_ldelim =
        p_jthread = t1;
    p_unroll = p_lcf = p_constcall = p_tailrec = p_cdfold = p_ifconv =
        p_callcse = p_deadargs = p_fnspec = p_icf = p_argpromo = t2;
    p_scctco = t2;
    p_unswitch = t2;
    p_lsimp = t1;
    do_inline = t2;
    do_opt = lv != 0;
  }
};

// ---- pass toggles: shared by the CLI, the fuzzer and --shrink ----------
// (ported from the Branchedmain monolith: fuzz random pass configs need a
// numeric apply + a description; the CLI keeps its string-based
// set_named_pass, which covers the same set plus 'inline'.)
inline constexpr uint32_t PASS_TOGGLE_COUNT = 31;
inline constexpr const char *const PASS_TOGGLE_NAMES[PASS_TOGGLE_COUNT] = {
    "fold",   "copyprop", "canon",     "cse",      "unreach",  "splice",
    "dce",    "mem2reg",  "trapelim",  "licm",     "divmagic", "reassoc",
    "unroll", "lcf",      "constcall", "scctco",   "tailrec",  "cdfold",
    "ifconv", "inline",   "sccp",      "ldelim",   "callcse",  "deadargs",
    "fnspec", "icf",      "jthread",   "argpromo", "gvn",      "unswitch",
    "lsimp"};
constexpr bool apply_pass_flag(PassCfg &p, uint32_t idx, bool on) {
  switch (idx) {
  case 0:
    p.p_fold = on;
    return true;
  case 1:
    p.p_copyprop = on;
    return true;
  case 2:
    p.p_canon = on;
    return true;
  case 3:
    p.p_cse = on;
    return true;
  case 4:
    p.p_unreach = on;
    return true;
  case 5:
    p.p_splice = on;
    return true;
  case 6:
    p.p_dce = on;
    return true;
  case 7:
    p.p_mem2reg = on;
    return true;
  case 8:
    p.p_trapelim = on;
    return true;
  case 9:
    p.p_licm = on;
    return true;
  case 10:
    p.p_divmagic = on;
    return true;
  case 11:
    p.p_reassoc = on;
    return true;
  case 12:
    p.p_unroll = on;
    return true;
  case 13:
    p.p_lcf = on;
    return true;
  case 14:
    p.p_constcall = on;
    return true;
  case 15:
    p.p_scctco = on;
    return true;
  case 16:
    p.p_tailrec = on;
    return true;
  case 17:
    p.p_cdfold = on;
    return true;
  case 18:
    p.p_ifconv = on;
    return true;
  case 19:
    p.do_inline = on;
    return true;
  case 20:
    p.p_sccp = on;
    return true;
  case 21:
    p.p_ldelim = on;
    return true;
  case 22:
    p.p_callcse = on;
    return true;
  case 23:
    p.p_deadargs = on;
    return true;
  case 24:
    p.p_fnspec = on;
    return true;
  case 25:
    p.p_icf = on;
    return true;
  case 26:
    p.p_jthread = on;
    return true;
  case 27:
    p.p_argpromo = on;
    return true;
  case 28:
    p.p_gvn = on;
    return true;
  case 29:
    p.p_unswitch = on;
    return true;
  case 30:
    p.p_lsimp = on;
    return true;
  default:
    return false;
  }
}
// one-line description for reports/failure .flags files (runtime-only)
static std::string describe_cfg(const PassCfg &p) {
  std::string r = "-O";
  r += char('0' + (char)p.level);
  std::string off{};
  // (explicit field reads in PASS_TOGGLE_NAMES order)
  const bool states[PASS_TOGGLE_COUNT] = {
      p.p_fold,     p.p_copyprop, p.p_canon,    p.p_cse,      p.p_unreach,
      p.p_splice,   p.p_dce,      p.p_mem2reg,  p.p_trapelim, p.p_licm,
      p.p_divmagic, p.p_reassoc,  p.p_unroll,   p.p_lcf,      p.p_constcall,
      p.p_scctco,   p.p_tailrec,  p.p_cdfold,   p.p_ifconv,   p.do_inline,
      p.p_sccp,     p.p_ldelim,   p.p_callcse,  p.p_deadargs, p.p_fnspec,
      p.p_icf,      p.p_jthread,  p.p_argpromo, p.p_gvn,      p.p_unswitch,
      p.p_lsimp};
  for (uint32_t i = 0; i < PASS_TOGGLE_COUNT; ++i)
    if (!states[i]) {
      if (!off.empty())
        off += ",";
      off += PASS_TOGGLE_NAMES[i];
    }
  if (!off.empty())
    r += " --disable=" + off;
  if (p.fp_limit != 24)
    r += " --fp-limit=" + std::to_string(p.fp_limit);
  if (!p.regalloc)
    r += " --no-regalloc";
  return r;
}

struct OptStats {
  uint32_t folds{};       // instrs turned into Const
  uint32_t copies{};      // instrs turned into Copy
  uint32_t brs_folded{};  // Cbr turned into Br
  uint32_t blocks_gone{}; // unreachable blocks removed
  uint32_t dce{};         // dead instrs removed
  uint32_t hoisted{};     // LICM hoists
  uint32_t copyprops{};   // copies propagated away (uses rewritten to source)
  uint32_t cses{};        // recomputations replaced by dominating defs
  uint32_t splices{};     // single-pred/succ block pairs merged
  uint32_t cbr_same{};    // Cbr with identical arms turned into Br
  uint32_t phi_degens{}; // phis reduced to Copy/Const (dead pred / single pred)
  uint32_t canon_swaps{}; // commutative ops put in canonical operand order
  uint32_t canon_cmps{};  // >/>= comparisons rewritten to the </<= direction
  uint32_t inlines{};  // call sites inlined (callee body spliced into caller)
  uint32_t unrolls{};  // loops fully unrolled (constant trip count)
  uint32_t iters{};    // fixpoint iterations of the pass pipeline
  uint32_t reassocs{}; // +/-/*c chains reassociated to canonical forms
  uint32_t reassoc_gone{}; // instructions removed by reassociation
  uint32_t lcf_loops{};    // loops replaced by closed-form series evaluation
  uint32_t lcf_sims{};     // loops replaced by compile-time simulation
  uint32_t lcf_rt{};       // loops replaced by RUNTIME-trip-count closed forms
  uint32_t divmagics{};    // div/mod-by-const -> mulhi/shift/bitops lowerings
  uint32_t notcombs{}; // !(cmp) / (cmp)==0 / (cmp)!=1 compare negations folded
  uint32_t traps_killed{};  // div/mod/load/store traps PROVEN impossible (nt)
  uint32_t mem2reg_lifts{}; // arrays lifted to SSA vregs (registers)
  uint32_t mem2reg_dses{};  // stores deleted for never-read (dead) arrays
  uint32_t
      lcf_gensims{}; // loops folded by the general (any-condition) simulator
  uint32_t
      constcalls{}; // pure constant-argument calls evaluated at compile time
  uint32_t deadallocs{};   // unused Allocs deleted (arrays read by no one)
  uint32_t scctco{};       // SCC members whose inner-SCC tail calls collapsed
  uint32_t scctco_folds{}; // collapsed count-down loops folded to & (c-1)
  uint32_t tces{};         // tail-recursion eliminated (recursion -> loops)
  uint32_t tce_merges{};   // pure-tail SCCs merged into one sel-dispatched fn
  uint32_t cdfolds{};      // countdown loops replaced by exact closed forms
  uint32_t ifconvs{};      // cbrs with provably-binary conds if-converted
  uint32_t sccp_consts{};  // SCCP: defs proven constant (rewritten to Const)
  uint32_t sccp_brs{};     // SCCP: branches pruned to unconditional
  uint32_t ld_fwds{};      // ldelim: loads forwarded from dominating stores
  uint32_t ld_cses{};      // ldelim: loads CSE'd against dominating loads
  uint32_t rangecmps{};    // trapelim: compares folded by range proofs
  uint32_t cmpfacts{};     // fold: compare-domain identities applied
  uint32_t callcses{};     // callcse: pure calls deduped to dominating ones
  uint32_t deadargs{};     // deadargs: parameters removed from fns + call sites
  uint32_t fnspecs{};    // fnspec: specialized clones created (const arg bound)
  uint32_t icf{};        // icf: duplicates folded INTO a representative
  uint32_t jthreads{};   // jthread: branch edges threaded past elbows
  uint32_t argpromos{};  // argpromo: array params promoted to scalar elements
  uint32_t gvns{};       // gvn: values unified (consts, phi pairs, recomputes)
  uint32_t unswitches{}; // unswitch: loops versioned on invariant conds
  uint32_t lsimps{};     // lsimp: trivial loop-carried phis dissolved
};

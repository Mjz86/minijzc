# OPT CHECKLIST — the big-function decomposition queue (opt/ tree)

The running list of oversized optimization functions to decompose
(support layer + slim driver, one small function per phase, scan
bounds and emission order preserved exactly, byte-identical
differential: stdin-aware CLI parity + --demo diff +
regen_audit_results.py + the full battery + the committed fuzz
windows differentially). House style: cdfold_impl.hpp (ckpt 23),
lcf_impl.hpp (ckpt 24), unroll_impl.hpp (ckpt 25).

## Done (do not touch; contracts live in the _impl headers)

- [x] opt/interprocedural/cdfold.hpp — opt_cdfold 642 lines -> 33-line
      driver + cdfold_impl.hpp (ckpt 23, commit 938593c)
- [x] opt/loop/lcf.hpp — opt_lcf 640 lines -> 24-line driver +
      lcf_impl.hpp, 66 small functions (ckpt 24, commit d750ae7)
- [x] opt/loop/unroll.hpp — opt_unroll 470 lines -> 16-line driver +
      unroll_impl.hpp, 33 small functions (ckpt 25, commit db2d70b)
- [x] opt/scalar/fold.hpp — opt_constprop 450 lines -> 11-line driver
      + fold_impl.hpp, 28 small functions + FoldCtx; the inline
      wrapped-arith switch deduped to instr.hpp's shared w_bin
      (ckpt 26; verified: 146-run parity, audit table identical,
      both fuzz windows identical modulo timing)
- [x] opt/loop/unswitch_bif.hpp — unswitch_body_if 432 lines ->
      21-line driver + unswitch_bif_impl.hpp, 57 small functions
      + BifCtx (the gates, the outside-use scan with rr/arm_edge,
      the routes validator, the commit tail: clone, version
      rewrites, VM blocks, retargets, guard, delivered walk,
      rewire); the monolith's `pi = 3' failure sentinel retired
      (return NONE directly — same observable result, documented
      in the impl header) (ckpt 27; verified: 146-run parity,
      audit table identical, both fuzz windows identical modulo
      timing)
- [x] opt/scalar/taildup.hpp — opt_taildup 419 lines -> 24-line
      driver + taildup_impl.hpp, 28 small functions + TdCtx (the
      gates: dedicated-pred pair, content envelope, private
      continuations; the route seed + fixpoint with the born sites;
      the outside-use scan's need/bvals closure, validated before
      the commit's first edit; the commit tail: per-copy vregs,
      routing phis, born phis, the two substituted copies, the
      rewire); TWO dedups (td_prepend_phis: the uphi and born-phi
      prepends' identical statement sequence; td_arg_of: the born
      phis' and the rewire's shared (r,di) lookup), the
      allocation-order landmine documented (ckpt 28; verified:
      146-run parity, audit table identical, both fuzz windows
      identical modulo timing via the committed scripts/
      fuzz_parity.sh)

- [x] opt/mem2reg.hpp — done earlier on this branch (round seven:
      the slim driver + mem2reg_impl.hpp, M2RCtx + the m2r_* phase
      functions; the queue entry above was stale)
- [x] opt/interprocedural/scctco.hpp — scctco_member 392 lines ->
      scctco_impl.hpp (round nine: SccCtx + the member phases, plus
      Fn::resucc deduped); the 735-line file's other half below
- [x] opt/interprocedural/scctco_fold.hpp — scctco_fold 362 lines ->
      17-line driver + scctco_fold_impl.hpp (round nine, part 2)
- [x] opt/loop/rotate.hpp — opt_rotate 405 lines -> 25-line driver +
      rotate_impl.hpp (round eight): RotCtx (the shape tables, the
      def/read classification, the commit's value maps), the gates
      (rot_shape_ok: the canonical shape + the dedicated `br h'
      preheader; rot_exit_ok: the only-exit + X-single-pred +
      X-phi-free; rot_header_phis_ok; rot_collect_hbody: the SAVED
      terminator + the const-test gate + the cap + body_on_true),
      rot_mark_def_sets (HDEF/HPHI/HBDEF + loopdef), rot_after_region
      (F), rot_classify_reads (loop_read/f_read + the outside-use
      safety scan), the value maps (rot_gmap/lmap/rmap/xmap + the
      generic rot_rw), rot_test_cbr, and the commit tail
      (rot_mint_vregs -- the observable gv/lv-then-pv/xv order;
      rot_emit_pre/header/latch/exit + the in-loop/F/verbatim arms;
      rot_succ_rewire). The `continue'-vs-fall-through discipline and
      the phase order preserved exactly (verified: 146-run parity
      byte-identical, fuzz windows 16131-16330 + 14731-14930
      identical modulo timing, audit table identical, selftest 208,
      units 6476, header order 111 + umbrella, includes 493/493,
      stress ALL PASS)

- [x] opt/loop/unswitch.hpp — opt_unswitch 381 lines -> 35-line
      driver + unswitch_impl.hpp (round ten): UsCtx (the shape
      fields, the analysis tables, the scan marks, the commit's merge
      values), us_shape_ok (the trivial/compound pick + the shared
      gates: both guards cond_invariant, the dedicated `br h'
      preheader, the 1..120 growth cap), us_exits_ok (exit_inventory
      + header_phis), us_false_region (the F worklist + loopdef),
      us_outside_uses_ok (the hphi_idx/arg_ok/plain_ok safety scan
      with read_on_false), and the commit tail: us_guard_blocks
      (compound g2/fx/t1 BEFORE the clone map), us_emit_guards (the
      trivial/&&/|| arms + the fx/t1 bodies), us_merge_phis (the
      fresh merge phis at X with the begin/end shift fixup),
      us_rewire (the rk phi-edge rekey + the rw plain-use rewrite).
      THE LANDMINE the first draft hit (fuzz seed 20029, caught by
      the committed differential window, NOT by the 146-run parity):
      UsCtx's id fields must default to NONE exactly like the
      monolith's per-header locals (`g2 = NONE, fx = NONE, t1 =
      NONE`) -- zero-init made the && shape emit the ||-only
      `t1: br H' into block 0. Documented in the struct's contract.
      (verified: 146-run parity byte-identical, fuzz windows 1-300
      + 20000-20300 identical modulo timing, audit table identical,
      selftest 208, units 6482, headers 117 + umbrella, includes
      535, kernel fuzz 3000 all components 0 fails, e2e fuzz 600,
      stress ALL PASS)

## Queue (priority = biggest first; sizes re-measured 2026-11-18)

NOTE (ckpt 31, 2026-11-17): the scctco_member and scctco_fold queue
entries are DONE (round nine, moved to the done list above). The
dedup waves of ckpt 31/33 SHRANK several queue files (the kc/kv
seedings, the bump counters, the ipa twins, the loop-family gates,
the mark/rw enumerations): re-measure before starting. The standing
dedup requirement (now the per-round protocol in CONTINUE.md) applies
to every decomposition: BEFORE the commit, diff-scan the touched
files for inline twins of code that already exists
(scripts/../dup_scan.py pattern or the mjz_lib kernel inventory) and
reuse FIRST.

- [ ] opt/interprocedural/tailrec.hpp — tce_merge ~370 lines
      (745-line file, also on the >700-line file list; the mapv
      remap + count_operand_uses twins already shared)
- [ ] opt/interprocedural/argpromo.hpp — ~408 lines (the ipa_sites /
      remap wave already removed ~70 lines of twins)
- [ ] opt/loop/unswitch_shape.hpp — 433-line file (shared shape
      matcher + clone machinery, all three unswitch paths call it;
      check function sizes when reached)

NOTE (branch generic-ssa, 2026-11-14): the SSA-construction core that
mem2reg shares with ssa_construct was extracted into the generic
index-space kernel `src/minijz/ir/ssa_generic.hpp` (mjz::ssa_ns, the
graph_ns treatment: uintlen_t ids, version_t templates, CSR forests,
N-ary phi records, event-stream renaming, reflow plans, merge-chain
plans). On that branch an.hpp/pred_norm/mem2reg are thin adapters over
it, byte-identical (146-run parity + both fuzz windows + full battery,
units 6369 with 41 new kernel checks including a 3-pred N-ary phi a
2-arg IR cannot spell). The queue item below is what remains THERE:
the classification gates, not the SSA walk (which is now kernel code).

NOTE (branch generic-ssa, third commit, 2026-11-15): EVERY optimization
algorithm got the same treatment -- opt/opt_generic.hpp (mjz::opt_ns),
the kernels over index space, with the passes as policy wrappers:
  * mark_ns  calculate_live_sites -- dce's mark fixpoint over the
    def/use bipartite tables (roots are policy, the flood is kernel)
  * vn_ns    value_number -- the dominator-scoped table walk cse and
    gvn hand-rolled twice: scoped truncation, operand resolution,
    const-keying by VALUE, commutative canonicalization, phi
    congruence with the taint rule (checked over the site rows so
    hit-dropped phis count, exactly like the old code scan), N-ary
    phi records (a 3-pred congruence test pins it)
  * sccp_ns  sccp_engine_t -- the sparse conditional constant ENGINE:
    two worklists, executable-edge tracking, phi merging over
    executable in-edges only (the duplicate-pred ANY-slot rule,
    N-ary), monotone descent, the step guard; the TRANSFER stays a
    client callback on scalars (the std::sort-comparator precedent)
  * pcopy_ns sequentialize_pcopy + calculate_edge_splits -- SSA
    destruction's lost-copy/swap-cycle sequentialization (the temp
    minting is a callback firing at exactly the cycle-break points)
    and the critical-edge split plan (>=2 distinct targets + phis)
  * cfg_ns   calculate_reachable_mask + calculate_splice_plan --
    unreach's flood and splice's pairing sweep (the kernel keeps its
    own rekeyed preds copy, exactly like the old inline sweep)
  * inv_ns   find_unique_outside_pred + calculate_first_invariant_
    site -- licm's unique-preheader rule, invariance predicate and
    header-first scan discipline; eligibility (purity, trap safety)
    stays in the pass
  * ren_ns   build_copy_rename + resolve_capped -- copyprop's rename
    map with the crossed-pair self-loop guard (seed 7406's lesson)
  * calculate_trivial_phi_pick -- dce's phi collapse, N-ary
Plus ir/sweep.hpp: the identical compaction tail of five passes
factored once (sweep_survivors, keep-mask convention like the reflow
plan). Converted wrappers: dce, cse, gvn, copyprop, sccp, unreach,
splice, licm, ir/destruct (sequentialize + split plan + peephole
sweep). Byte-identical: 146-run parity, audit table regenerated
identically, differential fuzz 16281-16380 + 14381-14480 identical
modulo timing, full battery green (selftest 207, ctest 44, units
6434 with section 17's 106 new kernel checks, header order 105/105,
includes 453/453, check-format clean on the tree's clang-format 16).
The catch found on the way: the first draft had the sweep take a
DROP mask while dce's live set is a KEEP mask -- the inversion kept
the dead and dropped the live (the 146-run parity caught it
instantly); the helper now takes the keep mask, the reflow-plan
convention.

NOTE (branch generic-ssa, second commit, 2026-11-15): placement got
the same treatment one layer down. The PURE Cytron relation now lives
in graph_ns itself (mjz_lib/mjz_lib/graph/phis.hpp — vendored, rides
in the zip, not in git): `graph_ns::calculate_phi_placements` is the
var-major relation with NO client assumptions (every def count places
— single-def vars get their DF(def) phis, zero-def vars nothing;
pred rows are not even an input, so an entry sitting in its own cycle
places at a ONE-PRED block), and graph_ns's
`calculate_inv_phi_placements` is the block-major transpose via the
new `make_basic_inv_forest_universe` (csr.hpp — the transpose for
BIPARTITE relations: the square make_basic_inv_forest sizes by the
row count and indexes out of bounds when var space != block space;
the universe variant takes the block count explicitly and fills
ascending). `ssa_ns::calculate_phi_placements` became THE COMPILER'S
POLICY WRAPPER: same signature, same ascending-var block-major
output, now computed as pure placement + the two IR filters (the
<2-defs skip and the >=2-pred-slot rule) — the assumptions moved
OUT of the algorithm and INTO the wrapper where they belong. Units
16i pins the gap both ways (single-def loop-body var places in the
pure layer, is dropped by the wrapper; the entry-cycle arity-1 phi
record lowers through rename_ssa untouched; the non-square universe
transpose both directions). Verified: 146-run parity byte-identical,
fuzz windows 16501-16650 + 16651-16800 identical modulo timing,
audit table regenerated identically, full battery green (selftest
207, ctest 44, units 6385, header order 102/102, includes 431/431,
check-format clean on clang-format 16).


## After the opt/ queue: >700-line files elsewhere

- [ ] back/emit_x64_impl.hpp
- [ ] back/emit_c_impl.hpp
- [ ] interprocedural/scctco.hpp (covered above)
- [ ] apps/fuzz_gen.hpp (909)
- [ ] interprocedural/tailrec.hpp (covered above)

## Standing traps (from ckpt 23-25, apply to every target)

1. `continue`-vs-fall-through discipline is behavior — model it
   explicitly, never implicitly.
2. The differential must feed programs their stdin (audit programs
   read tapes).
3. Never chain on a piped build (`| tail` masks failures) — check
   the "Built target" line or `test -x` on a fresh timestamp.
4. Preserved asymmetries (unguarded reads the original also did)
   stay preserved, documented in the _impl contract.
5. Loop-shape look-alikes are NOT shared between passes unless the
   semantics are identical (lcf precedent).

## Protocol

README task-list ledger update -> full battery green (selftest,
ctest, units, format, parity spot-checks incl. stdin tapes, fuzz
windows clean differentially via scripts/fuzz_parity.sh) -> commit
-> `download/NN-*.zip` with fresh SUMMARY.md + CONTINUE.md ->
archive with ANCHORED excludes (`./build`, `./build-ho`, `./mjz_lib`,
`*.exe`, `*.out.tmp`, `*.mbc`) -> do NOT push origin.

NOTE on format versions: the tree is formatted with clang-format 16
(16.0.6 reproduces check-format clean tree-wide, including the
hand-shaped nested-brace blocks in tests/unit_tests.cpp that
17+ would reflow wholesale). Pip-install that exact version for the
gate.

## The LLVM-gap backlog (added 2026-11-14, from the coverage analysis)

Every item of the LLVM/Clang optimization list was mapped to the
minijzc pass that implements it (LLVM_OPT_COVERAGE.md at the repo
root, the 127-item table; overall 62% of the applicable items, categories:
IR-cleanup 86, control-flow 80, memory 77, interprocedural 76,
scalar 68, loop 54, target-profitability 36, backend 34). EXCLUDED
from this backlog by request: the vectorization family (no SIMD in
the IR), the cache family (prefetch insertion et al. — a scalar
arena language), and the instruction-selection family (single
baseline target; selection is per-instruction emission). What
remains, ranked by value; each names the pass it would extend.

### A. high value — the classic pipeline completions

- [ ] PRE/LCM — partial redundancy elimination. The LICM hoisting
      half + GVN's available half exist; the missing piece is the
      speculative hoist with a compensation copy at the partial-
      predecessor (lazy code motion). Extends licm/gvn's dominator
      walk; needs the expression table keyed like cse's Ent.
- [ ] DSE killing-store chains + store-to-store forwarding — dead
      stores WITHIN a live array (a store overwritten before any
      read) and a later store of the identical value. Extends
      ldelim's dominator-scoped memory table (kill_for_store
      already exists; add the overwrite + same-value rules).
- [ ] loop-access/dependence analysis — which loop iterations touch
      which slots (the alias.single machinery extended by
      induction-subscript recognition). THE ENABLER for group B's
      fusion/interchange/distribution; scored weakest of the loop
      items (0.25) for a reason.
- [ ] IndVarSimplify/SCEV as a shared analysis — lcf's affine
      recurrence algebra + exact trip simulator generalized into
      an analysis other passes query (unroll's trip walk, licm's
      invariance, rotate's guard). Halves of it exist in three
      files; the unification is the work.

### B. medium value — real wins, self-contained

- [ ] code sinking — the missing half of code motion: sink pure
      instrs into the arm that uses them (shorter straight paths,
      lower pressure at joins). Mirror of licm on the dom tree.
- [ ] runtime-trip partial unrolling — unroll is full/constant-
      trip only; the runtime-trip variant (2x body + remainder
      loop, lcf_rt's guard pattern reused) covers the while-loops
      unroll currently declines.
- [ ] loop predication — an in-loop guard on a loop-index test
      (`if (i < n) x = a[i];`-shaped) folded to select arithmetic
      (ifconv's lowering, gated by the interval analysis proving
      both arms trap-free).
- [x] aggressive DCE (the control-dependence kind) — DONE (branch
      generic-ssa, 2026-11-17): `opt/scalar/breq.hpp`, the tier-0
      pass after splice. A Cbr whose arms are PRIVATE, phi-free
      `br`-only relays reconverging on one join (or one arm IS the
      join and the other relays into it), with the join's phis
      carrying the same vreg on both arm edges, folds to `br J`:
      the phis rekey to the single edge (the duplicate-pred shape
      phi_degens collapses), the relay arms go unreachable, the
      condition loses a use. fold's cbr_same keeps the
      identical-TARGETS case; this owns the RECONVERGING case
      (stat cbr~ in the -O3 report). One fold per invocation — the
      tier-0 loop fixpoints it; the pass reads fn.preds fresh each
      call so the sweep's rows stay honest.
- [ ] splice chains — multi-hop single-pred/succ merging (the
      current pass matches pairs only, no chains, documented).
- [ ] function outlining — the inverse of the inliner: identical
      block sequences at >=2 sites extracted under the growth cap
      (icf's serializer would build the key).
- [ ] attribute-inference expansion — noreturn (no Ret reachable),
      always-in-range handles (trapelim's interval proof, cached
      as a fn summary), the willreturn analogue (bounded by the
      depth budget). fnspec/constcall's purity closure is the
      pattern.
- [ ] register coalescing in regalloc — move-related coloring
      (copy chains born at phi destruction already fuse; the
      allocator proper could respect move-adjacency when choosing
      colors). Extends regalloc's chordal coloring.
- [ ] InstCombine rule-set growth — the fold/canon/reassoc layer
      is sound but small (x*2^k -> shift lives in divmagic's
      neighborhood; (x+c1)+c2 chains partially in reassoc). A
      standing wishlist, not a single pass.

### C. machine layer (asm-text level; no instruction selection)

- [ ] machine block placement — order fn.blocks for fallthrough
      (cold blocks out-of-line) before emission; the CFG is
      available, the emitter just walks the order.
- [ ] asm peephole pass — over the emitted text: redundant
      push/pop pairs, mov-to-mov, test-then-branch on a flag the
      previous instr set.
- [ ] spill placement / live-range splitting — regalloc spills at
      def; splitting at the pressure point needs the per-block
      pressure profile the audit table already computes.
- [ ] shrink-wrapping — push/pop only around the region that
      needs the callee-saved regs (per-fn first/last use).

### D. deliberately not planned (with the reason on record)

- vectorization (loop + SLP) — no SIMD in the IR; excluded.
- prefetch/cache family — no cache model for a scalar arena; excluded.
- instruction selection (CPU-specific, SIMD, FMA, target combine,
  addressing-mode formation) — single baseline x86-64, per-instr
  emission; excluded.
- instruction scheduling + microarch tuning — x86-64 OoO cores;
  the win is noise next to the correctness surface. Delegated.
- LCSSA — no current pass needs loop closure; revisit the day PRE
  lands (it simplifies LCM's frontier reasoning).
- UB exploitation — the language has no UB (wrapping arithmetic +
  explicit traps); trapelim is the analogue and already exists.
- branch relaxation, hazard avoidance — delegated to GNU as / the
  OoO core respectively (textual asm emission).

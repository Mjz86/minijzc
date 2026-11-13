# Checkpoint 25 — unroll decomposed (round three) + the algorithm/theory documentation wave

branchA @ (one commit on top of checkpoint 24's lcf commit).
Two things landed, both under the byte-identical discipline:

1. The CONTINUE backlog's item 1 consumed its next named target:
   `opt/loop/unroll.hpp`'s opt_unroll — at 470 lines the biggest
   function left in the tree — got the cdfold/lcf treatment.
2. The standing documentation request ("explain each algorithm and
   data structure in full detail with comments, especially the SSA
   and optimization, also the theory"): 55 files now carry
   theory-level header comments, ~1,570 inserted lines, zero code
   churn.

## What landed

### unroll: the 470-line driver, decomposed (`unroll.hpp` 498 -> 62 + `unroll_impl.hpp` 782)

The single opt_unroll became a 16-line driver plus a support layer
of 33 small constexpr functions (largest 28 lines) + 3 structs:

- **UnrScan/UnrLoop/UnrMaps**: the pass-wide tables, the per-loop
  shape, and the commit's block/vreg/entry maps — the state the
  original held in 20+ locals, now grouped.
- **the scan**: unr_defs (LAST def and LAST Const win — no ndefs==1
  strictness here, that is lcf's; documented in the contract),
  unr_header_list (the nest-descending pair-swap sort).
- **the shape matchers**, in the original's order: unr_loop_edges
  (several in-loop preds tolerated, the LAST is the latch; outside
  must be exactly one), unr_header_term (empty-header check BEFORE
  the terminator read), unr_single_exit (the only-exit-edge scan),
  unr_arms (break-style vs continue-style polarity + the self-loop
  skip), unr_gather_hphis (ALL phis must be (pre,latch)-keyed),
  unr_find_ind -> unr_try_ind -> unr_step_match/unr_test_uses_ind
  (the FIRST matching phi wins; Sub allows only D.a == phi).
- **the plan**: unr_trip (the trip count by RUNNING the loop's own
  wrapping arithmetic — ivk pushed BEFORE the exit check, so ivk[K]
  is the value the loop leaves in i; the cap break leaves K NONE;
  `K > limit` kept as unreachable belt and braces), unr_growth_ok
  (the (K+1)*instrs <= 400 cap).
- **the safety scan**: unr_outside_uses_ok — strict form flags only
  non-h-defined loop values; at the exit block X the phi relaxation
  re-checks with PLAIN loopdef on args keyed to preds other than h
  (both levels are the original's, documented at the functions).
- **the commit**: unr_commit (os.unrolls++ BEFORE the K==0 shortcut),
  unr_zero_trip (the in-place header rewrite), unr_build_maps (the
  k-major allocation order — blocks then vregs — that the register
  audit observes), unr_fill_entry + unr_mapv (the induction as
  per-copy Consts; accumulators chained copy k-1 -> k), the emitters
  (unr_emit_header_copy, unr_emit_body_copies -> unr_emit_body_block
  -> unr_emit_term/unr_edge — the back edge becomes the NEXT copy's
  header), and the rewire tail (unr_rewire_preheader,
  unr_rekey_exit_phis, unr_rewrite_uses — only h-defined values may
  escape, rewritten to the tail's values).

The byte-identical contract is unroll_impl.hpp's header block; the
one preserved landmine worth naming: unr_copy_succs' succ0 line
reads bmap[k][succ0(b)] WITHOUT the `!= NONE` guard its succ1 line
carries — the original's asymmetry, kept not "fixed" (the ckpt-24
class of decision). loop_shape.hpp's look-alikes (loop_scan,
in_loop_flags, header_phis, loop_shape_of) are deliberately NOT
reused (lcf's precedent, the divergence documented in the contract).

### The documentation wave (55 files, ~1,570 comment lines)

Hand-written, the SSA/optimization core (16 files):
- an.hpp — dominators (Lengauer-Tarjan), dominance intervals (the
  O(1) containment test), natural loops, dominance frontiers, the
  Cytron phi-placement theorem (minimal/non-pruned SSA and why the
  NONE args are safe), the renaming-stack theory (why the epilogue
  pops exactly what the prologue pushed; phi args seeded at the
  predecessor's end).
- ir/instr.hpp — the representation theory: flat code vector +
  block ranges (append-only emission), the two-tier vreg/version
  naming, pred-keyd phis in t/f, the arena memory model, the nt
  proof-carrying purity marker.
- ir/builder.hpp — the pre-SSA emission model (no phis ever emitted
  here; short-circuit diamonds as multi-def temps; oracle-order
  evaluation; the loop_stack discipline).
- ir/phi_norm (why phis-first is load-bearing), ir/destruct.hpp
  (parallel-copy lowering: the swap cycle/lost-copy problem, undef
  args -> Const 0, critical edges, the snapshot rebuild).
- opt/mem2reg.hpp — the slot machinery's data structures (AI.single,
  Cls, slot_base, sdefs, init_vreg) and the pipeline gates.
- back/interp_ssa.hpp — edge-parallel phi staging (read-all-then-
  write-all = destruct's semantics) + the five-runner parity chain.
- sccp.hpp — the lattice (TOP/CONST/OVERDEF), Tarski fixpoint,
  sparse delta propagation, executable-edge conditionality.
- gvn.hpp — the scoped-table watermark mechanism; dominator-based
  GVN, not PRE.
- licm.hpp — the soundness argument (purity because of zero-trip
  loops; operand dominance into the preheader; the context-free
  div/mod safety).
- opt/driver.hpp — phase ordering: why a fixpoint at all, the tier
  model, tier-2's load-bearing order (unswitch before unroll before
  lcf, rotate LAST because the rotated header is invisible to the
  other shape matchers).
- divmagic.hpp — Granlund-Montgomery magic numbers (the exactness
  certificate, the signed lowering, the economics).
- callgraph.hpp — SCC scheduling (emission order = callees before
  callers), the inline splice, the Depth adjustment.
- regalloc.hpp — liveness -> interference -> chordal MCS coloring
  -> homes (the fuzz-804 pairwise-param landmine referenced).
- config.hpp — the wrapping/trap semantic contract every backend
  must reproduce. fold.hpp — local vs conditional folding.

Parallel documentation passes (40 files, six agents, all
insert-only, verified): scalar tier-2 (cse, copyprop, dce, canon,
splice, ldelim, unreach), loop tier-2 (lsimp, unswitch,
unswitch_bif, unswitch_shape, lcf, lcf_sim, lcf_rt, loop_shape),
interprocedural tier-2 (icf, argpromo, deadargs, constcall,
callcse, fnspec; tailrec, scctco, scctco_fold got full blocks,
cdfold a THEORY-only add, cdfold_impl skipped as already adequate),
linear + analysis (linform, reassoc, alias, intervals, trapelim),
and the back end (vm, vm_step, stack_alloc, bytecode, emit_plan,
emit_c + impl, emit_x64 + impl). rotate.hpp already had the tree's
best theory header and was left as-is.

## Verification (the strongest available differential)

* 93 CLI parity runs over demos/ + examples/ + examples/audit/
  (--check-all, --opt-report, -O0; the .in tapes piped in; the four
  debugger tours), all diffed byte-identical against the
  pre-refactor binary (build/minijz-ckpt24 saved before any edit).
* The full 206-check --demo suite and selftest 207/207.
* The register-audit table regenerated byte-identically
  (regen_audit_results.py; RESULTS.md untouched: 710 vregs /
  573 housed / 137 spill slots / 424 traffic).
* Differential fuzz, old binary vs new, on the committed windows:
  16131-16280 (rotation) and 14231-14430 (taildup + rt-lcf) —
  identical modulo timing lines, zero mismatches.
* ctest 44/44, units 6328/6328, check-format clean; header order
  99/99 + umbrella (unroll_impl.hpp is self-contained); include
  portability 419/419.

## Measured shape

Top-level brace-matched functions in src/minijz/**/*.hpp: 286 ->
319 (+33: the unroll pair's 34 functions, the old opt_unroll
replaced). <= 10 lines: 85 -> 93; 11-20: 88 -> 109; 21-40: 40 ->
45; 41-70: 13 (unchanged); 71+: 60 -> 59 — the 470-line opt_unroll
is gone. The biggest function in the tree is now opt_constprop
(~450, fold.hpp), the next backlog target.

## Known limits

* unr_single_exit (28) and unr_trip (26) sit slightly over the 25
  line bar the lcf pair used — splitting them further would trade
  clarity for the metric; stated plainly instead of churning.
* The comment wave covers src/minijz's ir/, opt/, back/ + config.
  front/, apps/, prog/, base/, runtime/ keep their existing
  (checkpoint-era) comments; the same treatment for them is in
  CONTINUE.md's new item 6.
* The tree-wide census remains the brace-matching heuristic; the
  per-file numbers (33 functions, driver 16 lines) are exact.
* No MSVC run (unchanged since checkpoint 16).

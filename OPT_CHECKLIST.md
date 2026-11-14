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

## Queue (priority = biggest first; sizes re-measured 2026-11-14)

- [ ] opt/mem2reg.hpp — ~415 lines (474-line file; the SSA-promotion
      core, theory header already landed at 25) NEXT UP.
- [ ] opt/loop/rotate.hpp — ~405 lines (477-line file)
- [ ] opt/interprocedural/scctco.hpp — scctco_member ~392 lines
      (735-line file, also on the >700-line file list)
- [ ] opt/loop/unswitch.hpp — ~389 lines (468-line file)
- [ ] opt/interprocedural/argpromo.hpp — ~387 lines (465-line file)
- [ ] opt/interprocedural/tailrec.hpp — tce_merge ~385 lines
      (797-line file, also on the >700-line file list)
- [ ] opt/interprocedural/scctco_fold.hpp — ~450-line file, function
      size to re-measure when reached
- [ ] opt/loop/unswitch_shape.hpp — 429-line file (shared shape
      matcher, check function sizes when reached)

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
- [ ] aggressive DCE (the control-dependence kind) — a Cbr whose
      arms reconverge with no observable difference (pure arms,
      same terminator) folds to Br; fold handles the const-cond
      case, this is the both-arms-equivalent case. Extends
      splice's pairing scan.
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

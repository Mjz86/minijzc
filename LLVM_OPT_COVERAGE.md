# minijzc vs the LLVM optimization list - coverage analysis

Every item of the LLVM/Clang architecture-independence list
mapped to the minijzc pass that implements it. Scored:
DONE 1.0 / STRONG 0.75 / PARTIAL 0.5 / WEAK 0.25 / ABSENT 0;
N/A items (the language or IR lacks the construct entirely)
are excluded from the percentages. Generated 2026-11-14 from
the tree at checkpoint 26 (commit 7fdabba).

## Category coverage, most done first

| # | category | coverage | applicable | N/A |
|---|----------|----------:|-----------:|----:|
| 1 | Code-size and IR cleanup | 86% | 9 | 1 |
| 2 | Control-flow optimizations | 80% | 15 | 1 |
| 3 | Memory-related optimizations | 77% | 11 | 2 |
| 4 | Function and interprocedural optimizations | 76% | 13 | 2 |
| 5 | Scalar and expression optimizations | 68% | 19 | 0 |
| 6 | Loop optimizations | 54% | 17 | 0 |
| 7 | Generic but target-dependent (profitability) | 36% | 18 | 0 |
| 8 | Clearly architecture-dependent (backend) | 34% | 15 | 4 |
| | **OVERALL (applicable items)** | **62%** | **117** | **10** |

## Code-size and IR cleanup - 86% (7.75/9)

- [x] **CFG cleanup** - DONE. splice + unreach + jthread
- [x] **Basic-block merging** - DONE. opt/scalar/splice.hpp (single-pred/succ pairs, matching only)
- [x] **Instruction simplification** - DONE. fold + canon
-  --  **Redundant cast elimination** - N/A. no casts in the IR
- [x] **Dead PHI elimination** - DONE. dce trivial-phi collapse + unreach pruning + lsimp
- [x] **Unused global elimination** - STRONG. dead-function sweep (no globals exist)
- [x] **Attribute-based simplification** - STRONG. pure -> callcse/constcall; nt -> divmagic eligibility
- [x] **IR canonicalization** - DONE. canon (compare direction, commutative order) + normalize_phis phis-first invariant + reassoc
- [~] **Debug-information-aware cleanup** - PARTIAL. no debug info, but Bkpt-observed values/arrays are optimization barriers (debugger-aware)
- [x] **Constant and string merging** - STRONG. gvn merges duplicate consts; language has no strings

## Control-flow optimizations - 80% (12.00/15)

- [x] **Dead Code Elimination (DCE)** - DONE. opt/scalar/dce.hpp - SSA mark-sweep from observable roots + trivial-phi collapse
- [x] **Aggressive DCE (ADCE)** - STRONG. dce + sccp proves dead branches -> fold -> unreach removes the control flow
- [x] **Dead Basic Block Elimination** - DONE. opt/scalar/unreach.hpp - reachability flood + isolate + phi arg pruning
- [x] **Dead Global Elimination (GlobalDCE)** - STRONG. dead-function sweep (unreachable from main); language has no global variables
- [x] **SimplifyCFG** - STRONG. splice (block merge) + jthread + unreach + branch folding + phi degeneration
- [x] **Jump threading** - STRONG. opt/scalar/jthread.hpp - per-edge elbow-branch threading
- [x] **Branch folding** - DONE. opt/scalar/fold.hpp - Cbr on known const -> Br (explicit pass phase)
- [x] **Unreachable-block elimination** - DONE. opt/scalar/unreach.hpp
- [x] **If-conversion (simple cases)** - DONE. opt/scalar/ifconv.hpp - provably-binary diamond Cbr -> select arithmetic
-  --  **Switch lowering simplification** - N/A. the language/IR has no switch (Br/Cbr only)
- [x] **Tail duplication** - STRONG. opt/scalar/taildup.hpp - small 2-pred merge blocks duplicated into both arms
- [x] **Loop deletion** - STRONG. lcf/lsimp/cdfold delete provably-computable and trivial loops
- [x] **Loop rotation** - DONE. opt/loop/rotate.hpp - test-first -> test-last + one-shot guard, phis preserved
- [x] **Loop simplification** - STRONG. opt/loop/lsimp.hpp - trivial header-phi elimination, canonical preheader/header shape
- [x] **Loop canonicalization** - STRONG. lsimp + loop_shape.hpp canonical counted-loop form shared by unroll/lcf/unswitch
- [ ] **LCSSA** - ABSENT. absent - values escape loops directly; no loop-closed SSA invariant

## Memory-related optimizations - 77% (8.50/11)

- [x] **Memory-to-register promotion (mem2reg)** - DONE. opt/mem2reg.hpp - single-handle arrays -> SSA slots, IDF phi placement + Cytron renaming, dead-store/alloc elim
- [x] **Scalar Replacement of Aggregates (SROA)** - STRONG. same machinery (array elements -> SSA registers) + argpromo for params; const-index, len<=256 lift limit
- [x] **Dead Store Elimination (DSE)** - STRONG. never-read arrays + dead allocs eliminated; no killing-store chains within live arrays
-  --  **Memory optimization (MemCpyOpt)** - N/A. no memcpy/memset intrinsics in the IR
- [x] **Load/store forwarding** - DONE. opt/scalar/ldelim.hpp - dominator walk, single-allocation slots
- [x] **Redundant load elimination** - DONE. ldelim load-CSE on the same slots
- [x] **Store-to-load forwarding** - DONE. ldelim from_store entries
- [~] **Redundant store elimination** - PARTIAL. dead-array store elimination only
- [x] **Alias-analysis-assisted optimization** - STRONG. opt/analysis/alias.hpp - single-allocation identity analysis driving mem2reg/ldelim/trapelim
- [x] **Stack allocation promotion** - DONE. back/stack_alloc.hpp (non-escaping arrays -> C locals) + mem2reg to registers
- [~] **Local stack-slot merging** - WEAK. destruct.hpp fuses copy chains; x64 backend keeps values in 5 callee-saved regs
-  --  **Lifetime-marker optimization** - N/A. no lifetime markers in the IR
- [~] **Null-pointer check elimination** - PARTIAL. no pointers; the analogue (bounds + divisor trap elimination) is trapelim's nt marks

## Function and interprocedural optimizations - 76% (9.90/13)

- [x] **Function inlining** - DONE. callgraph.hpp opt_inline_one + 4 SCC-ordered rounds; cost model: tiny<=30-no-loops always, single-use<=300, growth cap 8000, recursive excluded
- [x] **Always-inline processing** - STRONG. the always-inline-tiny rule (<=30 instrs, no back edge) + fnspec pre-clones tiny readers
- [ ] **Function outlining** - ABSENT. absent
- [x] **Function specialization** - DONE. opt/interprocedural/fnspec.hpp - per-constant-value clones under growth cap
- [x] **Argument promotion** - DONE. opt/interprocedural/argpromo.hpp - array params -> scalar elements
-  --  **Return-value promotion** - N/A. functions return a single int64; no aggregates
- [x] **Interprocedural constant propagation** - STRONG. constcall + fnspec + callcse give the effect; not the classic param-lattice IPSCCP
- [x] **Interprocedural dead argument elimination** - DONE. opt/interprocedural/deadargs.hpp - greatest fixpoint through soft forwarding edges
- [~] **Global variable optimization (GlobalOpt)** - PARTIAL. no globals; dead-fn sweep + ICF give the code-size effect
- [~] **Function attribute inference (Attributor)** - PARTIAL. fn_pure_closure (cycle-safe purity), recursion/SCC detection, tail-call eligibility, single-handle aliasing
- [~] **Attribute inference: readonly/readnone/noreturn/nounwind/willreturn/nonnull/align/memory-effects** - PARTIAL. pure (=readnone-ish) yes; nt (trap-free = nounwind-ish) yes; noreturn/willreturn/align/nonnull no
- [x] **Dead argument elimination (dup row)** - DONE. deadargs.hpp
-  --  **Global constant propagation** - N/A. no global variables
- [x] **Constant merging** - DONE. gvn unifies duplicate Const defs; icf merges identical code
- [x] **Identical code folding (ICF)** - DONE. opt/interprocedural/icf.hpp - 4-phase: serialize+wildcard, group, 8 refinement rounds, merge

## Scalar and expression optimizations - 68% (13.00/19)

- [x] **Constant folding** - DONE. opt/scalar/fold.hpp - 12-round fixpoint, wrapping + trap discipline
- [x] **Constant propagation** - DONE. fold (local) + sccp (global/conditional)
- [x] **SCCP** - DONE. opt/scalar/sccp.hpp - real SCCP: TOP/CONST/OVERDEF lattice, executable-edge worklists
- [x] **IPSCCP** - STRONG. constcall (pure const-arg calls executed exactly, recursive too) + fnspec + callcse
- [x] **Algebraic simplification** - DONE. fold_impl.hpp - nine identity families + compare-domain facts
- [~] **Instruction combining (InstCombine)** - PARTIAL. fold+canon+reassoc is the combine layer; small rule set vs LLVM's 1000+
- [x] **Reassociation** - STRONG. opt/linear/reassoc.hpp - canonical +/-/[*c] chains (feeds LCF)
- [x] **Strength reduction** - STRONG. divmagic (/c -> mulhi+shift) + lcf closed forms for induction recurrences
- [x] **Common subexpression elimination** - DONE. opt/scalar/cse.hpp - dominator-scoped, value-keyed
- [x] **Global Value Numbering (GVN)** - STRONG. opt/scalar/gvn.hpp - dominator-based VN: consts + phi congruence + bins; no PRE
- [x] **Early CSE** - DONE. opt/scalar/cse.hpp is exactly this (preorder dom walk, scoped table)
- [~] **Partial redundancy elimination (PRE)** - WEAK. only the hoisting half (LICM); no speculation, no LCM
- [x] **Value propagation** - STRONG. sccp + copyprop + range facts propagate values across the CFG
- [x] **Correlated value propagation** - STRONG. intervals.hpp refines ranges per branch outcome; trapelim folds the compares
- [~] **Constraint elimination** - WEAK. intervals are the weaker cousin; no inequality-graph reasoning
- [x] **Range analysis** - DONE. opt/analysis/intervals.hpp - signed intervals, hull join, widening at headers, 3 narrowing rounds, 128-bit corners
- [~] **Undefined-behavior exploitation** - PARTIAL. language has no UB (wrapping + explicit traps); trapelim removes provably-false traps - the analogue
- [ ] **Known bits** - ABSENT. absent
- [ ] **Demanded bits** - ABSENT. absent

## Loop optimizations - 54% (9.25/17)

- [x] **Loop-invariant code motion (LICM)** - DONE. opt/loop/licm.hpp - purity-gated hoist to preheader
- [~] **Induction-variable simplification (IndVarSimplify)** - PARTIAL. loop_shape recognizes counted loops; lsimp kills trivial update phis; no SCEV-based IV rewriting
- [~] **Scalar evolution analysis (ScalarEvolution)** - PARTIAL. lcf's affine-recurrence algebra + exact trip simulation = SCEV-lite (affine int64 subset)
- [x] **Loop unswitching** - STRONG. unswitch.hpp (invariant header test) + unswitch_bif.hpp (invariant inner branch = LLVM simple unswitch)
- [x] **Loop deletion** - STRONG. lcf replaces provable loops with closed forms; lsimp removes no-op updates
- [ ] **Loop fusion** - ABSENT. absent
- [ ] **Loop fission** - ABSENT. absent
- [ ] **Loop distribution** - ABSENT. absent
- [x] **Loop unrolling** - STRONG. opt/loop/unroll.hpp - FULL unrolling of constant-trip loops (cap 16); no partial/runtime unrolling
- [~] **Loop peeling** - WEAK. only rotate's one-shot guard peels iteration 0
- [x] **Loop versioning** - DONE. lcf_rt.hpp (runtime trip-count guard + closed-form version + original fallback) + unswitch versioning
- [ ] **Loop predication** - ABSENT. absent
- [x] **Loop idiom recognition** - STRONG. lcf_sim (GCD/Euclid simulation), cdfold residue folds (n & (c-1)), countdown closed forms
- [x] **Strength reduction inside loops** - STRONG. lcf closed-form series for i, i*i, sums; divmagic
- [x] **Range-check elimination** - DONE. trapelim marks Load/Store nt when the index interval is provably inside [0,L)
- [x] **Bounds-check simplification** - DONE. same machinery (alias len + interval proof)
- [~] **Loop access analysis** - WEAK. alias single-allocation tracking + ldelim slots; no dependence analysis

## Generic but target-dependent (profitability) - 36% (6.50/18)

- [x] **Loop unrolling** - STRONG. full unroll with trip cap 16 (simple cost model)
- [ ] **Loop vectorization** - ABSENT. absent - no SIMD in the IR
- [ ] **SLP vectorization** - ABSENT. absent
- [x] **Inlining** - DONE. real cost model (size, loops, call-site count, growth cap)
- [ ] **Function outlining** - ABSENT. absent
- [~] **Instruction combining w/ target ops** - WEAK. divmagic pre-lowers to MulHi (an IR-native target op); no target hook
- [x] **Strength reduction** - STRONG. divmagic (div ~20-40 cyc -> mulhi+shift ~5)
- [x] **If-conversion** - STRONG. opt_ifconv is unconditional (no profitability ask)
- [ ] **Loop interchange** - ABSENT. absent
- [ ] **Loop fusion** - ABSENT. absent
- [ ] **Loop distribution** - ABSENT. absent
- [ ] **Software pipelining** - ABSENT. absent
- [ ] **Prefetch insertion** - ABSENT. absent
- [~] **Code sinking and hoisting** - PARTIAL. hoisting via LICM; no sinking pass
- [x] **Tail duplication** - STRONG. small-2-pred-block size cap = the cost model
- [x] **Branch folding** - DONE. fold.hpp
- [x] **Jump threading** - STRONG. jthread.hpp
- [ ] **Register-pressure-sensitive transforms** - ABSENT. absent - regalloc runs after, no RP feedback

## Clearly architecture-dependent (backend) - 34% (5.15/15)

- [~] **Instruction selection** - PARTIAL. back/emit_x64*.hpp - correct per-instruction emission, no selection DAG / patterns
- [ ] **Instruction scheduling** - ABSENT. absent (textual asm; order = IR order)
- [x] **Register allocation** - DONE. back/regalloc.hpp - liveness fixpoint + SSA interference + CHORDAL coloring via MCS (optimal for SSA graphs), 5 callee-saved regs
- [~] **Register coalescing** - PARTIAL. ir/destruct.hpp fuses copy chains when phis materialize
- [~] **Target-specific peephole** - WEAK. index/scale addressing in runtime stubs; no machine peephole pass
- [~] **Machine-level DCE** - PARTIAL. vacuously satisfied: IR-level DCE ran before emission
- [~] **Target-specific instruction combining** - WEAK. divmagic does the x64-flavored lowering at IR level
-  --  **Branch relaxation** - N/A. emits GNU asm text; relaxation delegated to 'as'
- [~] **Machine block placement** - WEAK. straight-line IR block order; no layout pass
- [ ] **CPU-specific instruction selection** - ABSENT. single baseline x86-64 target
-  --  **SIMD instruction selection** - N/A. no SIMD/FP in the language
-  --  **Fused multiply-add selection** - N/A. integer-only language
- [~] **Addressing-mode formation** - PARTIAL. base+index*scale displacements used in stubs; per-instr emission elsewhere
- [x] **Stack-frame layout** - STRONG. callee-saved pool, exact 512-frame arena budget, Depth materialization
- [~] **Prologue/epilogue optimization** - PARTIAL. compact push/pop prologues; no shrink-wrapping
- [~] **Spill-code optimization** - WEAK. spills exist in regalloc; no spill placement optimization
- [ ] **Pipeline scheduling** - ABSENT. absent
-  --  **Hazard avoidance** - N/A. x86-64 OoO cores; no VLIW/dual-issue constraints
- [ ] **Microarchitecture-specific tuning** - ABSENT. absent

## Beyond the list: things minijzc has that the LLVM list does not mention

- [x] **Closed-form loop evaluation (lcf)** - affine recurrences -> series (Gauss sums), exact N-iteration simulation of non-affine pure bodies, runtime trip-count versioned closed forms (lcf_rt). LLVM has no single pass that replaces a loop with its closed form.
- [x] **Countdown fold (cdfold)** - TCE'd budget loops -> exact closed forms incl. the n & (c-1) residue rule (scctco_fold).
- [x] **SCC tail-call collapse (scctco) + mutual-recursion merge (tce_merge)** - recursion rings become sel-dispatched loops at IR level, more aggressive than LLVM's late TCO.
- [x] **Exact interprocedural const-call evaluation (constcall)** - pure constant-argument calls executed at compile time, recursive functions included, via the SSA interpreter.
- [x] **Debugger-observability-aware optimization** - Bkpt args are DCE roots; observed arrays stay in the arena (un-promoted).
- [x] **Tiered fixpoint pass manager** - documented phase ordering (unswitch -> unroll -> lcf -> rotate, rotate LAST) with Kleene-style iteration to the least fixed point per tier.

## The honest gaps (ranked by how much they would add)

1. **PRE / LCM** - the classic redundancy elimination completion; LICM+GVN cover the available part only.
2. **Vectorization** - impossible in this IR (no SIMD types), would need IR extensions first.
3. **Loop fusion/fission/interchange/distribution/predication** - absent; legality needs dependence analysis (loop access analysis is the weakest scored loop item, 0.25).
4. **Known/demanded bits, constraint elimination** - bit-level and inequality reasoning beyond intervals.
5. **Machine layer: scheduling, block placement, peepholes, spill optimization** - the backend is a correct naive emitter plus a principled register allocator; everything after regalloc is delegated to GNU as/gcc.
6. **LCSSA** - not needed by the current pass set (no loop pass requires closure); would matter if PRE/loop transforms grew.

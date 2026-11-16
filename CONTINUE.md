# CONTINUE — next steps after checkpoint 29 (reducible-MFAS + breq)

Read SUMMARY.md (same zip root) first. BranchA @ one commit on top of
checkpoint 28; the graph library gained its polynomial reducible-MFAS
and the compiler gained the breq pass. This file is the resume manual.

## The reducible-MFAS, in one paragraph

`mjz_lib/mjz_lib/graph/rmas.hpp`,
`calculate_minimum_feedback_arc_set_reducible(root, graph, weights)`:
contract check (dominator back arcs stripped -> DAG?) -> natural loops
(one region per header, the reverse flood) -> the region lattice
(laminar; header containment is the whole nesting test) -> the PACKING
(innermost-first min-cost flows over shared capacities, arc cost = the
number of enclosing loops whose header paths cross it; the loads are
the discount ledger) -> the CUTS (outermost-first, each arc priced
w(e) minus the inner packing loads, Dinic min cut, filtered to arcs on
some header path). The packing total rides home as packing_weight — a
lower bound on every FAS (the min-max theorem); equal weights prove
the minimum outright. Fuzzed as gns.rmasp: the differential vs the
exact 2^V sweep on EVERY seed + the packing bound + the
irreducible-witness check. The three dead designs and their pinned
seeds (2601: the unsaturated lone crossing arc; 348: the joint
discharge through one shared arc; 496: the parallel pair that
reroutes) are documented in the header — read them before touching
the pricing.

## Where the next work is (priority order)

1. **OPT_CHECKLIST queue** (the decomposition work): mem2reg (~415,
   the classification gates), rotate (~405), scctco_member (~392),
   unswitch (~389), argpromo (~387), tce_merge (~385). The
   cdfold/lcf/unroll/fold/unswitch_bif/taildup treatment applies to
   each.
2. **The LLVM-gap backlog** (section A/B): PRE/LCM (extends
   licm/gvn), DSE killing-store chains (extends ldelim —
   kill_for_store exists, add the overwrite + same-value rules),
   loop-access/dependence analysis, SCEV unification. Section B now
   one item lighter (breq done); code sinking, runtime-trip
   unrolling, loop predication, splice chains, outlining,
   attribute inference, regalloc coalescing, InstCombine growth
   remain. Machine layer: block placement, peephole, spill
   splitting, shrink-wrapping.
3. **Graph lib**: the vendored copy in mjz_lib rides in the zip (not
   git); the algorithmic core is lifted — new passes should be born
   generic per the founding rule. gns.rmasp is the model: the
   fuzzer's differential gate IS the design tool — three iterations
   died on it before the packing ledger, and the shipped design's
   certificate (packing == total) needs no exponential reference.
4. **Widen the fuzz window**: rotation 16131+, random-cfg 14731+.
   Windows verified this checkpoint: e2e 1-2400 + 30001-30400
   (random cfg), kernel campaign 1-30000 all 56 components.

## Build in a recycled sandbox (the short version)

1) cmake is gone again? recreate the shim (the cache points at it):
   mkdir -p scripts/shim, then write scripts/shim/cmake as a /bin/sh
   script whose whole body is: if [ "$1" = "-E" ] && [ "$2" =
   "cmake_link_script" ]; then while IFS= read -r line; do if [ -n
   "$line" ]; then eval "$line" || exit $?; fi; done < "$3"; fi;
   exit 0  — then chmod +x it and point build/CMakeCache.txt's
   CMAKE_COMMAND:INTERNAL at it.
2) rebuild (touch the TUs: the shim skips cmake_depends):
   bash scripts/rebuild.sh minijz && bash scripts/rebuild.sh
   minijz_unit_tests
3) the battery:
   ./build/minijz --selftest          # 208
   ./build/minijz_unit_tests          # 6476
   ./build/minijz --demo              # 206
   ./build/minijz --fuzz-kernels=30000 --fuzz-kernels-from=1   # all 56
   ./build/minijz --fuzz=800 --fuzz-from=1                    # e2e window
   ctest has no binary here; the tests are enumerated in
   build/CTestTestfile.cmake and ran via a python harness this
   checkpoint (cx checks, debugger tours, bugrepros, native gate,
   stress) — all green; reproduce with the same pattern.

## The gates that protect the tree (unchanged)

bash scripts/include_portability_test.sh    # 481/481
bash scripts/header_order_test.sh           # 109 + umbrella
clang-format --dry-run --Werror <file>      # 16.0.6, tree-wide clean
python3 scripts/regen_audit_results.py      # identical this checkpoint

## Protocol (unchanged)

README task-list ledger update -> full battery green -> commit ->
`download/NN-*.zip` with fresh SUMMARY.md + CONTINUE.md -> archive
with ANCHORED excludes (`./build`, `./build-ho`, `./mjz_lib`,
`*.exe`, `*.out.tmp`, `*.mbc`) -> do NOT push origin.

TRAPS: background processes die with the shell session (run
synchronously, never `| tail` a build); the `continue`-vs-fall-through
discipline is behavior; the allocation order of new_anon_vreg calls is
observable (the audit table keys on vreg ids); formatting is
clang-format 16.0.6 exactly.

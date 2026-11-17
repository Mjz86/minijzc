# CONTINUE — next steps after checkpoint 30 (backend kernels + DSE)

Read SUMMARY.md (same zip root) first. The main repo is 13 commits
ahead of origin/main; mjz_lib rides branch `graph-opt-kernels` (six
commits, diffable against main). This file is the resume manual.

## What this checkpoint shipped

1. **The backend analysis kernels** (mjz_lib/opt/liveness.hpp,
   layout.hpp, escape.hpp): regalloc / emit_plan / stack_alloc are
   thin adapters now (146-run byte-identical parity held). Fuzzers
   opt.liveness / opt.trace / opt.escape.
2. **rotate decomposed** (round eight): 25-line driver +
   rotate_impl.hpp; the observable-order ledger (the gv/lv-then-pv/xv
   mint order) is documented in the impl header.
3. **DSE** (opt/scalar/dse.hpp + mjz_lib/opt/dse.hpp): the in-window
   kill rules; toggle `dse` (tier 1, the 35th), stats DSE o+s. The
   window discipline is the soundness core — read the trap-text
   contract (kind AND line are observable) before relaxing anything.
   The kernel's dead ordinals are ALL-EVENT ordinals: pair them
   through an every-event map, never a stores-only array.

## Where the next work is (priority order)

1. **OPT_CHECKLIST queue** (the decomposition work): scctco_member
   (~392), unswitch (~389), argpromo (~387), tce_merge (~385),
   scctco_fold (~450), unswitch_shape (~429). The treatment is
   documented at the top of OPT_CHECKLIST.md; rotate_impl.hpp (this
   checkpoint) is the freshest template.
2. **The LLVM-gap backlog** (section A/B): PRE/LCM (extends
   licm/gvn), loop-access/dependence analysis, SCEV unification.
   DSE's killing-store chains are DONE; remaining: code sinking,
   runtime-trip unrolling, loop predication, splice chains,
   outlining, attribute inference, regalloc coalescing, InstCombine
   growth. Machine layer: block placement (the layout kernels are
   the foundation), peephole, spill splitting, shrink-wrapping.
3. **DSE v2 candidates** (each needs its own soundness story): the
   cross-block overwrite via postdominance (no postdom analysis in
   the tree yet — An would need it), the dead-array global rule
   (mem2reg's sweep owns it), store-to-load-forwarding feeding
   same-value across windows.
4. **Widen the fuzz window**: rotation 16331+, random-cfg 14931+;
   post-DSE windows 1-1300 + 20000-21200 verified this checkpoint.
   NOTE: pre-DSE parity windows are now HISTORICAL (the pass fires);
   the committed fuzz_parity.sh windows for refactors must be
   re-baselined after any new pass.

## Build in a recycled sandbox (the short version)

1) cmake is gone again? recreate the shim (the cache points at it):
   mkdir -p scripts/shim, then write scripts/shim/cmake as a /bin/sh
   script whose whole body is: if [ "$1" = "-E" ] && [ "$2" =
   "cmake_link_script" ]; then while IFS= read -r line; do if [ -n
   "$line" ]; then eval "$line" || exit $?; fi; done < "$3"; fi;
   exit 0  — then chmod +x it and point build/CMakeCache.txt's
   CMAKE_COMMAND:INTERNAL at it.
2) rebuild (touch the TUs: the shim skips cmake_depends):
   bash scripts/rebuild.sh
3) the battery:
   ./build/minijz --selftest          # 208
   ./build/minijz_unit_tests          # 6476
   ./build/minijz --demo              # 206
   ./build/minijz --fuzz-kernels=30000   # all 60 components
   ./build/minijz --fuzz=800              # e2e window
   bash scripts/stress.sh             # via MINIJZ_BIN=./build/minijz
   bugrepros: run each *.mjz under --check-all

## The gates that protect the tree

bash scripts/include_portability_test.sh    # 499/499
bash scripts/header_order_test.sh           # 112 + umbrella
python3 scripts/regen_audit_results.py      # identical unless a new
                                           # pass fires on the audit
                                           # programs (DSE does not)
clang-format --dry-run --Werror <file>      # 16.0.6, tree-wide clean
                                           # (NOT available in this
                                           # sandbox this session)

## Protocol (unchanged)

README task-list ledger update -> full battery green -> commit ->
`download/NN-*.zip` with fresh SUMMARY.md + CONTINUE.md -> archive
excluding `./build`, `./build-ho`, `*.exe`, `*.out.tmp`, `*.mbc`,
and `./fuzz_native` artifacts (the mjz_lib .git RIDES IN the zip:
the branch tracking ships) -> do NOT push origin.

TRAPS: background processes die with the shell session (run
synchronously, never `| tail` a build); the `continue`-vs-fall-
through discipline is behavior; the allocation order of
new_anon_vreg calls is observable (the audit table keys on vreg
ids); formatting is clang-format 16.0.6 exactly; the kernel fuzzer's
component table is ORDER-COUPLED (KZ_NAMES == KZ_FNS ==
fuzz_base's counter arrays) — adding a component means touching all
three plus the static_assert catches drift.

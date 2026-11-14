# CONTINUE — next steps after checkpoint 28 (taildup decomposed)

Read SUMMARY.md (same zip root) first: branchA @ one commit on top
of checkpoint 27's unswitch_bif commit. The OPT_CHECKLIST queue
advanced by one target (opt_taildup, 419 lines, decomposed into a
24-line driver + taildup_impl.hpp's 28 small functions + TdCtx,
byte-identical), and the protocol's differential-fuzz half is now
the committed scripts/fuzz_parity.sh. This file is the resume
manual.

## The generic-ssa branch (the uintlen_t SSA kernel)

Branch `generic-ssa` (off the ckpt-28 main, one commit) holds the
generic SSA-construction redesign: `src/minijz/ir/ssa_generic.hpp`
(`mjz::ssa_ns`, the graph_ns treatment — uintlen_t blocks/vars/sites,
version_t templates, CSR forests, MJZ_CX_FN noexcept) with

  * `calculate_phi_placements` — Cytron IDF placement, ascending-var
    rows (both clients' old loop order),
  * `rename_ssa` — the dominator-tree walk as a PURE TABLE:
    reaching_def[site], def_ordinal[site] (stack-depth-at-push),
    visit_order, and phi records with N-ARY (pred, incoming) entries
    (arity = pred count — a 3-pred join is a 3-entry record; units
    16d/16e pin it, incl. the duplicate-pred last-match rule),
  * `calculate_reflow_plan` — the rebuild-with-removal index math
    (keep mask + per-block head inserts + anchored afters -> new
    ranges + old->new map), consumed by ssa_construct's phi rebuild,
    mem2reg's drop/prepend/init rebuild AND its dead-alloc sweep,
  * `calculate_merge_chain_plan` — the N-preds -> binary-chain
    lowering pred_norm used to hand-roll, now data.

an.hpp's ssa_construct and mem2reg's promotion walk became
gather-events / rename / replay adapters (fresh vregs minted in
visit_order x program order = the old interleaved sequence, so
byte-identical), pred_norm consumes the chain plan. Verified: 146-run
stdin-aware parity vs the pre-change binary, both committed fuzz
windows + a fresh 20000-20149 window differentially clean, full
battery green (selftest 207, ctest 44, units 6369 — 41 new kernel
checks —, header order 103/103, includes 437/437, check-format clean
on clang-format 16). The kernel is liftable into mjz_lib's graph_ns
unchanged (it depends only on graph_lib.hpp).

## Setup (2 min)

```sh
cd minijzc                       # branchA @ the ckpt-28 commit
git log --oneline -4             # ckpt28 ... ckpt27 ... 6ca8ef4 ...
git clone https://github.com/Mjz86/String mjz_lib   # inside this repo
# (NO symlinks: src/mjz_lib/alllib_import.hpp adapts to both String
#  layouts via __has_include and carries the libc++ atomic_ref shim.)
export PATH="$HOME/.venv/bin:$PATH"   # cmake 4.4 + clang-format via pip
# (if the sandbox recycled: pip install cmake clang-format)
# FORMAT VERSION: pip install clang-format==16.0.6 -- the tree is
# formatted with 16; 17+ reflows tests/unit_tests.cpp's hand-shaped
# nested-brace blocks wholesale and fails check-format on code this
# checkpoint did not touch.
cmake -S . -B build && cmake --build build -j4
./build/minijz --selftest                    # 207/207
ctest --test-dir build -j4                  # 44/44 (~9 min)
./build/minijz_unit_tests                   # 6328/6328
./build/minijz --fuzz=300 --fuzz-from=16131 --fuzz-jobs=4   # the open window
```

## Try the checkpoint-28 shape right away

```sh
# 1) the slim driver: 24 lines, the phase sequence top to bottom
sed -n '/^constexpr bool opt_taildup/,/^}/p' src/minijz/opt/scalar/taildup.hpp
#    the 419-line body it replaced lives on as 28 small functions
#    in taildup_impl.hpp: TdCtx, the gates (preds/content/conts/
#    succs), the route seed + fixpoint + born sites, the
#    outside-use scan (need0/need1/bvals, validated before the
#    commit's first edit), the commit tail (per-copy vregs, uphis,
#    born phis, the two copies, the rewire).

# 2) everything still duplicates identically:
./build/minijz --demo                          # 206 checks, all green
bash scripts/parity_diff.sh /tmp/minijz_base ./build/minijz
#    (146 runs byte-identical vs any pre-28 binary you keep around)
bash scripts/fuzz_parity.sh /tmp/minijz_base ./build/minijz 16131 16280
bash scripts/fuzz_parity.sh /tmp/minijz_base ./build/minijz 14231 14430
#    (both committed fuzz windows, modulo timing lines only)

# 3) the two plan halves:
cat OPT_CHECKLIST.md   # the decomposition queue + the LLVM-gap backlog

# 4) the gates that protect the tree:
bash scripts/include_portability_test.sh    # 431/431
bash scripts/header_order_test.sh           # 102/102 + umbrella
cmake --build build --target check-format   # clean (clang-format 16)
```

## Standing backlog (priority order)

1. **Code-quality continuation** (this checkpoint's engine, keep
   going; the queue is OPT_CHECKLIST.md, sizes re-measured there):
   the next target is `opt/mem2reg.hpp` (~415, the SSA-promotion
   core, 474-line file, theory header already landed at 25), then
   `opt/loop/rotate.hpp` (~405), scctco_member (~392), unswitch
   (~389), argpromo (~387), tailrec's tce_merge (~385). The
   cdfold/lcf/unroll/fold/unswitch_bif/taildup treatment applies to
   each (support layer + slim driver, one small function per phase,
   scan bounds and emission order preserved exactly, byte-identical
   differential via scripts/parity_diff.sh + fuzz_parity.sh +
   regen_audit_results.py + the full battery). Files still over 700
   lines: emit_x64_impl, emit_c_impl, scctco, fuzz_gen (909).
   TRAPS TO REMEMBER: the `continue`-vs-fall-through discipline is
   behavior (model it explicitly, never implicitly); the
   ALLOCATION ORDER of new_anon_vreg calls is observable (the
   register-audit table keys on vreg ids — taildup's contract
   documents the pattern); background processes die with the shell
   session here — run ctest/selftest/fuzz synchronously, never `|
   tail` a build (the pipeline masks failures; check "Built
   target").
2. **The LLVM-gap backlog** (OPT_CHECKLIST.md's section — the
   coverage analysis as a ranked plan). High-value first: PRE/LCM
   (extends licm/gvn), DSE killing-store chains (extends ldelim),
   loop-access/dependence analysis (the enabler for the loop
   transforms), SCEV-as-shared-analysis (unifies lcf/unroll/licm
   trip+recurrence logic). Excluded by design: vectorization,
   cache, instruction selection (reasons on record in section D).
3. **Keep widening the fuzz window**: rotation 16131+, random-cfg
   14731+ (the bait vocabulary is seed-floored: 12301 bkpt, 12331
   gvn, 12731 unswitch, 13331 compound-cond, 13631 body-if, 14231
   taildup + rt-lcf, 15531 rotation — a new flavor needs a NEW
   floor constant and the below-floor gate discipline; verify the
   older windows still regenerate byte-identically when you
   replace one). 16131-16280 + 14231-14430 re-verified
   differentially at 28 (modulo timing lines only, via
   scripts/fuzz_parity.sh).

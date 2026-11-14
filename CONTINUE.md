# CONTINUE — next steps after checkpoint 27 (unswitch_bif decomposed)

Read SUMMARY.md (same zip root) first: branchA @ one commit on top
of checkpoint 26's fold commit. The OPT_CHECKLIST queue advanced by
one target (unswitch_body_if, 432 lines, decomposed into a
21-line driver + unswitch_bif_impl.hpp's 57 small functions +
BifCtx, byte-identical), and the plan grew its second half: the
LLVM-gap backlog (OPT_CHECKLIST.md's new section — the coverage
analysis turned into a ranked work list, minus the excluded
vectorization/cache/instruction-selection families). This file is
the resume manual.

## Setup (2 min)

```sh
cd minijzc                       # branchA @ the ckpt-27 commit
git log --oneline -4             # ckpt27 ... ckpt26 ... 7fdabba ...
git clone https://github.com/Mjz86/String mjz_lib   # inside this repo
# (NO symlinks: src/mjz_lib/alllib_import.hpp adapts to both String
#  layouts via __has_include and carries the libc++ atomic_ref shim.)
export PATH="$HOME/.venv/bin:$PATH"   # cmake 4.4 + clang-format via pip
# (if the sandbox recycled: pip install cmake clang-format)
cmake -S . -B build && cmake --build build -j4
./build/minijz --selftest                    # 207/207
ctest --test-dir build -j4                  # 44/44 (~9 min)
./build/minijz_unit_tests                   # 6328/6328
./build/minijz --fuzz=300 --fuzz-from=16131 --fuzz-jobs=4   # the open window
```

## Try the checkpoint-27 shape right away

```sh
# 1) the slim driver: 21 lines, the phase sequence top to bottom
sed -n '66,87p' src/minijz/opt/loop/unswitch_bif.hpp
#    the 432-line body it replaced lives on as 57 small functions
#    in unswitch_bif_impl.hpp: BifCtx, the gates, loopdef/arms,
#    the rr flood, the outside-use scan, the routes validator,
#    the commit tail (clone, rewrites, VMs, retargets, guard,
#    the delivered walk, the rewire).

# 2) everything still unswitches identically:
./build/minijz --demo                          # 206 checks, all green
bash scripts/parity_diff.sh /tmp/minijz_base ./build/minijz
#    (146 runs byte-identical vs any pre-27 binary you keep around)

# 3) the two plan halves:
cat OPT_CHECKLIST.md   # the decomposition queue + the LLVM-gap backlog

# 4) the gates that protect the tree:
bash scripts/include_portability_test.sh    # 427/427
bash scripts/header_order_test.sh           # 101/101 + umbrella
cmake --build build --target check-format   # clean
```

## Standing backlog (priority order)

1. **Code-quality continuation** (this checkpoint's engine, keep
   going; the queue is OPT_CHECKLIST.md, sizes re-measured there):
   the next target is `opt/scalar/taildup.hpp` (~419), then
   `opt/mem2reg.hpp` (~415, the SSA-promotion core),
   `opt/loop/rotate.hpp` (~405), scctco_member (~392), unswitch
   (~389), argpromo (~387), tailrec's tce_merge (~385). The
   cdfold/lcf/unroll/fold/unswitch_bif treatment applies to each
   (support layer + slim driver, one small function per phase,
   scan bounds and emission order preserved exactly, byte-identical
   differential via scripts/parity_diff.sh + regen_audit_results.py
   + the full battery + the committed fuzz windows differentially).
   Files still over 700 lines: emit_x64_impl, emit_c_impl, scctco,
   fuzz_gen (909). TRAPS TO REMEMBER: the `continue`-vs-fall-
   through discipline is behavior (model it explicitly, never
   implicitly); background processes die with the shell session
   here — run ctest/selftest/fuzz synchronously, never `| tail`
   a build (the pipeline masks failures; check "Built target").
2. **The LLVM-gap backlog** (OPT_CHECKLIST.md's new section — the
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
   differentially at 27 (modulo timing lines only).

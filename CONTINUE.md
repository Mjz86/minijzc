# CONTINUE — next steps after checkpoint 26 (fold decomposed)

Read SUMMARY.md (same zip root) first: branchA @ one commit on top
of checkpoint 25's unroll commit. The CONTINUE backlog's item 1
advanced by one target (opt_constprop, 450 lines, decomposed into
an 11-line driver + fold_impl.hpp's 28 small functions,
byte-identical), and the lost-checklist recovery landed as
tooling: OPT_CHECKLIST.md (the queue, persisted) and
scripts/parity_diff.sh (the differential, committed). This file is
the resume manual.

## Setup (2 min)

```sh
cd minijzc                       # branchA @ the ckpt-26 commit
git log --oneline -4             # ckpt26 ... ckpt25 ... db2d70b ...
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

## Try the checkpoint-26 shape right away

```sh
# 1) the slim driver: 11 lines, the fixpoint reads top to bottom
sed -n '58,68p' src/minijz/opt/scalar/fold.hpp
#    the 450-line body it replaced lives on as 28 small functions
#    in fold_impl.hpp: the def-index, the per-round tables, the
#    compare-domain facts, the two-def identity, nine algebraic
#    families, the same-vreg facts, the w_bin dedup, the handlers.

# 2) everything still folds identically (the demo suite covers it):
./build/minijz --demo                          # 206 checks, all green
bash scripts/parity_diff.sh /tmp/minijz_base ./build/minijz
#    (146 runs byte-identical vs any pre-26 binary you keep around)

# 3) the persisted queue (the lost-checklist fix):
cat OPT_CHECKLIST.md        # done list + priority remainder + traps

# 4) the gates that protect the tree:
bash scripts/include_portability_test.sh    # 422/422
bash scripts/header_order_test.sh           # 100/100 + umbrella
cmake --build build --target check-format   # clean
```

## Standing backlog (priority order)

1. **Code-quality continuation** (this checkpoint's engine, keep
   going; the queue is OPT_CHECKLIST.md, sizes re-measured there):
   the next target is `opt/loop/unswitch_bif.hpp`'s
   unswitch_body_if (~432), then taildup (~419), `opt/mem2reg.hpp`
   (~415, the SSA-promotion core), `opt/loop/rotate.hpp` (~405),
   scctco_member (~392), unswitch (~389), argpromo (~387),
   tailrec's tce_merge (~385). The cdfold/lcf/unroll/fold
   treatment applies to each (support layer + slim driver, one
   small function per phase, scan bounds and emission order
   preserved exactly, byte-identical differential via
   scripts/parity_diff.sh + regen_audit_results.py + the full
   battery + the committed fuzz windows differentially). Files
   still over 700 lines: emit_x64_impl, emit_c_impl, scctco,
   fuzz_gen (909). TRAPS TO REMEMBER: the `continue`-vs-fall-
   through discipline is behavior (model it explicitly, never
   implicitly); background processes die with the shell session
   here — run ctest/selftest/fuzz synchronously, never `| tail`
   a build (the pipeline masks failures; check "Built target").
2. **Keep widening the fuzz window**: rotation 16131+, random-cfg
   14731+ (the bait vocabulary is seed-floored: 12301 bkpt, 12331
   gvn, 12731 unswitch, 13331 compound-cond, 13631 body-if, 14231
   taildup + rt-lcf, 15531 rotation — a new flavor needs a NEW
   floor constant and the below-floor gate discipline; verify the
   older windows still regenerate byte-identically when you
   replace one). 16131-16280 + 14231-14430 re-verified
   differentially at 26 (modulo timing lines only).
3. **Documentation continuation**: the remaining trees without
   theory headers are front/ (lexer, ast, parser, diag), apps/
   (cli_parse/cli_run, the debugger family, fuzz_gen/fuzz_run,
   shrink), prog/ (compile_impl, printers, runners), base/
   (pass_cfg's toggle table, srcmap, native_run), runtime/astrun
   (the oracle). Insert-only, <= 78 cols, house style =
   lcf_impl.hpp's header. (The opt/ tree is fully covered: 16
   hand-written at 25 + 40 tier-2 + fold_impl at 26.)
4. **Wishlist remainder** (README task 3): the partial/guarded
   unswitch family proper, then LCSSA (document as
   not-constructible — the IR has no undef), demanded/known bits
   (thin: no bitwise syntax in the surface language), MemCPyOpt,
   loop fusion/fission/distribution, function outlining,
   Attributor-style inference, GlobalOpt (no globals — likely
   not-constructible too).
5. **Allocator follow-ups the audit motivates**: baseline 710
   vregs / 137 spill slots / 424 traffic — live-range splitting at
   the loop edges or rematerializing the guard/latch copies' cheap
   constants are the natural experiments; regen_audit_results.py
   re-measures (the audit is the observer, not the judge). After
   any FUTURE pass that changes the optimized IR the table WILL
   move — expected; only pure refactors must keep it
   byte-identical (fold's did, at 26).
6. **A real `vs-msvc` run on actual Windows/MSVC** is still the one
   unverified step (no MSVC in the sandbox; native parity ran on
   g++; see the ckpt-16 SUMMARY "Known limits").

## Protocol (unchanged, plus the two new tools)

README task-list ledger update -> OPT_CHECKLIST.md tick (the
target moves from Queue to Done) -> full battery green (selftest,
ctest, units, format, scripts/parity_diff.sh vs the pre-refactor
binary, regen_audit_results.py identical, fuzz windows clean
differentially) -> commit -> `download/NN-*.zip` with fresh
SUMMARY.md + CONTINUE.md -> archive with ANCHORED excludes
(`./build`, `./build-ho`, `./mjz_lib`, `*.exe`, `*.out.tmp`,
`*.mbc` — never a bare `--exclude=mjz_lib`, it eats the tracked
`src/mjz_lib/`) -> do NOT push origin (sandbox has no
credentials).

## Landmines hit this checkpoint (worth remembering)

- **Background processes die with the shell session.** A nohup'd
  ctest/selftest vanished with the tool timeout; run the long
  gates synchronously with a generous timeout instead (ctest
  needs ~9 min wall).
- **The `| tail` / piped-build landmine is still the one that
  bites** (kept from 25): check the "Built target" line or
  `test -x` on a fresh timestamp before trusting any parity diff.
- **cmp -s on three files (out/err/rc)** — the parity gate must
  compare exit codes too, not just stdout: a trap that changed
  depth would show in rc while stdout matches.
- **clang-format realigns trailing struct comments** — the FoldCtx
  member comments moved a column; run clang-format BEFORE the
  first build, not after (dry-run --Werror is the check).

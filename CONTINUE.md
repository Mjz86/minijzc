# CONTINUE — next steps after checkpoint 25 (unroll decomposed + documented)

Read SUMMARY.md (same zip root) first: branchA @ one commit on top
of checkpoint 24's lcf commit. The CONTINUE backlog's item 1
advanced by one target (opt_unroll, 470 lines, decomposed into a
16-line driver + unroll_impl.hpp's 33 small functions, byte-
identical), and the standing documentation request landed: 55 files
now carry theory-level headers (~1,570 comment lines, zero code
churn). This file is the resume manual.

## Setup (2 min)

```sh
cd minijzc                       # branchA @ the ckpt-25 commit
git log --oneline -4             # ckpt25 ... ckpt24 ... d750ae7 ...
git clone https://github.com/Mjz86/String mjz_lib   # inside this repo
# (NO symlinks: src/mjz_lib/alllib_import.hpp adapts to both String
#  layouts via __has_include and carries the libc++ atomic_ref shim.)
export PATH="$HOME/.venv/bin:$PATH"   # cmake 4.4 + clang-format via pip
# (if the sandbox recycled: pip install cmake clang-format)
cmake -S . -B build && cmake --build build -j4
./build/minijz --selftest                    # 207/207
ctest --test-dir build -j4                  # 44/44
./build/minijz_unit_tests                   # 6328/6328
./build/minijz --fuzz=300 --fuzz-from=16131 --fuzz-jobs=4   # the open window
```

## Try the checkpoint-25 shape right away

```sh
# 1) the slim driver: 16 lines, the phase list reads top to bottom
sed -n '48,64p' src/minijz/opt/loop/unroll.hpp
#    the 470-line body it replaced lives on as 33 small functions
#    in unroll_impl.hpp: scan tables, 6 shape matchers, the trip
#    simulator, the safety scan, the emitters + rewire tail — and
#    the byte-identical contract is the file's header block.

# 2) every unrolled loop still folds (the demo suite covers them):
./build/minijz --demo                          # 206 checks, all green
./build/minijz demos/gcd_tour.mjz --check-all  # the general simulator

# 3) the theory headers, one example per family:
sed -n '1,66p'  src/minijz/opt/analysis/an.hpp   # dominators + Cytron SSA
sed -n '1,40p'  src/minijz/ir/instr.hpp          # the IR representation
sed -n '1,35p'  src/minijz/opt/driver.hpp        # phase ordering
sed -n '1,34p'  src/minijz/back/interp_ssa.hpp   # executing SSA directly

# 4) the gates that protect the tree:
bash scripts/include_portability_test.sh    # 419/419
bash scripts/header_order_test.sh           # 99/99 + umbrella
cmake --build build --target check-format   # clean
```

## Standing backlog (priority order)

1. **Code-quality continuation** (this checkpoint's engine, keep
   going): the next-biggest function is `opt/scalar/fold.hpp`'s
   opt_constprop (~450), then `opt/loop/unswitch_bif.hpp`'s
   unswitch_body_if (~432), taildup (~419), `opt/mem2reg.hpp`
   (~415), `opt/loop/rotate.hpp` (~405), unswitch (~389),
   scctco_member (~392), argpromo (~387), tailrec's tce_merge
   (~385). The cdfold/lcf/unroll treatment applies to each (support
   layer + slim driver, one small function per phase, scan bounds
   and emission order preserved exactly, byte-identical
   differential: stdin-aware CLI parity + --demo diff +
   regen_audit_results.py + the full battery + the committed fuzz
   windows differentially). Files still over 700 lines:
   emit_x64_impl, emit_c_impl, scctco, fuzz_gen (909).
   TRAPS TO REMEMBER: the `continue`-vs-fall-through discipline is
   behavior (model it explicitly, never implicitly); opt_constprop
   is a 12-round fixpoint whose per-round table rebuilds are part
   of the observable iteration count (os.iters).
2. **Keep widening the fuzz window**: rotation 16131+, random-cfg
   14731+ (the bait vocabulary is seed-floored: 12301 bkpt, 12331
   gvn, 12731 unswitch, 13331 compound-cond, 13631 body-if, 14231
   taildup + rt-lcf, 15531 rotation — a new flavor needs a NEW
   floor constant and the below-floor gate discipline; verify the
   older windows still regenerate byte-identically when you replace
   one). 16131-16430 ran clean at 23; 16131-16280 + 14231-14430
   re-verified differentially at 25.
3. **Documentation continuation** (this checkpoint's second engine):
   the same theory-header treatment for the remaining trees —
   front/ (lexer, ast, parser, diag), apps/ (cli_parse/cli_run,
   the debugger family, fuzz_gen/fuzz_run, shrink), prog/
   (compile_impl, printers, runners), base/ (pass_cfg's toggle
   table, srcmap, native_run), runtime/astrun (the oracle).
   Insert-only, <= 78 cols, house style = lcf_impl.hpp's header.
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
   byte-identical (unroll's did, at 25).
6. **A real `vs-msvc` run on actual Windows/MSVC** is still the one
   unverified step (no MSVC in the sandbox; native parity ran on
   g++; see the ckpt-16 SUMMARY "Known limits").

## Protocol (unchanged)

README task-list ledger update -> full battery green (selftest,
ctest, units, format, parity spot-checks incl. stdin tapes, fuzz
windows clean differentially) -> commit -> `download/26-*.zip` with
fresh SUMMARY.md + CONTINUE.md -> archive with ANCHORED excludes
(`./build`, `./build-ho`, `./mjz_lib`, `*.exe`, `*.out.tmp`,
`*.mbc` — never a bare `--exclude=mjz_lib`, it eats the tracked
`src/mjz_lib/`) -> do NOT push origin (sandbox has no
credentials).

## Landmines hit this checkpoint (worth remembering)

- **A `| tail` pipeline masks build failures.** `cmake --build ...
  | tail -3 && ./build/minijz --selftest` ran the OLD binary after
  a FAILED build — the pipeline's exit status is tail's, not the
  build's. Check the "Built target" line or use `test -x` on a
  fresh timestamp; never chain on a piped build.
- **A cross-struct typo (S.nest for A.nest) surfaced only in the
  unit-tests TU.** With -j4 the main binary linked first and the
  demo diff compared a stale binary against the old one —
  "identical" was meaningless until the real rebuild. Always
  confirm the build's own success line before trusting a parity
  diff (the check above is the fix).
- **unr_copy_succs' succ0 line lacks the `!= NONE` guard its succ1
  line carries.** The original opt_unroll read bmap[k][NONE] on
  Ret-ending body blocks exactly the same way — preserved to the
  character and documented in unroll_impl.hpp's contract, the
  ckpt-24 class of decision ("the asymmetry stays until the pass
  itself is reworked").
- **The differential must feed the programs their stdin** (kept
  from 24): the audit programs read tapes; without them both
  binaries run a much shallower path and the diff stays green while
  covering less.

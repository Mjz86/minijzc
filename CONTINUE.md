# CONTINUE — next steps after checkpoint 24 (lcf decomposed)

Read SUMMARY.md (same zip root) first: branchA @ one commit on top
of checkpoint 23's code-quality commit — the CONTINUE backlog's
item 1 (code-quality continuation) advanced by one target:
`opt/loop/lcf.hpp`'s opt_lcf, at 640 lines the biggest function in
the tree, decomposed into a 24-line driver + `lcf_impl.hpp`
(66 small constexpr functions, nothing over 25 lines), pure
refactor verified byte-identical (differential CLI/demo/audit/fuzz
vs the pre-refactor binary, full battery green). This file is the
resume manual.

## Setup (2 min)

```sh
cd minijzc                       # branchA @ the ckpt-24 commit
git log --oneline -4             # ckpt24 ... ckpt23 ... b17dbbb rotate ...
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

## Try the checkpoint-24 shape right away

```sh
# 1) the slim driver: 24 lines, the phase list reads top to bottom
sed -n '44,70p' src/minijz/opt/loop/lcf.hpp
#    the 640-line body it replaced lives on as 66 small functions
#    in lcf_impl.hpp: scan tables, 5 shape matchers, plan gates,
#    LcfGo tri-state, 2 dispatchers, 2 emitters, the rewire tail.

# 2) every LCF sub-path still folds (the demo suite covers them all):
./build/minijz --demo                          # 206 checks, all green
./build/minijz demos/gcd_tour.mjz --check-all  # the general simulator

# 3) the gates that protect the split:
bash scripts/include_portability_test.sh    # 414/414
bash scripts/header_order_test.sh           # 98/98 + umbrella
cmake --build build --target check-format   # clean
```

## Standing backlog (priority order)

1. **Code-quality continuation** (this checkpoint's engine, keep
   going): the next-biggest functions are `opt/loop/unroll.hpp`'s
   opt_unroll (~470), `opt/scalar/fold.hpp`'s opt_constprop (~450),
   `opt/loop/unswitch_bif.hpp`'s unswitch_body_if (~432), taildup
   (~419), `opt/mem2reg.hpp` (~415), `opt/loop/rotate.hpp` (~405),
   unswitch (~389), scctco_member (~392), argpromo (~387),
   tailrec's tce_merge (~385) — the lcf/cdfold treatment (support
   layer + slim driver, one small function per phase, the scan
   bounds and emission order preserved exactly, byte-identical
   differential: CLI parity runs + --demo suite diff +
   regen_audit_results.py + the full battery) applies to each.
   Files still over 700 lines: emit_x64_impl (727), emit_c_impl
   (720), scctco (705), fuzz_gen (909), fuzz_run (691).
   THE TRAP TO REMEMBER (hit at 24, in review): the original's
   `continue`-vs-fall-through discipline around each gate is
   behavior — model it explicitly (lcf_find_ind's bool return /
   LcfGo tri-state), never implicitly.
2. **Keep widening the fuzz window**: rotation 16131+, random-cfg
   14731+ (the bait vocabulary is seed-floored: 12301 bkpt, 12331
   gvn, 12731 unswitch, 13331 compound-cond, 13631 body-if, 14231
   taildup + rt-lcf, 15531 rotation — the next flavor needs a NEW
   floor constant, and the below-floor gate discipline: a new pass
   joins the fuzz config only from its floor UP, so committed
   fixtures of every older window keep regenerating
   byte-identically — verify that on the window you are replacing).
   16131-16430 ran clean in checkpoint 23's spot checks; 16131-16280
   and 14231-14430 re-verified clean (differentially!) at 24.
3. **Wishlist remainder** (README task 3): the partial/guarded
   unswitch family proper (version on a loop-VARYING branch whose
   taken arm is cheap to duplicate) is the named next pass now that
   rotation is DONE. After that: LCSSA (document as
   not-constructible — the IR has no undef), demanded/known bits
   (thin: no bitwise syntax in the surface language), MemCPyOpt,
   loop fusion/fission/distribution, function outlining,
   Attributor-style inference, GlobalOpt (no globals — likely
   not-constructible too).
4. **Allocator follow-ups the audit motivates**: the baseline sits at
   710 vregs / 137 spill slots / 424 traffic — live-range splitting
   at the loop edges, or rematerializing the guard/latch copies'
   cheap constants, are the natural experiments, and
   `scripts/regen_audit_results.py` re-measures the whole table in
   one run (the audit is the observer, not the judge). NOTE: after
   any FUTURE pass that changes the optimized IR, the audit table
   WILL move — that is expected; only pure refactors must keep it
   byte-identical (it did at 24).
5. **A real `vs-msvc` run on actual Windows/MSVC** is still the one
   unverified step (no MSVC in the sandbox; native parity ran on
   g++; see the ckpt-16 SUMMARY "Known limits" for what was
   cross-verified back then).

## Protocol (unchanged)

README task-list ledger update -> full battery green (selftest,
ctest, units, format, parity spot-checks, fuzz windows clean) ->
commit -> `download/24-*.zip` with fresh SUMMARY.md + CONTINUE.md ->
archive with ANCHORED excludes (`./build`, `./build-ho`, `./mjz_lib`,
`*.exe`, `*.out.tmp`, `*.mbc` — never a bare `--exclude=mjz_lib`, it
eats the tracked `src/mjz_lib/`) -> do NOT push origin (sandbox has
no credentials).

## Landmines hit this checkpoint (worth remembering)

- **The cond gate is a fork in behavior, not just a filter.** When
  the header test's value is not header-defined, the original
  skips the loop ENTIRELY — the general simulator (which needs the
  same header-defined test) is not tried. A driver that dispatches
  on `ind == NONE` alone would have run the simulator on loops the
  original rejected. lcf_find_ind returns bool and lcf_shape
  propagates it; the LcfGo tri-state carries the same discipline
  past the plan (zero-trip and rt folds return Folded, a failed
  gate returns Skip — the original's `continue`).
- **loop_shape.hpp's helpers look reusable and are not.** loop_scan
  marks kc/kv from the LAST def (lcf needs ndefs==1) and has no
  uses/ndefs; loop_shape_of demands exactly one internal pred (lcf
  takes the LAST of several back-edges as the latch) and derives no
  exit polarity. The split keeps lcf's own scans and documents the
  divergence at the top of lcf_impl.hpp.
- **The fallback simulator's operand check reads a/b2/c2 only.**
  Not I.args — the body is pure so args is empty, but the shape of
  the check is the original's and a "fix" is a behavior change in
  waiting. Same class as ckpt-23's dead-but-allocating vreg: the
  asymmetry stays until the pass itself is reworked.
- **LcfRtIn.C is never set by the caller.** The struct field exists
  (documented "the header compare instr") but the old dispatcher
  left it nullptr and lcf_rt_fold never dereferences it — the
  refactor preserves the null exactly rather than "completing" it.
- **The differential must feed the programs their stdin.** The
  audit programs read tapes; running them without input runs a
  much shallower path on BOTH binaries — the diff stays green while
  covering less. Pipe the .in files (30 stdin-aware runs at 24).

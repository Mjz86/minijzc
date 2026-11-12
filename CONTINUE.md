# CONTINUE — next steps after checkpoint 23 (the code-quality continuation)

Read SUMMARY.md (same zip root) first: branchA @ one commit on top of
checkpoint 22's `b17dbbb` merge — the CONTINUE backlog's item 4
consumed: `apps/debugger.hpp` (970 lines) and
`opt/interprocedural/cdfold.hpp` (809 lines, one 642-line function)
both decomposed into small functions (~10 lines where the logic
allows) and split into focused files (cdfold.hpp 33-line driver +
cdfold_impl.hpp support layer; debugger.hpp 171-line session +
dbg_sess/dbg_cmds/dbg_opt/dbg_vm), pure refactor verified
byte-identical (scripted debugger sessions diffed against the
pre-refactor binary, audit table regenerated identically, full
battery green). This file is the resume manual.

## Setup (2 min)

```sh
cd minijzc                       # branchA @ the ckpt-23 commit
git log --oneline -4             # ckpt23 ... b17dbbb rotate ... 9d4d234 ...
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

## Try the checkpoint-23 shape right away

```sh
# 1) the slim driver: 33 lines, one match per loop header
sed -n '1,40p' src/minijz/opt/interprocedural/cdfold.hpp
#    the 642-line matcher it replaced lives on as ~35 small constexpr
#    functions in cdfold_impl.hpp — cd_match runs them in the old
#    order, cd_apply emits in the old vreg-allocation order.

# 2) the countdown fold still firing (the parity ring, closed-formed):
printf 'fn even(n) { if (n == 0) { return 1; } return odd(n - 1); }\nfn odd(n) { if (n == 0) { return 0; } return even(n - 1); }\nprint even(101);\nprint odd(101);\n' > p.mjz
./build/minijz p.mjz --check-all    # 0 1 everywhere; opt report: CDFOLD 1

# 3) the debugger layers (every command still answers identically):
printf 'opt break rotate\nopt\nopt show 33\nopt continue\nvm reset\nvm step 5\nvm run\ncheck\nquit\n' | \
  ./build/minijz demos/rotate_tour.mjz --debugger --stdin=demos/rotate_tour.in

# 4) the gates that protect the split:
bash scripts/include_portability_test.sh    # 410/410
bash scripts/header_order_test.sh           # 97/97 + umbrella
cmake --build build --target check-format   # clean
```

## Standing backlog (priority order)

1. **Code-quality continuation** (this checkpoint's engine, keep
   going): the next-biggest functions are `opt/loop/lcf.hpp`'s
   opt_lcf (~640), `opt/loop/unroll.hpp`'s opt_unroll (~470),
   `opt/scalar/fold.hpp`'s opt_constprop (~450), taildup (~419),
   fuzz_gen's gen_stmt (~408), `opt/loop/rotate.hpp` (~405),
   unswitch (~389) — the cdfold treatment (support layer + slim
   driver, one small function per phase, the scan bounds and
   emission order preserved exactly) applies to each; the byte-
   identical differential recipe is: scripted debugger/CLI session
   diff + --check-all + regen_audit_results.py + the full battery.
   Files still over 700 lines: emit_x64_impl (727), emit_c_impl
   (720), scctco (705), fuzz_gen (909), fuzz_run (691).
2. **Keep widening the fuzz window**: rotation 16131+, random-cfg
   14731+ (the bait vocabulary is seed-floored: 12301 bkpt, 12331
   gvn, 12731 unswitch, 13331 compound-cond, 13631 body-if, 14231
   taildup + rt-lcf, 15531 rotation — the next flavor needs a NEW
   floor constant, and the below-floor gate discipline: a new pass
   joins the fuzz config only from its floor UP, so committed
   fixtures of every older window keep regenerating
   byte-identically — verify that on the window you are replacing).
   16131-16430 ran clean in this session's spot checks.
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
   one run (the audit is the observer, not the judge).
5. **A real `vs-msvc` run on actual Windows/MSVC** is still the one
   unverified step (no MSVC in the sandbox; native parity ran on
   g++; see the ckpt-16 SUMMARY "Known limits" for what was
   cross-verified back then).

## Protocol (unchanged)

README task-list ledger update -> full battery green (selftest,
ctest, units, format, parity spot-checks, fuzz windows clean) ->
commit -> `download/23-*.zip` with fresh SUMMARY.md + CONTINUE.md ->
archive with ANCHORED excludes (`./build`, `./build-ho`, `./mjz_lib`,
`*.exe`, `*.out.tmp`, `*.mbc` — never a bare `--exclude=mjz_lib`, it
eats the tracked `src/mjz_lib/`) -> do NOT push origin (sandbox has
no credentials).

## Landmines hit this checkpoint (worth remembering)

- **The scan bounds are load-bearing, not noise.** cdfold's original
  had ten copies of the last-def scan, some bounded by the PASS-ENTRY
  block count, some by the CURRENT one — and the difference is
  behavior: blocks appended by earlier folds in the same pass call
  are visible to the resolver (fn.nblocks()) and deliberately
  invisible to the shape matchers (nb). The helper takes the bound as
  a PARAMETER; a "simplification" to one bound would have changed
  which loops fold.
- **The vreg allocation order is part of the output.** cd_emit_triangle's
  `2N` bin is dead (parity comes from pV) but it ALLOCATES a vreg —
  dropping it would renumber every later vreg and the audit table
  would drift. Dead-but-allocating code stays until the pass itself
  is reworked.
- **The outside-use rewrite precedes the last validations.** The old
  code rewrites uses, then validates the preheader terminator and
  the select dispatch — a late return leaves partial mutation. The
  shape match makes those unreachable in practice, but the ORDER is
  preserved exactly (cd_apply) rather than "fixed": a refactor must
  not silently change failure behavior, only prove it can't happen.
- **A shared report helper must keep message variants distinct.**
  The debugger's manual-bp stop prints two different lines (with the
  ops count once running, without it at the pre-loop check); the ops
  argument carries the distinction — collapsing them would have
  broken the scripted-session diffs the ctest gates replay.
- **The header-order test OOMs under -j4.** Run it with the script's
  default JOBS=2 (its own comment warns); a spurious ORDER-FAIL under
  load disappears on the retry pass.

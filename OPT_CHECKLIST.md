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

## Queue (priority = biggest first; sizes re-measured 2026-11-14)

- [ ] opt/loop/unswitch_bif.hpp — unswitch_body_if ~432 lines
      (517-line file) NEXT UP.
- [ ] opt/scalar/taildup.hpp — ~419 lines (483-line file)
- [ ] opt/mem2reg.hpp — ~415 lines (474-line file; the SSA-promotion
      core, theory header already landed at 25)
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
windows clean differentially) -> commit -> `download/NN-*.zip` with
fresh SUMMARY.md + CONTINUE.md -> archive with ANCHORED excludes
(`./build`, `./build-ho`, `./mjz_lib`, `*.exe`, `*.out.tmp`,
`*.mbc`) -> do NOT push origin.

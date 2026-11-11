# CONTINUE — next steps after checkpoint 20 (taildup + lcf-rt guard fix + recurrence chains)

Read SUMMARY.md (same zip root) first: branchA @ one commit on top of
checkpoint 19 (`bbf3c7b`) — tail duplication (toggle #32, tier 1: small
2-pred merge blocks duplicated into their arms, per-use routing, born
phis at 2-pred re-merges), the latent lcf-rt miscompile the probes
exposed (the guard's use-before-def of an in-loop Const bound), the
linform ANALYSIS mode that unlocks the sum-of-series recurrence
closed forms, and the 14231+ fuzz floor. This file is the resume
manual.

## Setup (2 min)

```sh
cd minijzc                       # branchA @ the ckpt-20 commit
git log --oneline -4             # ckpt20 ... bbf3c7b body-if ... d697225 ...
git clone https://github.com/Mjz86/String mjz_lib   # inside this repo
# (NO symlinks: src/mjz_lib/alllib_import.hpp adapts to both String
#  layouts via __has_include and carries the libc++ atomic_ref shim.)
export PATH="$HOME/.venv/bin:$PATH"   # cmake 4.4 + clang-format via pip
# (if the sandbox recycled: pip install cmake clang-format)
cmake -S . -B build && cmake --build build -j4
./build/minijz --selftest                    # 207/207
ctest --test-dir build -j4                  # 41/41
./build/minijz_unit_tests                   # 6262/6262
./build/minijz --fuzz=300 --fuzz-from=14231 --fuzz-jobs=4   # the bait window
```

## Try the checkpoint-20 features right away

```sh
# 1) tail duplication — the correlated merge (watch TAILDUP + the
#    folded constants 6 / 14 in the diff):
printf 'let c = input();\nlet x = 0;\nif (c > 0) { x = 1; } else { x = 9; }\nlet y = x + 5;\nprint y;\n' > td.mjz
echo "3" | ./build/minijz td.mjz                # 6  (1+5, folded in the arm)
echo "0" | ./build/minijz td.mjz                # 14 (9+5, folded in the arm)
./build/minijz td.mjz --opt-diff=0:taildup      # the tail into both arms

# 2) THE MISCOMPILE FIX — the runtime-trip guard with an in-loop Const
#    bound (oracle 114 vs compiled 3 on checkpoint 19):
printf 'let i = input();\nlet s = 0;\nwhile (i < 40) { i = i + 1; s = s + 2; }\nprint s + i;\n' > rt.mjz
echo "3" | ./build/minijz rt.mjz                # 114 (37 trips x 2 + 40)
echo "45" | ./build/minijz rt.mjz               # 45 (zero trips: the guard)

# 3) the recurrence chain (the ANALYSIS-mode absorption; s reads i
#    BEFORE the update -> sum(3..39) + 40):
printf 'let i = input();\nlet s = 0;\nwhile (i < 40) { s = s + i; i = i + 1; }\nprint s + i;\n' > rc.mjz
echo "3" | ./build/minijz rc.mjz                # 817 (777 + 40) -- the
                                               # closed form fires, no loop

# 4) the scripted debugger session (the committed ctest):
printf 'opt break taildup\nopt\nopt show 21\nopt continue\nvm run\ncheck\nquit\n' \
  | ./build/minijz demos/taildup_tour.mjz --debugger --stdin=demos/taildup_tour.in
# (the tour prints 101 102 103 104 5 10 17 with a=3 n=5)
```

## Standing backlog (priority order)

1. **Keep widening the fuzz window**: rotation 14831+, random-cfg
   14231+ (the bait vocabulary is seed-floored: 12301 bkpt, 12331 gvn,
   12731 unswitch, 13331 compound-cond, 13631 body-if, 14231 taildup +
   rt-lcf — the next flavor needs a NEW floor constant so committed
   fixtures stay byte-identical). The 14231-14530 and 14531-14830
   rotation windows + random-cfg 13831-14030 / 14031-14230 already ran
   clean this session.
2. **Wishlist remainder** (README task 3): tail duplication is DONE —
   the natural next steps are **loop rotation proper** (the taildup
   gates already point at it: the latch merge — the merge whose
   continuation IS the loop header — is the exact shape rotation
   consumes; moving the body-first shape in feeds unroll/lcf more
   closed forms) and the **partial/guarded unswitch family proper**
   (version on a loop-VARYING branch whose taken arm is cheap to
   duplicate — the unswitch_shape clone machinery + a cost model is
   most of it; taildup covered the degenerate case). After that: LCSSA
   (needs undef semantics this IR does not have — a loop-exit phi's
   zero-trip edge has no defined value; document it as
   not-constructible rather than open), demanded/known bits (thin:
   the surface language has no bitwise syntax — only
   divmagic-produced shifts/mulhi), MemCPyOpt, loop
   fusion/fission/distribution, function outlining, Attributor-style
   inference, GlobalOpt (no globals — likely not-constructible too).
3. **Code quality continuation** if wanted: next-biggest files are
   `back/vm.hpp` (~1044), `back/emit_x64_impl.hpp` (~888),
   `apps/debugger.hpp` (~970), `opt/interprocedural/cdfold.hpp`
   (~815) — the support-layer + slim-driver treatment (the lcf and
   unswitch splits) works well; the full battery + all scripted
   debugger sessions protect byte-identical behavior. taildup.hpp
   landed at ~480 and stays one file (one pass, one soundness
   envelope — no split needed).
4. **A real `vs-msvc` run on actual Windows/MSVC** is still the one
   unverified step (no MSVC in the sandbox; this session also lacked
   clang++ — native parity ran on g++; see the ckpt-16 SUMMARY "Known
   limits" for what was cross-verified back then).

## Protocol (unchanged)

README task-list ledger update → full battery green (selftest,
ctest, units, format, parity spot-checks, fuzz windows clean) →
commit → `download/20-*.zip` with fresh SUMMARY.md + CONTINUE.md →
archive with ANCHORED excludes (`./build`, `./build-ho`, `./mjz_lib`,
`*.exe`, `*.out.tmp`, `*.mbc` — never a bare `--exclude=mjz_lib`, it
eats the tracked `src/mjz_lib/`) → do NOT push origin (sandbox has no
credentials).

## Landmines hit this checkpoint (worth remembering)

- **A "constant" bound is not necessarily a preheader value.** The
  while-lowering materializes loop-test constants IN the header, and
  licm only duplicates such a Const out when it hoists an instruction
  that reads it — the header compare is never hoisted (its phi
  operand is loop-carried), so a constant bound routinely stays
  defined INSIDE the loop. Any new preheader/guard code that names
  the bound must either check the def's block or materialize a local
  Const copy (the position-independence argument). The fuzzer missed
  this for two checkpoints because its loops always carried
  per-iteration prints — the LCF purity gate rejected the only shape
  that would have caught it. When probing for latent bugs, plant the
  shape the FUZZER cannot express, not just the shape it does.
- **Routing must be per-USE, not per-value.** A merge's value can
  escape through BOTH continuations at once (used in U0-dominated and
  U1-dominated blocks): routing by value wires one arm through the
  wrong supplier. The born-phi machinery (one nesting level per
  2-pred re-merge) is the general answer.
- **Absorption restrictions are consumer-specific.** linform's
  single-use rule exists because REASSOCIATION re-emits the chain;
  the lcf recurrence scan only reads the linear form (the loop is
  deleted on commit), so multi-use absorption is sound THERE. When a
  restriction is inherited by a new consumer, ask which consumer it
  was protecting — the analysis mode exists exactly for this.
- **Validate before ANY commit edit** (unchanged from ckpt 19): a
  walk that could fail mid-commit would leave broken IR behind.

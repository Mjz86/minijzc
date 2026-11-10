# CONTINUE — next steps after checkpoint 18 (compound unswitch + lcf split + auto-pause)

Read SUMMARY.md (same zip root) first: branchA @ one commit on top of
checkpoint 17 (`f651041`) — the unswitch extension for && / ||
short-circuit loop conditions, the lcf.hpp split (lcf_sim.hpp +
lcf_rt.hpp + constcall's real home), the debugger's `opt break`
auto-pause, and the 13031+/13331+ fuzz windows. This file is the
resume manual.

## Setup (2 min)

```sh
cd minijzc                       # branchA @ the ckpt-18 commit
git log --oneline -4             # ckpt18 ... f651041 unswitch+canon ... 72685f7 ...
git clone https://github.com/Mjz86/String mjz_lib   # inside this repo
# (NO symlinks: src/mjz_lib/alllib_import.hpp adapts to both String
#  layouts via __has_include and carries the libc++ atomic_ref shim.)
export PATH="$HOME/.venv/bin:$PATH"   # cmake 4.4 + clang-format via pip
# (if the sandbox recycled: pip install cmake clang-format)
cmake -S . -B build && cmake --build build -j4
./build/minijz --selftest                    # 207/207
ctest --test-dir build -j4                  # 39/39
./build/minijz_unit_tests                   # 6191/6191
./build/minijz --fuzz=300 --fuzz-from=13331 --fuzz-jobs=4   # the bait window
```

## Try the checkpoint-18 features right away

```sh
# 1) compound unswitch (watch UNSWITCH and the guard diff):
printf 'let f = input();\nlet g = input();\nlet c = 5;\nwhile (f > 0 && g > 0) {\n  if (c <= 0) { break; }\n  c = c - 1;\n  print c;\n}\nprint 1000 + c;\n' > cc.mjz
echo "3 2" | ./build/minijz cc.mjz                # 4 3 2 1 0 1000
echo "0 2" | ./build/minijz cc.mjz                # 1005 (zero iterations)
echo "3 0" | ./build/minijz cc.mjz                # 1005 (second leaf false)
./build/minijz cc.mjz --opt-diff=0:unswitch       # only the versioning diff

# 2) the pass-breakpoint auto-pause:
printf 'opt break sccp\nopt\nopt show 1\nopt continue\ncheck\nquit\n' \
  | ./build/minijz cc.mjz --debugger

# 3) the || form (true arms short-circuit into the loop):
printf 'let f = input();\nlet g = input();\nlet c = 5;\nwhile (f > 0 || g > 0) {\n  if (c <= 0) { break; }\n  c = c - 1;\n  print c;\n}\nprint 1000 + c;\n' > co.mjz
echo "0 5" | ./build/minijz co.mjz                # 4 3 2 1 0 1000
echo "0 0" | ./build/minijz co.mjz                # 1005
```

## Standing backlog (priority order)

1. **Keep widening the fuzz window**: rotation 13631+, random-cfg
   13131+. The bait vocabulary is seed-floored (12301 bkpt, 12331
   gvn, 12731 unswitch, 13331 compound-cond) — the next flavor needs
   a NEW floor constant so committed fixtures stay byte-identical.
2. **Wishlist remainder** (README task 3): the natural next step is
   **BODY-IF unswitching** — version a loop on an invariant INNER
   branch (the classic LLVM "trivial unswitch"; both versions keep
   running the loop, so every exit needs a version-merge block to
   hold the ≤2-preds invariant — the unswitch.hpp clone machinery
   plus per-exit VM blocks is the whole design; the fuzz bait is an
   invariant `if` inside a while). After that: tail duplication,
   loop rotation proper, LCSSA (NOTE: needs undef semantics this IR
   does not have — a loop-exit phi's zero-trip edge has no defined
   value; document it as not-constructible rather than open),
   demanded/known bits (thin here: the surface language has no
   bitwise syntax — only divmagic-produced shifts/mulhi), MemCPyOpt,
   loop fusion/fission/distribution, function outlining,
   Attributor-style inference, GlobalOpt (no globals in this
   language — likely not-constructible too).
3. **Code quality continuation** if wanted: next-biggest files are
   `back/vm.hpp` (~1044), `back/emit_x64_impl.hpp` (~888),
   `apps/debugger.hpp` (~970 after this checkpoint),
   `opt/interprocedural/cdfold.hpp` (~815) — same treatment as the
   debugger/lcf splits (support layer + slim driver) works well; the
   full battery + all three scripted debugger sessions protect
   byte-identical behavior. lcf.hpp is down to ~707 (the driver +
   const-trip closed forms); unswitch.hpp grew to ~706 with the
   compound matcher — if it keeps growing, the trivial/compound
   matchers can extract into a shared `unswitch_shape.hpp`.
4. **A real `vs-msvc` run on actual Windows/MSVC** is still the one
   unverified step (no MSVC in the sandbox; this session's sandbox
   also lacked zig/MinGW, so nothing Windows-shaped was re-verified —
   nothing in ckpt-18 touches platform code; see the ckpt-16 SUMMARY
   "Known limits" for what was cross-verified back then).

## Protocol (unchanged)

README task-list ledger update → full battery green (selftest both
modes, ctest, units, format, parity spot-checks, fuzz windows clean)
→ commit → `download/19-*.zip` with fresh SUMMARY.md + CONTINUE.md →
archive with ANCHORED excludes (`./build`, `./build-ho`, `./mjz_lib`,
`*.exe`, `*.out.tmp`, `*.mbc` — never a bare `--exclude=mjz_lib`, it
eats the tracked `src/mjz_lib/`) → do NOT push origin (sandbox has no
credentials).

## Landmines hit this checkpoint (worth remembering)

- **The break-arm block is OUTSIDE the natural loop.** `if (c <= 0)
  { break; }` lowers to a block that cannot reach the latch, so inL
  does NOT contain it — the clone keeps targeting the ORIGINAL break
  arm (shared by both versions), and the transient post-unswitch CFG
  has dead-block preds (the abandoned original loop + the clone's dead
  diamond) that unreach removes within the same tier round. Judge
  pred-count invariants on the LIVE CFG after tier-0, not on
  `--opt-diff` snapshots.
- **Compound conditions are evaluated on the zero-iteration path
  too.** Every pass that SKIPS in-loop execution (unswitch's false
  path, future guards) must prove the skipped blocks pure — Const/
  Copy/Bin with nt on Div/Mod. The invariance check kills most of it
  (a call computing a test result puts the cond def in-loop); the
  explicit purity gate is the belt-and-braces.
- **licm feeds unswitch.** Both compound leaf compares must be
  hoisted to the preheader before tier 2 (the header scan + the
  ordinary invariant hoist do it); if fp-limit truncates tier 1, the
  compound matcher just sees in-loop cond defs and bails — coverage,
  not correctness.

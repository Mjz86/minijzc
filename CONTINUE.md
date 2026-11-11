# CONTINUE — next steps after checkpoint 21 (backend modularity + the register audit)

Read SUMMARY.md (same zip root) first: branchA @ one commit on top of
checkpoint 20 (`51bf809`) — the vm tripart split (vm.hpp / vm_step.hpp /
interp_ssa.hpp), the emit_x64_impl → regalloc + codegen split (both
byte-identical, verified by a 34-file asm/bytecode corpus diff), the
`--reg-audit` register-allocation table (one allocator, two consumers),
and the twenty natural audit programs in examples/audit/ with their
committed RESULTS.md. This file is the resume manual.

## Setup (2 min)

```sh
cd minijzc                       # branchA @ the ckpt-21 commit
git log --oneline -4             # ckpt21 ... 51bf809 taildup ... bbf3c7b ...
git clone https://github.com/Mjz86/String mjz_lib   # inside this repo
# (NO symlinks: src/mjz_lib/alllib_import.hpp adapts to both String
#  layouts via __has_include and carries the libc++ atomic_ref shim.)
export PATH="$HOME/.venv/bin:$PATH"   # cmake 4.4 + clang-format via pip
# (if the sandbox recycled: pip install cmake clang-format)
cmake -S . -B build && cmake --build build -j4
./build/minijz --selftest                    # 207/207
ctest --test-dir build -j4                  # 42/42
./build/minijz_unit_tests                   # 6291/6291
./build/minijz --fuzz=300 --fuzz-from=14831 --fuzz-jobs=4   # latest window
```

## Try the checkpoint-21 features right away

```sh
# 1) the register-allocation audit on a natural program (18 spills):
./build/minijz examples/audit/aa05_bubble_sort.mjz --reg-audit
# (or the pressure story in one line:)
printf 'let n = input();\nlet a0 = 1; let a1 = 1; let a2 = 1; let a3 = 1;\nlet a4 = 1; let a5 = 1; let a6 = 1; let a7 = 1;\nwhile (a0 < n) {\n  a0 = a0 + a1; a1 = a1 + a2; a2 = a2 + a3; a3 = a3 + a4;\n  a4 = a4 + a5; a5 = a5 + a6; a6 = a6 + a7; a7 = a7 + 1;\n}\nprint a0 + a7;\n' > p.mjz
echo 100 | ./build/minijz p.mjz --reg-audit --quiet
#   <main> 4 blocks | 13 vregs | 9 housed | 4 spills | 12 rd + 8 wr

# 2) the committed audit table (all twenty + the reading guide):
cat examples/audit/RESULTS.md

# 3) the audit gate (the ctest): every program, five runners + the table:
for f in examples/audit/aa*.mjz; do
  s=$(basename "$f" .mjz); i=""
  [ -f examples/audit/$s.in ] && i="--stdin=examples/audit/$s.in"
  ./build/minijz "$f" --check-all --reg-audit --quiet $i > /dev/null || echo "FAIL $s"
done

# 4) exactness spot-check (the audit's traffic == the emitted asm's
#    slot accesses):
./build/minijz p.mjz --emit=asm --out-dir=/tmp --quiet
grep -c "%rbp)" /tmp/p.s    # 46 total slot movq's...
```

## Standing backlog (priority order)

1. **Keep widening the fuzz window**: rotation 15431+, random-cfg
   14431+ (the bait vocabulary is seed-floored: 12301 bkpt, 12331 gvn,
   12731 unswitch, 13331 compound-cond, 13631 body-if, 14231 taildup +
   rt-lcf — the next flavor needs a NEW floor constant so committed
   fixtures stay byte-identical). The 14831-15130 and 15131-15430
   rotation windows + random-cfg 14231-14430 ran clean this session.
2. **Wishlist remainder** (README task 3): loop rotation proper (the
   taildup gates already point at it: the latch merge — the merge whose
   continuation IS the loop header — is the exact shape rotation
   consumes) and the partial/guarded unswitch family proper. After
   that: LCSSA (document as not-constructible — the IR has no undef),
   demanded/known bits (thin: no bitwise syntax in the surface
   language), MemCPyOpt, loop fusion/fission/distribution, function
   outlining, Attributor-style inference, GlobalOpt (no globals —
   likely not-constructible too).
3. **Allocator follow-ups the audit motivates**: the audit now gives
   the ground truth for spill experiments — e.g. splitting the
   five-register pool's competition (live-range splitting, rematerial-
   izable constants counted as free), or coalescing report (how many
   copies the phi destruction inserts vs what survives). Any such work
   has a ready-made twenty-program benchmark + RESULTS.md baseline to
   diff against.
4. **Code quality continuation** if wanted: next-biggest files are
   `apps/debugger.hpp` (~970), `opt/interprocedural/cdfold.hpp`
   (~815), `back/vm_step.hpp` (~380 now, fine) — the support-layer +
   slim-driver treatment keeps working; the full battery + all
   scripted debugger sessions protect byte-identical behavior.
5. **A real `vs-msvc` run on actual Windows/MSVC** is still the one
   unverified step (no MSVC in the sandbox; native parity ran on g++;
   see the ckpt-16 SUMMARY "Known limits" for what was cross-verified
   back then).

## Protocol (unchanged)

README task-list ledger update → full battery green (selftest,
ctest, units, format, parity spot-checks, fuzz windows clean) →
commit → `download/21-*.zip` with fresh SUMMARY.md + CONTINUE.md →
archive with ANCHORED excludes (`./build`, `./build-ho`, `./mjz_lib`,
`*.exe`, `*.out.tmp`, `*.mbc` — never a bare `--exclude=mjz_lib`, it
eats the tracked `src/mjz_lib/`) → do NOT push origin (sandbox has no
credentials).

## Landmines hit this checkpoint (worth remembering)

- **A string_view is not a C string.** The audit's first draft printed
  row names with `%s` and ran past the fn name into the source text —
  the codegen had hit the same thing and converts to std::string for
  its banner. Print `%.*s` with the explicit length, always.
- **P.uses[] counts dead instructions' uses.** make_emit_plan computes
  the use table BEFORE the dead marking, so a pure dead Copy still
  counts a read there — the codegen skips the instruction. Any new
  consumer that wants "what is actually emitted" must walk uses with
  the dead skip, not read P.uses directly (the audit's +1 delta on the
  press program was exactly a dead Copy's read).
- **Constant programs audit as empty rows.** At default -O the
  const-call evaluator + SCCP fold a fully-constant program to one
  Print chain — `1 block | 0 vregs`. The audit honestly proves the
  folding, but audit COVERAGE wants input()-driven programs (the
  eleven first-draft programs that collapsed got inputs). The inliner
  likewise eats constant CALLS — a wide-signature pressure test needs
  a RECURSIVE callee to keep the calls real.
- **Byte-identical splits need a corpus, not vibes.** The 34-file
  .s/.mbc corpus (examples + demos + tests + fixtures + a spill-heavy
  pressure program) diffed after EACH split caught the one-line
  boundary mistakes (a stray closing brace between vm_step and
  interp_ssa) before the battery did.

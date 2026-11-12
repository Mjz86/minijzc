# CONTINUE — next steps after checkpoint 22 (loop rotation proper)

Read SUMMARY.md (same zip root) first: branchA @ one commit on top of
checkpoint 21 (`9d4d234`) — loop rotation proper (`rotate`, toggle
#33, tier 2, the LAST loop consumer: the whole non-phi header body
moves to the guard + latch-edge test, the header phis keep their keys
and args, per-position routing phis carry moved defs, exit phis
relocate the (pre_arg, latch_arg) pairs past the loop), the
selftest-caught header-increment regression (the `while(1)+break`
shape whose increment lives in the header — three phi paths routed
through the moved def's fresh header phi), the rotation fuzz bait
(runtime-clamped start + impure body, floor 15531), the below-floor
gate that keeps every older window's committed fixtures regenerating
byte-identically, and the regenerated register-audit table
(710/573/137/424 — the rotation's edge pressure, honestly measured).
This file is the resume manual.

## Setup (2 min)

```sh
cd minijzc                       # branchA @ the ckpt-22 commit
git log --oneline -4             # ckpt22 ... 9d4d234 rotate ... 51bf809 ...
git clone https://github.com/Mjz86/String mjz_lib   # inside this repo
# (NO symlinks: src/mjz_lib/alllib_import.hpp adapts to both String
#  layouts via __has_include and carries the libc++ atomic_ref shim.)
export PATH="$HOME/.venv/bin:$PATH"   # cmake 4.4 + clang-format via pip
# (if the sandbox recycled: pip install cmake clang-format)
cmake -S . -B build && cmake --build build -j4
./build/minijz --selftest                    # 207/207
ctest --test-dir build -j4                  # 44/44
./build/minijz_unit_tests                   # 6328/6328
./build/minijz --fuzz=300 --fuzz-from=15531 --fuzz-jobs=4   # the rotation window
```

## Try the checkpoint-22 features right away

```sh
# 1) the rotation — the natural while (impure body defeats every
#    earlier consumer; watch ROTATE and the exit values):
printf 'let n = input();\nlet s = 0;\nlet i = 1;\nwhile (i <= n) {\n  print i;\n  s = s + i;\n  i = i + 1;\n}\nprint s;\nprint i;\n' > p.mjz
echo 4 | ./build/minijz p.mjz --check-all
#   opt report ends "... TAILDUP 0 | ROTATE 1"; output 1 2 3 4 10 5.

# 2) the rotated shape, dumped at the moment it happens:
printf 'opt break rotate\nopt\nopt show 33\nopt continue\nvm run\ncheck\nquit\n' | \
  ./build/minijz demos/rotate_tour.mjz --debugger --stdin=demos/rotate_tour.in
#   the header keeps phis + a fall-through Br; the guard (in the
#   preheader) and the latch edge test carry the moved condition.

# 3) the header-increment regression shape (the increment lives IN
#    the header after tier-0 folds `while(1)+break`):
printf 'let s = 0;\nlet i = 0;\nwhile (1) {\n  i = i + 1;\n  if (i > 10) { break; }\n  if (i %% 2 == 1) { continue; }\n  s = s + i;\n}\nprint s;\nprint i;\n' > bc.mjz
./build/minijz bc.mjz --check-all    # 30 11 — the phi routing works

# 4) the audit table against the rotated code (the regenerated
#    numbers; the FIR filter's 8 slots are the rotation's price):
./build/minijz examples/audit/aa07_fir_filter.mjz --reg-audit --check-all
python3 scripts/regen_audit_results.py   # regenerates RESULTS.md
```

## Standing backlog (priority order)

1. **Keep widening the fuzz window**: rotation 16131+, random-cfg
   14731+ (the bait vocabulary is seed-floored: 12301 bkpt, 12331
   gvn, 12731 unswitch, 13331 compound-cond, 13631 body-if, 14231
   taildup + rt-lcf, 15531 rotation — the next flavor needs a NEW
   floor constant, and note the STRENGTHENED discipline from this
   checkpoint: a new pass joins the fuzz config only from its floor
   UP (the below-floor gate), so committed fixtures of every older
   window keep regenerating byte-identically — verify that on the
   window you are replacing). The 15531-15830 + 15831-16130
   rotation windows and random-cfg 14431-14730 + 15531-15830 ran
   clean this session.
2. **Wishlist remainder** (README task 3): the partial/guarded
   unswitch family proper (version on a loop-VARYING branch whose
   taken arm is cheap to duplicate) is the named next step now that
   rotation is DONE. After that: LCSSA (document as
   not-constructible — the IR has no undef), demanded/known bits
   (thin: no bitwise syntax in the surface language), MemCPyOpt,
   loop fusion/fission/distribution, function outlining,
   Attributor-style inference, GlobalOpt (no globals — likely
   not-constructible too).
3. **Allocator follow-ups the regenerated audit motivates**: the
   rotation moved the twenty-program baseline to 710 vregs / 137
   spill slots / 424 traffic — live-range splitting at the loop
   edges, or rematerializing the guard/latch copies' cheap constants,
   are the natural experiments, and `scripts/regen_audit_results.py`
   re-measures the whole table in one run (the audit is the
   observer, not the judge).
4. **Code quality continuation** if wanted: next-biggest files are
   `apps/debugger.hpp` (~970), `opt/interprocedural/cdfold.hpp`
   (~815) — the support-layer + slim-driver treatment keeps
   working; the full battery + all scripted debugger sessions
   protect byte-identical behavior.
5. **A real `vs-msvc` run on actual Windows/MSVC** is still the one
   unverified step (no MSVC in the sandbox; native parity ran on
   g++; see the ckpt-16 SUMMARY "Known limits" for what was
   cross-verified back then).

## Protocol (unchanged)

README task-list ledger update → full battery green (selftest,
ctest, units, format, parity spot-checks, fuzz windows clean) →
commit → `download/22-*.zip` with fresh SUMMARY.md + CONTINUE.md →
archive with ANCHORED excludes (`./build`, `./build-ho`, `./mjz_lib`,
`*.exe`, `*.out.tmp`, `*.mbc` — never a bare `--exclude=mjz_lib`, it
eats the tracked `src/mjz_lib/`) → do NOT push origin (sandbox has
no credentials).

## Landmines hit this checkpoint (worth remembering)

- **The copies must rewrite their dst.** The rotation's first
  working draft kept the ORIGINAL dst on the guard/latch copies
  while the cbrs read the fresh vregs — both copies defined the
  same vreg and the tests read never-defined values. The
  checkpoint-20 taildup landmine list already named this exact
  mistake; it repeats in any pass that clones instrs with fresh
  vregs. Rewrite dst AND reads in the same breath.
- **A moved def can BE a header phi's latch arg.** The
  `while(1)+break` programs fold so the increment lives IN the
  header — then an h-phi's latch arg is itself a moved h-body def,
  and THREE paths need routing through the moved def's fresh header
  phi: the phi's own latch arg, the latch copy's read of that phi
  (it needs the phi's NEXT-position value — exactly the phi_v
  carrier), and the exit phi's latch side (the latch copy's own
  output). The selftest caught it as an infinite loop, not a wrong
  value — moved-def routing bugs spin, they don't misprint.
- **The fuzzer's plain loops never rotate.** Constant bounds +
  constant-driven updates mean SCCP/unroll eat every natural while
  (`rotations: 0` across a whole window — the bait lesson for any
  pass that lives late in the pipeline: the earlier consumers have
  already eaten everything the plain vocabulary produces; the new
  pass needs its OWN bait shaped to survive the whole pipeline in
  front of it — runtime start + impure body here).
- **Below the floor, the fuzz config must stay the OLD pipeline.**
  The unswitch precedent's comment claimed older seeds keep their
  old config, but the code only had in-window off-cases: adding a
  pass to set_level turned it on for every seed, and the older
  windows' fixtures would drift on the next byte-verification.
  Rotation now disables below the floor in fuzz_cfg_for — verified
  by regenerating fz14231+ and fz14831+ byte-for-byte — and the next
  pass should copy that gate, not the old comment.
- **A regenerated RESULTS.md needs its claims re-checked.** The
  closing paragraph's "zero-spill programs" list was written from
  the ckpt-21 table; the rotation moved the FIR filter out of that
  set (0 -> 8 slots) and a stale claim would have shipped. Every
  headline number in a regenerated table must be re-derived from
  the actual rows (the audit lesson again: verify, never assume).

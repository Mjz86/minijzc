# Checkpoint 21 — backend modularity (the vm tripart + regalloc), the register-allocation audit, 20 natural audit programs

branchA @ (one commit on top of checkpoint 20's `51bf809`). The user
asked for two things: make the code modular, and add twenty natural
example programs to audit compiler output — especially register usage
spills. The CONTINUE backlog had already named the targets
(vm.hpp ~1044 and emit_x64_impl.hpp ~888 were the two biggest backend
files), and the audit grew exactly there: the allocator got a second,
read-only consumer. Battery all green, re-verified immediately before
the commit.

## What landed

### 1. Backend modularity — byte-identical splits

- `back/vm.hpp` (~1044) → the vm tripart: `vm.hpp` (the bytecode layer
  — Bc opcodes, bc_name/bc_rd_u32/bc_put_u32/bc_size, emit_bytecode) +
  `back/vm_step.hpp` (VmCtx + vm_init + vm_step_op + vm_run — the
  debugger's live view) + `back/interp_ssa.hpp` (run_prog, the SSA
  interpreter with its explicit frame stack). Parent includes the two
  children at the exact historical definition positions, so every
  includer keeps working unchanged.
- `back/emit_x64_impl.hpp` (~888) → `back/regalloc.hpp` (RaPlan +
  ra_plan: block liveness fixpoint, the SSA interference graph, the
  mjz_lib chordal coloring, the 5-register assignment + spill ordinals)
  + the codegen remainder. regalloc.hpp included at the same position;
  the multi-TU build still compiles it once (tu/backends.cpp).
- Verification of "byte-identical": a corpus of 17 programs (examples,
  demos, tests, two fuzz fixtures + a deliberately spill-heavy
  pressure program) emitted .s + .mbc BEFORE the splits and diffed
  after each split — 34/34 byte-identical, plus the full battery.

### 2. `--reg-audit` — the register-allocation audit

`back/regaudit.hpp` (light declaration header, the emit_x64.hpp
pattern: RegAuditRow/RegAuditOut structs + the two functions) +
`back/regaudit_impl.hpp` (the body — compiled exactly once, via the
light header in the header-only build and via tu/backends.cpp in the
multi-TU build).

The audit re-runs make_emit_plan + ra_plan per fn — the SAME plan the
codegen gets (one allocator, two consumers; it cannot drift from the
emitted asm) — and reports per fn: reachable blocks, vregs with a
home, housed (the five callee-saved registers rbx/r12-r15), spill
slots, and the spill TRAFFIC:

- reloads = one per textual use of a spilled vreg (walking uses with
  the codegen's own discipline: dead instructions skipped, fused cbrs
  read the comparison's operands, inlined consts are not uses —
  P.uses[] alone would overcount uses made by dead instructions);
- stores = one per live def of a spilled vreg + one per spilled
  incoming param (the prologue's `movq %argreg, slot`).

Exactness proven by grep against the emitted asm: the press program
reports 26 rd + 20 wr and the .s file contains exactly 26 slot reads +
20 slot writes; showcase's main reports 11 rd + 6 wr, the asm shows
11 + 6. Row names print with their OWN length — a string_view has no
NUL terminator, and the first draft's `%s` ran past the fn name into
the source text (the codegen had converted to std::string for exactly
this reason; the audit now prints `%.*s`).

Printed even under --quiet (an explicit flag is an explicit ask);
wired into cli_parse/cli_run; unit-tested (29 new checks: totals
consistency, the worst row, multi-fn rows with borrowed names, the
zero-pressure and folded-program degenerate rows, the NUL-safety).

### 3. Twenty natural audit programs (examples/audit/)

aa01..aa20 + .in tapes + RESULTS.md (the committed audit table with a
reading guide + observations). The set spans the pressure classes on
purpose: series (2 spills) / Horner (0) / eight accumulators (4) /
one-pass statistics (5) / bubble sort (18) / 3x3 matmul (8) / FIR
filter (0 — the unroller straightens the window) / Fletcher checksum
(161 vregs in one block, ZERO spills — the chordal-graph story) /
gcd+lcm (2) / fib+tribo (4) / three-seed Collatz (22) / trial division
(0, is_prime kept as a real call) / digit cipher (4) / ledger state
machine (7) / calendar (1) / unit chain (2) / Game-of-Life census (0
— array loads are consumed immediately) / interest streams (21) /
wide RECURSIVE calls (7 in the callee — the inliner eats constant
calls, so recursion is what keeps calls real) / deep recursion (0 —
two locals across a call is what the callee-saved pool preserves
free).

Every program is input()-driven where the optimizer would otherwise
fold it to a constant chain (the first draft's constant programs
audited as `1 block | 0 vregs` — the audit's own proof the folding
works, but weak coverage; the natural fix was reading the inputs,
the way real programs do). Outputs verified by hand (sum 1..60=1830,
poly(4)=959, fib(20)=6765, gcd(1071,462)=21, sum_to(40)=820, ...).

The `reg_audit_examples` ctest: all twenty through
`--check-all --reg-audit` — a runner mismatch exits nonzero (the
CHECK macro feeds the exit code), and a missing audit table is itself
a failure. Verified the gate fails when a program breaks.

### 4. Fuzz

The rotation window 14831-15130 (native parity included — fixtures
committed as fuzz_native/fz14831..14851) and 15131-15430, random-cfg
14231-14430, and the ckpt-20 bait window 14231-14530 re-verified —
all clean (0 mismatches; the bait window re-confirming 182 taildup
baits / 112 rt-lcf baits / 581 taildups).

## Battery (re-verified before the commit)

selftest 207/207 | ctest 42/42 (new reg_audit_examples) |
units 6291/6291 (+29 audit checks) | `format`/`check-format` clean |
header-only build compiled + smoke-run | the 34-file asm/bytecode
corpus byte-identical across both splits | fuzz 1-200 (selftest),
14231-14530 (re-verified), 14831-15130 (parity), 15131-15430,
random-cfg 14231-14430 — all clean.

## Known limits

- The audit measures the five-register callee-saved pool the x86-64
  backend uses; the C backend (gcc/clang register-allocate it away)
  and the VM (stack slots) are not audited — the doc says so.
- A spilled value used by a DEAD instruction correctly never reloads;
  the traffic is what the codegen emits, not a theoretical maximum.
- A real Windows/MSVC run remains the one unverified step (this
  session's sandbox had no MSVC/zig/MinGW; native parity ran on g++).
  The vm/regalloc/regaudit splits are plain-file moves + includes — no
  build/platform code touched beyond backends.cpp gaining one include.

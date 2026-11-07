# minijzc

sloppy! test compiler
uses c++20, my https://github.com/Mjz86/String (`mjz_lib/` inside it) to
build an optimizing compiler for a simple lang

This tree merges the two feature branches that grew on top of upstream
`main` (616efc7):

- **split + scctco + wider fuzzer** — the monolith cut into 24 ordered
  `src/*.hpp` parts (still a single translation unit, byte-exact
  round-trip asserted by `scripts/split_main.py`), the Branchedmain bug
  fixes folded in, the `scctco` SCC tail-call optimizer, and a much wider
  differential-fuzz vocabulary (`--fuzz-from` shards, native gate).
- **tailrec + cdfold + ifconv** — tail-call elimination with an exact
  `K::Depth` recursion budget (self recursion → loop, pure-tail SCCs →
  one sel-dispatched leaf), countdown closed forms for the emitted
  budget loops, and if-conversion of provably-binary selects — the
  pipeline that turns `print(is_odd(input()))` into
  `[depth guard] + print(input() & 1)`.

## Build & test

```sh
git clone https://github.com/Mjz86/String mjz_lib   # next to this repo
g++ -std=c++20 -O2 -I. -I mjz_lib main.cpp -o minijz

./minijz --selftest                    # demos + fuzz(200) + native gate
./minijz --fuzz=300 --fuzz-native=8    # wider sweep
```

(`-I.` resolves `mjz_lib/...` includes through the clone above; `-I mjz_lib`
covers the rest.)

The compile-time proof (optional; each check its own g++ invocation
because g++ retains each constant expression's state until end-of-TU):

```sh
for k in 1 2 3 4; do
  g++ -std=c++20 -O0 -I. -I mjz_lib -DMJZ_CX_CHECK=$k \
      -fconstexpr-depth=8192 -fconstexpr-ops-limit=4000000000 \
      -c constexpr_check.cpp -o /dev/null && echo "cx check $k: OK"
done
```

## The file split (single TU)

`main.cpp` is a thin stub: it includes the parts below in the exact
original textual order, so the project remains one translation unit.
`scripts/split_main.py` is the generator — it cuts the monolith at its
section banners and asserts the concatenation reproduces the source
byte-for-byte.

| part | contents |
| --- | --- |
| `src/00_common.hpp` | values, wrapping/trapping arithmetic |
| `src/10_lexer.hpp` … `src/31_builder.hpp` | front end: lexer, AST, parser, IR, CFG builder |
| `src/40_opt_core.hpp` … `src/42_linform.hpp` | scalar opts, analyses, mem2reg, trapelim, LICM, divmagic, reassoc, LCF, const-call |
| `src/50_callgraph.hpp` | pass manager (`PassCfg`), call graph/SCC, inliner, `optimize_prog` driver |
| `src/50b_scctco.hpp` | **scctco**: SCC tail-call collapse (runs before the inliner) |
| `src/50c_tco.hpp` | **tailrec**: tail-call elimination (runs after the inliner) |
| `src/50d_cdfold.hpp` | **cdfold**: countdown-loop closed forms |
| `src/50e_ifconv.hpp` | **ifconv**: provably-binary selects → arithmetic |
| `src/51_optimize.hpp` … `src/52_destr.hpp` | per-fn fixpoint driver, SSA destruction |
| `src/60_vm.hpp` … `src/61_astrun.hpp` | bytecode + VM, SSA interpreter, AST oracle, slot allocation |
| `src/70_program.hpp` … `src/72_emit_x64.hpp` | pipeline driver, pretty-printers, C and x86-64 backends |
| `src/80_demo.hpp` … `src/90_cli.hpp` | demos, differential fuzzer, constexpr proofs, CLI |

## Two tail-call optimizers, one pipeline

Both branches eliminated tail recursion, in two different ways; the merge
keeps **both** and orders them so each does what it is best at:

1. **`scctco` (before the inliner rounds)** collapses every all-tail SCC
   in place: each member becomes a loop with a count-down trip guard, and
   count-down residues fold to `V0 & (c-1)` with the exact 512-frame trap
   boundary.
2. **`tailrec` (after the inliner rounds)** catches what scctco declined
   (shape/size caps): self tail recursion becomes a budget loop
   (`CAP+1 - Depth()`), pure-tail SCCs merge into ONE sel-dispatched leaf
   that a second inline wave pulls into its callers.
3. **`cdfold`** then replaces such countdown loops by exact closed forms
   (`N = (u64)n0`, toggle `sel_N`, additive accumulators
   `x_N = x0 + gN + b(N·n0 − N(N−1)/2)`, guard `(u64)n0 > (u64)budget`
   jumping to the SAME trap), and **`ifconv**` folds the final
   provably-binary select into arithmetic.

The flagship result either way:

```sh
echo 7 | ./minijz --emit=c demos/tco_parity.mjz   # scctco route (default)
echo 7 | ./minijz --emit=c --disable=scctco demos/tco_parity.mjz
```

both emit `if ((uint64_t)v <= 511) … else print(v & 1)` — the guard being
semantically REQUIRED (`is_odd(600)` traps in the source language, at
exactly the activation the original 512-frame cap would refuse).
`--disable=scctco` routes the same program through tailrec → cdfold →
ifconv so each pass's contribution is visible in the report/stats.

Every transformation is exact w.r.t. the recursion-depth cap: the oracle,
the VM, both interpreters and both native backends agree bit-for-bit (the
differential fuzzer checks all of them on every program).

## Demos, tests, examples

- `demos/tco_parity.mjz` — mutual tail recursion → `input() & 1` behind
  the exact depth-cap guard.
- `demos/tco_countdown.mjz` — self tail recursion with an accumulator →
  `a + N·n0 − N(N−1)/2`.
- `demos/tco_mixed.mjz` — self + mutual recursion at multiple call sites.
- `demos/tco_parity_twice.mjz` — two call sites (cdfold/inline interplay).
- `tests/` — regression programs for the merged fixes: `t_gcd.mjz`
  (argument rotation), `t_swap.mjz` (copy fusion), `t_m3/t_m4.mjz`
  (3-/4-member SCCs), `t_bs.mjz`, `t_down.mjz`, `is_even_is_odd.mjz`
  (exact frame boundary: 511 → 1, 512 → TRAP, −3 → TRAP).
- `examples/showcase.mjz` — an input-driven optimizer tour (see
  `examples/showcase-RESULTS.md`).

Run any of them with `./minijz --run file.mjz` (verifies ALL backends
agree) or `--emit=c file.mjz` to inspect the generated code.

## Bug fixes carried by the merge

From the Branchedmain merge: AstRun empty-body retval; x86-64 spill-slot
overlap below pushed callee-saved regs; dead-param entry moves; frame
floor; `I.nt` guard-skip exploitation in the C/x86 backends; mem2reg
loop-array staleness.

From the tailrec branch: C-backend div/mod traps `exit(1)` (a trap must
kill the whole program — kept in the `is_main ? "return 1" : "exit(1)"`
form); post-tailrec re-fold gives transformed fns the full config;
slot-allocation liveness is branch-fusion aware (fuzz seed 33); asm
register allocation adds pairwise param interference (fuzz seed 804);
recursion depth cap off-by-one — 512 CALLEE activations, matching the
oracle/native convention; inliner re-folds run tier-0-only with a tiny
fixpoint budget (the level-2 re-fold was quadratic on many-site
programs); trapelim runs once per optimize() call; the fuzzer caps
fixpoint depth at 8.

Found by the merged fuzzer itself (the two vocabularies combined): the
tailrec self-loop fed a RAW-PARAMETER tail-call argument
(`gcd(b, a % b)`) back as the original entry value instead of routing it
through the collapsed loop's header phi — the same rotation bug scctco
had (and was fixed for); now `rw()`-rewritten at every latch phi, caught
at seed 40 under `--disable=scctco`.

## Validation status (this merge)

- `--selftest`: **186 checks, 0 failures** (demos + fuzz(200) + 6
  native-checked).
- Extended sweep: 300 programs, 0 mismatches, 8 native-checked
  (C + asm at −O0/−O2); scctco 781 collapses + tailrec 65 + ifconv 200
  firing together.
- Boundary: `is_odd(511) → 1`, `is_odd(512) → TRAP`, `is_odd(-3) → TRAP`
  identical across oracle/SSA/optimized/destroyed/VM/native.
- Regression: `t_gcd` (6 6 7), `t_swap` (0 7 0), showcase outputs
  identical across all runners.

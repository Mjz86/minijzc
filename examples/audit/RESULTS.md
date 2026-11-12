# examples/audit — the register-allocation audit (checkpoint 21, table refreshed at 22)

Twenty natural programs — series, Horner, bubble sort, matrix multiply,
FIR filter, checksums, gcd/lcm, fib/tribo, Collatz, trial division,
ciphers, ledgers, calendars, unit chains, Game-of-Life neighbor census,
interest schedules, wide recursive calls, deep recursion — each run
through `--check-all --reg-audit` (five runners must agree) at the
default optimization level. The table is the x86-64 register-allocation
audit: the SAME chordal-coloring plan the codegen gets (one allocator,
two consumers — the audit cannot drift from the emitted asm).

How to reproduce any row:

```sh
./build/minijz examples/audit/aa05_bubble_sort.mjz --reg-audit --check-all
```

The columns: `blocks` reachable blocks in the emitted layout,
`vregs` values with a home, `housed` the ones kept in the five
callee-saved registers (rbx, r12-r15), `spills` the -N(%rbp) slot
values, `reloads`/`stores` the spill-slot traffic (each is exactly one
`movq` in the emitted asm), `regs` the callee-saved subset pushed.

Checkpoint 22 (loop rotation proper) re-shaped these programs' optimized
IR — the while tests moved to the latch edges, one fewer control
transfer per iteration — so the table below is the audit of the
rotated code. `scripts/regen_audit_results.py` regenerates it.

## aa01_series — the low-pressure baseline: one accumulator, one counter.

```
  fn                    blocks  vregs  housed  spills  reloads  stores  regs
  <main>                      7     33      32       1        3       2     5
  totals: 7 blocks | 33 vregs | 32 housed | 1 spill slots | traffic 5 (3 rd + 2 wr) | 0/1 fns spill-free
  worst: <main> — 1 spills, 5 traffic (3 rd + 2 wr)
```

input (`aa01_series.in`): `60`

output: `1830 1830 12`


## aa02_horner — polynomial evaluation the natural way: Horner's rule with

```
  fn                    blocks  vregs  housed  spills  reloads  stores  regs
  <main>                      1      9       9       0        0       0     2
  totals: 1 blocks | 9 vregs | 9 housed | 0 spill slots | traffic 0 (0 rd + 0 wr) | 1/1 fns spill-free
```

input (`aa02_horner.in`): `4`

output: `959 77 11 5695`


## aa03_accumulators — the deliberate pressure cooker: eight rolling

```
  fn                    blocks  vregs  housed  spills  reloads  stores  regs
  <main>                      4     22      14       8       16      12     5
  totals: 4 blocks | 22 vregs | 14 housed | 8 spill slots | traffic 28 (16 rd + 12 wr) | 0/1 fns spill-free
  worst: <main> — 8 spills, 28 traffic (16 rd + 12 wr)
```

output: `2769181 549106 55756 2326`


## aa04_stats — one-pass statistics the natural way: count, sum, sum of

```
  fn                    blocks  vregs  housed  spills  reloads  stores  regs
  <main>                     13     26      21       5       11       7     5
  totals: 13 blocks | 26 vregs | 21 housed | 5 spill slots | traffic 18 (11 rd + 7 wr) | 0/1 fns spill-free
  worst: <main> — 5 spills, 18 traffic (11 rd + 7 wr)
```

output: `3 23 1 17 7 56`


## aa05_bubble_sort — the classic: array sort with swap counting. The

```
  fn                    blocks  vregs  housed  spills  reloads  stores  regs
  <main>                     31     49      26      23       56      46     5
  totals: 31 blocks | 49 vregs | 26 housed | 23 spill slots | traffic 102 (56 rd + 46 wr) | 0/1 fns spill-free
  worst: <main> — 23 spills, 102 traffic (56 rd + 46 wr)
```

output: `0 4 9 8 24`


## aa06_matmul — 3x3 matrix multiply plus the trace of the product: each

```
  fn                    blocks  vregs  housed  spills  reloads  stores  regs
  <main>                      4     51      28      23       29      23     5
  totals: 4 blocks | 51 vregs | 28 housed | 23 spill slots | traffic 52 (29 rd + 23 wr) | 0/1 fns spill-free
  worst: <main> — 23 spills, 52 traffic (29 rd + 23 wr)
```

input (`aa06_matmul.in`): `0`

output: `18 141 190`


## aa07_fir_filter — a five-tap FIR filter over a twelve-sample signal:

```
  fn                    blocks  vregs  housed  spills  reloads  stores  regs
  <main>                      9     27      19       8        9       8     5
  totals: 9 blocks | 27 vregs | 19 housed | 8 spill slots | traffic 17 (9 rd + 8 wr) | 0/1 fns spill-free
  worst: <main> — 8 spills, 17 traffic (9 rd + 8 wr)
```

input (`aa07_fir_filter.in`): `3`

output: `37 0 28 8`


## aa08_checksum — a Fletcher-16-style two rolling checksum over a data

```
  fn                    blocks  vregs  housed  spills  reloads  stores  regs
  <main>                      1    161     161       0        0       0     4
  totals: 1 blocks | 161 vregs | 161 housed | 0 spill slots | traffic 0 (0 rd + 0 wr) | 1/1 fns spill-free
```

input (`aa08_checksum.in`): `12`

output: `2 160 40962`


## aa09_gcd_lcm — Euclid over three pairs with the lcm from each gcd:

```
  fn                    blocks  vregs  housed  spills  reloads  stores  regs
  <main>                      1     12      10       2        3       2     5
  gcd                         7      6       6       0        0       0     3
  totals: 8 blocks | 18 vregs | 16 housed | 2 spill slots | traffic 5 (3 rd + 2 wr) | 1/2 fns spill-free
  worst: <main> — 2 spills, 5 traffic (3 rd + 2 wr)
```

input (`aa09_gcd_lcm.in`): `1071 462 2424`

output: `21 6 24 23562 186648 29088`


## aa10_fib_tribo — the two classic linear recurrences interleaved in one

```
  fn                    blocks  vregs  housed  spills  reloads  stores  regs
  <main>                      7     16      12       4        7       7     5
  totals: 7 blocks | 16 vregs | 12 housed | 4 spill slots | traffic 14 (7 rd + 7 wr) | 0/1 fns spill-free
  worst: <main> — 4 spills, 14 traffic (7 rd + 7 wr)
```

input (`aa10_fib_tribo.in`): `20`

output: `6765 10946 66012 121415 223317`


## aa11_collatz — three Collatz seeds scanned together: per-seed step

```
  fn                    blocks  vregs  housed  spills  reloads  stores  regs
  <main>                     51     74      52      22       36      28     5
  totals: 51 blocks | 74 vregs | 52 housed | 22 spill slots | traffic 64 (36 rd + 28 wr) | 0/1 fns spill-free
  worst: <main> — 22 spills, 64 traffic (36 rd + 28 wr)
```

output: `118 9232 305643`


## aa12_prime_trial — trial division for three candidates: each candidate's

```
  fn                    blocks  vregs  housed  spills  reloads  stores  regs
  <main>                      1      8       8       0        0       0     4
  is_prime                    8      4       4       0        0       0     3
  totals: 9 blocks | 12 vregs | 12 housed | 0 spill slots | traffic 0 (0 rd + 0 wr) | 2/2 fns spill-free
```

input (`aa12_prime_trial.in`): `7919 7917 104729`

output: `1 0 1 2`


## aa13_cipher — a digit-stream Caesar transform with carry: the incoming

```
  fn                    blocks  vregs  housed  spills  reloads  stores  regs
  <main>                      7     30      27       3        8       4     5
  totals: 7 blocks | 30 vregs | 27 housed | 3 spill slots | traffic 12 (8 rd + 4 wr) | 0/1 fns spill-free
  worst: <main> — 3 spills, 12 traffic (8 rd + 4 wr)
```

input (`aa13_cipher.in`): `6

output: `8 4 8 2 6 0 1 78`


## aa14_ledger — an account ledger state machine over an event stream

```
  fn                    blocks  vregs  housed  spills  reloads  stores  regs
  <main>                     13     24      17       7       14      14     5
  totals: 13 blocks | 24 vregs | 17 housed | 7 spill slots | traffic 28 (14 rd + 14 wr) | 0/1 fns spill-free
  worst: <main> — 7 spills, 28 traffic (14 rd + 14 wr)
```

input (`aa14_ledger.in`): `7

output: `1116 130 50 3 2`


## aa15_calendar — calendar arithmetic: days between two dates and the

```
  fn                    blocks  vregs  housed  spills  reloads  stores  regs
  <main>                      1     10      10       0        0       0     2
  days_since                 31     23      22       1        1       1     5
  totals: 32 blocks | 33 vregs | 32 housed | 1 spill slots | traffic 2 (1 rd + 1 wr) | 1/2 fns spill-free
  worst: days_since — 1 spills, 2 traffic (1 rd + 1 wr)
```

input (`aa15_calendar.in`): `2026`

output: `35503 46337 10834 4 29`


## aa16_units — a natural conversion chain: one quantity pushed through

```
  fn                    blocks  vregs  housed  spills  reloads  stores  regs
  <main>                      1     33      31       2        5       2     5
  totals: 1 blocks | 33 vregs | 31 housed | 2 spill slots | traffic 7 (5 rd + 2 wr) | 0/1 fns spill-free
  worst: <main> — 2 spills, 7 traffic (5 rd + 2 wr)
```

input (`aa16_units.in`): `2621`

output: `2621000 103188 8599 2866 1 8 80`


## aa17_life_neighbors — the Game of Life neighbor census for a 3x3 patch:

```
  fn                    blocks  vregs  housed  spills  reloads  stores  regs
  <main>                     14     14      14       0        0       0     5
  totals: 14 blocks | 14 vregs | 14 housed | 0 spill slots | traffic 0 (0 rd + 0 wr) | 1/1 fns spill-free
```

input (`aa17_life_neighbors.in`): `0`

output: `3 1 3`


## aa18_interest — a compound-interest schedule: three principal streams

```
  fn                    blocks  vregs  housed  spills  reloads  stores  regs
  <main>                      4     33      12      21       33      21     5
  totals: 4 blocks | 33 vregs | 12 housed | 21 spill slots | traffic 54 (33 rd + 21 wr) | 0/1 fns spill-free
  worst: <main> — 21 spills, 54 traffic (33 rd + 21 wr)
```

input (`aa18_interest.in`): `1000`

output: `1789 1355 780 1454 12`


## aa19_call_pressure — calls with wide signatures: a six-argument

```
  fn                    blocks  vregs  housed  spills  reloads  stores  regs
  <main>                      1      6       6       0        0       0     4
  combine                     3     22      15       7        9       7     5
  totals: 4 blocks | 28 vregs | 21 housed | 7 spill slots | traffic 16 (9 rd + 7 wr) | 1/2 fns spill-free
  worst: combine — 7 spills, 16 traffic (9 rd + 7 wr)
```

input (`aa19_call_pressure.in`): `11`

output: `290 2056 7569 201`


## aa20_recursion_depth — the natural deep-recursion pair: a plain sum_to

```
  fn                    blocks  vregs  housed  spills  reloads  stores  regs
  <main>                      1      7       7       0        0       0     4
  sum_to                      3      4       4       0        0       0     2
  weighted                    3      6       6       0        0       0     3
  totals: 7 blocks | 17 vregs | 17 housed | 0 spill slots | traffic 0 (0 rd + 0 wr) | 3/3 fns spill-free
```

input (`aa20_recursion_depth.in`): `40`

output: `820 2460 3340 1640 1010`


The twenty together: 227 blocks, 710 vregs, 573 housed in the five callee-saved registers, 137 spill slots, spill traffic 240+184 (`240` reloads + `184` stores). Zero-spill programs: the horner, the checksums, the trial division, the Life census and the deep-recursion pair stay spill-free; the rotation's guard and latch-edge copies plus the routing phis add pressure at the loop edges, so the FIR filter (spill-free at checkpoint 21) now carries eight slots — the measured price of one fewer control transfer per iteration.

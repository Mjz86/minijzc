# showcase.mjz — how the compiler does

One 40-line program, four numbers on stdin (`echo "21 14 1000003 12"`),
and every headline optimization fires. Output (identical on all seven
runners — AST oracle, SSA, optimized, destroyed, bytecode VM, emitted C,
emitted x86-64):

```
1  7  231  231  142857  4  4
```

```
$ printf '21\n14\n1000003\n12\n' | minijz showcase.mjz --run --stdin=showcase.in
  fns: 2 | ssa: 71 instrs, 3 phis | opt: 172 | destroyed: 179
  opt: folds 28 | copyprop 19 | cse 7 | dead-blk 13 | splice 33 | dce 52
       | INLINED 3 | UNROLLED 1 | LCF rt1 | DIVMAGIC 2 | TRAP-KILL 3
       | SCC-TCO 4(+1)
  backend              time(ms)  output
  interp (SSA opt)        0.012  reference
  vm (bytecode)           0.029  match
  c -O0                   1.710  match        (16.5 KB binary)
  asm -O0                 1.696  match        (16.5 KB binary)
  c -O2                   1.742  match        (16.3 KB binary)
  asm -O2                 1.760  match        (16.5 KB binary)
```

## What happened to each construct

| source construct | what the compiler turned it into |
| --- | --- |
| `is_even` / `is_odd` (mutual tail recursion) | SCC-TCO collapses the 2-member SCC into one loop, the countdown-residue analysis recognizes `n % 2`, strength-reduces it, and the whole thing folds to `print(n & 1)` behind the exact 512-frame trap guard: `if (n < 0 \|\| n >= 512) TRAP else n & 1`. `511 → 1`, `512 → TRAP`, `-3 → TRAP`. |
| `gcd(a, b)` — `return gcd(b, a % b)` | a tail call whose FIRST argument is a raw parameter. The collapse rotates both params through loop-header phis; the emitted loop is textbook Euclid: `t = a % b; a = b; b = t` plus the 511-trip guard. (This exact shape — a rotating raw param — was a real miscompile found by the new fuzzer vocabulary; see patch 0004.) |
| `sum_to(n)` — `return sum_to(n-1) + n` | NON-tail recursion: the call feeds an `+`, so no collapse, no inline (recursive fns are never inlined). It stays one honest call (`mjz_f1`), the boundary case the optimizer declines to fake. |
| `while (i <= n) { s = s + i; i = i + 1; }` | the linear-closed-form engine (`LCF rt1`) dissolves the loop into `n*(n+1)/2` computed branch-free with a shift and a mask — zero iterations at runtime. Both this and `sum_to` print 231, one via a call, one via arithmetic. |
| `d / 7`, `d % 7` | divmagic: `d * 0x924924924924924a >> 64 >> 2` with the sign fixups, and the remainder as `d - (d/7)*7` — no `idiv` anywhere in the binary. |
| `a[k] = k * 3` fill loop | unrolled and constant-folded: the emitted C stores 0, 3, 6, …, 21 directly into the arena — the loop is gone. |
| `bfind(a, lo, hi, x)` | binary search over a runtime-filled array: an array param plus two tail arms. The 4-param SCC collapses to one loop; dead-fn elimination then drops the original fn (it was inlined into main, single-use). |

## The pipeline, in numbers

- 5 functions + main in the source → **2 functions survive** (main +
  `sum_to`); everything else was collapsed, inlined and folded into main.
- SSA: 71 instrs / 3 phis → after optimization 172 (the number grows
  because 4 function bodies now live inside main) → 179 after
  destruction. 52 instrs DCE'd, 13 dead blocks removed.
- Emitted code: `showcase.c` (165 lines, 4.5 KB), `showcase.s`
  (368 lines, 6.3 KB), `showcase.mbc` (2.4 KB bytecode).
- Every backend — the two interpreters, the VM, and gcc-built C and the
  hand-written x86-64 asm at -O0 and -O2 — produces the byte-identical
  output, trap semantics included.

## Files

- `showcase.mjz` — the program (also lands in the repo as
  `examples/showcase.mjz` via patch 0008)
- `showcase.in` — the stdin tape used for every run above
- `showcase.c` / `showcase.s` / `showcase.mbc` — the emitted C, x86-64
  asm, and bytecode for that tape

## Re-running it

```
g++ -std=c++20 -I<mjz_lib-repo> -O2 -o minijzc main.cpp
cd examples
printf '21\n14\n1000003\n12\n' | ../minijzc showcase.mjz --run \
    --stdin=showcase.in --out-dir=.
# try other tapes: negative n (TRAP), n = 511 vs 512 (the exact frame
# boundary), x not in the array (bfind prints -1)
```

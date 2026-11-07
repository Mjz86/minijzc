# minijzc

sloppy! test compiler — uses C++20 and my [mjz_lib](https://github.com/Mjz86/String) graph algorithms to build an optimizing compiler for a small language (`while`/`if`/`break`/`continue`/`fn`/arrays/`input()`).

Backends: optimized SSA IR → C / x86-64 asm / a small bytecode VM, all kept bit-for-bit consistent with each other (and with the AST oracle and the SSA interpreter) by the built-in differential fuzzer.

## Build

```
git clone https://github.com/Mjz86/String mjz_lib   # the graph library
g++ -std=c++20 -O2 -I mjz_lib main.cpp -o minijz
```

The compiler is still a **single translation unit**: `main.cpp` includes `src/minijz.hpp`, which includes the ordered section headers — the original monolith, cut at its section banners, declaration order preserved bit for bit.

```
main.cpp               the tool: CLI + main()  (§17)
src/minijz.hpp         ordered umbrella — include this one
src/minijz_common.hpp      doc, includes, macros, wrapping/trapping arithmetic
src/minijz_front.hpp       lexer / AST / parser
src/minijz_ir.hpp          IR + CFG builder
src/minijz_ssa.hpp         analyses (mjz::graph_ns) + SSA construction
src/minijz_opt_scalar.hpp  scalar passes, alias + range analysis, mem2reg
src/minijz_opt_tailrec.hpp const-call, tail-call elimination, countdown closed
                            forms, if-conversion
src/minijz_opt_manager.hpp pass manager (tiers, fixpoints, optimize())
src/minijz_destroy.hpp     SSA destruction
src/minijz_bc.hpp          bytecode emitter
src/minijz_interp.hpp      SSA interpreter + AST interpreter (the oracle)
src/minijz_slots.hpp       slot allocation (C + bytecode backends)
src/minijz_pipeline.hpp    compile() driver
src/minijz_print.hpp       pretty-printers
src/minijz_backend.hpp     C / x86-64 asm backends, emission plan, regalloc
src/minijz_demos.hpp       curated demo suite (--demo, --selftest)
src/minijz_fuzz.hpp        differential fuzzer (--fuzz)
src/minijz_constexpr.hpp   the constexpr proof programs
constexpr_check.cpp    optional proof TU (see below)
```

Headers are ordered-include slices: rely on `src/minijz.hpp`'s order, do not include them in arbitrary order.

## The compile-time proof (optional)

The whole pipeline — parse → SSA → optimize → destroy → bytecode → VM — is `constexpr`. `constexpr_check.cpp` `static_assert`s four full programs (arena arrays; recursion/inlining/const-call/tail-call; unrolling/closed forms/divmagic; branchy unrolling + short-circuit). g++ retains each constant expression's evaluation state until the end of the translation unit, so with the tail-call pipeline the proofs no longer all fit one 4 GiB compile — build each as its own invocation:

```
for k in 1 2 3 4; do
  g++ -std=c++20 -O0 -I. -I mjz_lib -DMJZ_CX_CHECK=$k \
      -fconstexpr-depth=8192 -fconstexpr-ops-limit=4000000000 \
      -c constexpr_check.cpp -o /dev/null && echo "cx check $k: OK"
done
```

## Quick tour

```
./minijz --demo                 # curated programs, all backends
./minijz --selftest             # demos + differential fuzz battery
./minijz --run program.mjz      # compile, run everywhere, verify identical
./minijz --fuzz=1000 --fuzz-native=40   # random-program correctness gate
./minijz --help
```

Try the tail-call pipeline end to end — mutual recursion folds to `input() & 1` (with the exact depth-cap guard) in the emitted C:

```
fn pe(n) { if (n == 0) { return 1; } return po(n - 1); }
fn po(n) { if (n == 0) { return 0; } return pe(n - 1); }
print po(input());
```

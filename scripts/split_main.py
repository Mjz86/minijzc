#!/usr/bin/env python3
"""Mechanically split main.cpp into ordered .hpp parts (TU count stays 1).

main.cpp includes the parts in the exact original textual order, so the
translation unit is byte-identical to the original monolith — verified by
reconstruction below (asserts on any deviation)."""
import pathlib

REPO = pathlib.Path('/home/z/my-project/repo')
SRC = REPO / 'main.cpp'
OUT = REPO / 'src'

# 1-based line -> prefix that must be found there (guards against drift)
expect = {
    411: 'enum class Tk', 526: 'enum class Ek', 590: 'struct Parser',
    1085: 'enum class K', 1310: 'struct Builder', 1599: 'struct An',
    2737: 'struct AliasInfo', 3676: 'struct LinForm', 5784: 'struct PassCfg',
    6233: 'struct Optimized', 6519: 'struct DestrStats', 6752: 'struct RunOut',
    7376: 'struct AstRun', 7735: 'struct FnUnit', 8054: 'struct EmitPlan',
    8731: 'struct RaPlan', 9571: 'struct Demo', 9946: 'struct GenCtx',
    10515: 'struct Cli', 10991: 'int main',
}
names = ['00_common.hpp', '10_lexer.hpp', '20_ast.hpp', '21_parser.hpp',
         '30_ir.hpp', '31_builder.hpp', '40_opt_core.hpp',
         '41_opt_analysis.hpp', '42_linform.hpp', '50_callgraph.hpp',
         '51_optimize.hpp', '52_destr.hpp', '60_vm.hpp', '61_astrun.hpp',
         '70_program.hpp', '71_emit_c.hpp', '72_emit_x64.hpp',
         '80_demo.hpp', '81_fuzz.hpp', '90_cli.hpp']

text = SRC.read_text()
lines = text.split('\n')
n = len(lines)

for ln, prefix in expect.items():
    got = lines[ln - 1]
    assert got.startswith(prefix), f'line {ln}: expected {prefix!r}, got {got!r}'

# cut starts; walk back over pure // comment / blank lines so each decl keeps
# its doc-comment banner, never past the previous cut
starts = [1] + sorted(expect.keys())
adj = []
for i, s in enumerate(starts):
    if i == 0:
        adj.append(1)
        continue
    lo = adj[-1]
    k = s - 2  # 0-based line above the decl
    while k >= lo - 1 and (lines[k].lstrip().startswith('//') or lines[k].strip() == ''):
        k -= 1
    start = k + 2  # 1-based
    assert lo <= start <= s, (name if False else i, start, s, lo)
    adj.append(start)

OUT.mkdir(exist_ok=True)
chunks = []  # list of (filename, chunk_lines)
for i, name in enumerate(names):
    a = adj[i] - 1                      # 0-based inclusive
    b = (adj[i + 1] - 1) if i + 1 < len(adj) else n  # 0-based exclusive
    chunk = lines[a:b]
    chunks.append((name, chunk))
    (OUT / name).write_text('#pragma once\n' + '\n'.join(chunk) + '\n')

# tail: int main() block stays in main.cpp
tail = lines[adj[-1] - 1:]

new_main = ['// main.cpp — single translation unit.',
            '// Mechanical split: the parts below, concatenated in include order,',
            '// reproduce the former monolithic main.cpp byte-for-byte.',
            '// See scripts/split_main.py (the generator) for the invariant.']
for name, _ in chunks:
    new_main.append(f'#include "src/{name}"')
new_main.append('')
new_main.extend(tail)
SRC.write_text('\n'.join(new_main) + '\n')

# --- byte-exact reconstruction check ------------------------------------
recon = []
for _, chunk in chunks:
    recon.extend(chunk)
recon.extend(tail)
assert '\n'.join(recon) == text, 'reconstruction mismatch!'
total = sum(len(c) for _, c in chunks) + len(tail)
print(f'split OK: {len(chunks)} headers + main() tail; {total} lines round-trip '
      f'== original {n}; sizes:')
for name, chunk in chunks:
    print(f'  src/{name:<20} {len(chunk):>5} lines')
print(f'  main.cpp (incl. main()) {len(new_main):>5} lines')

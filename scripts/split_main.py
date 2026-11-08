# DEPRECATED (2026-11): the mechanical monolith split was superseded by the
# src/minijz/ tree (see git log a46e361). Kept for history; do not run.
#!/usr/bin/env python3
"""Mechanically split the monolithic compiler into ordered .hpp parts.

The project stays a SINGLE translation unit: main.cpp includes the parts in
the exact original textual order, so the concatenation of the parts (plus
the main() tail) reproduces the monolith byte-for-byte — verified by the
reconstruction assert at the bottom.

Usage:  python3 scripts/split_main.py <monolith.cpp>   (writes src/ + main.cpp
into the directory containing the monolith; run from a scratch copy).
"""
import pathlib
import sys

# (anchor line prefix, output name). Each cut starts at the anchor and walks
# BACK over pure // comment / blank lines so a decl keeps its doc-comment
# banner, never past the previous cut.
PARTS = [
    ('enum class Tk', '10_lexer.hpp'),
    ('enum class Ek', '20_ast.hpp'),
    ('struct Parser', '21_parser.hpp'),
    ('enum class K', '30_ir.hpp'),
    ('struct Builder', '31_builder.hpp'),
    ('struct An {', '40_opt_core.hpp'),
    ('struct AliasInfo', '41_opt_analysis.hpp'),
    ('struct LinForm', '42_linform.hpp'),
    ('struct CallGraph', '50_callgraph.hpp'),
    ('constexpr uint32_t scctco_uses', '50b_scctco.hpp'),
    ('constexpr uint32_t MJZ_TCE_MAX_MEMBERS', '50c_tco.hpp'),
    ('struct CdAff', '50d_cdfold.hpp'),
    ('constexpr bool opt_ifconv', '50e_ifconv.hpp'),
    ('struct Optimized', '51_optimize.hpp'),
    ('struct DestrStats', '52_destr.hpp'),
    ('struct RunOut', '60_vm.hpp'),
    ('struct AstRun', '61_astrun.hpp'),
    ('struct FnUnit', '70_program.hpp'),
    ('struct EmitPlan', '71_emit_c.hpp'),
    ('struct RaPlan', '72_emit_x64.hpp'),
    ('struct Demo {', '80_demo.hpp'),
    ('struct GenCtx', '81_fuzz.hpp'),
    ('struct Cli {', '90_cli.hpp'),
]
NAMES = ['00_common.hpp'] + [n for _, n in PARTS]

mono_path = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else 'main.cpp')
root = mono_path.parent
text = mono_path.read_text()
lines = text.split('\n')
n = len(lines)

# locate every anchor (first line that starts with the prefix)
starts = [1]  # 1-based; part 0 = 00_common.hpp
for prefix, _ in PARTS:
    hit = next((i + 1 for i, l in enumerate(lines)
                if l.startswith(prefix)), None)
    assert hit, f'anchor not found: {prefix!r}'
    assert hit >= starts[-1], f'anchor out of order: {prefix!r}'
    starts.append(hit)

# walk each cut start back over comment/blank lines (never past previous cut)
adj = [1]
for s in starts[1:]:
    lo = adj[-1]
    k = s - 2  # 0-based line above the anchor
    while k >= lo - 1 and (lines[k].lstrip().startswith('//')
                           or lines[k].strip() == ''):
        k -= 1
    start = k + 2  # 1-based
    assert lo <= start <= s, (start, s, lo)
    adj.append(start)

# the main() tail stays in main.cpp
mi = next(i for i, l in enumerate(lines) if l.startswith('int main('))
tail = lines[mi:]

out = root / 'src'
out.mkdir(exist_ok=True)
chunks = []
for i, name in enumerate(NAMES):
    a = adj[i] - 1                    # 0-based inclusive
    b = (adj[i + 1] - 1) if i + 1 < len(adj) else mi  # exclusive
    chunk = lines[a:b]
    chunks.append((name, chunk))
    # exactly pragma + chunk: a chunk that does not end with a blank line
    # yields a file with no trailing newline -- that is what keeps the
    # file-level round-trip byte-exact
    (out / name).write_text('#pragma once\n' + '\n'.join(chunk))

new_main = ['// main.cpp — single translation unit.',
            '// Mechanical split: the parts below, concatenated in include order',
            '// plus the main() tail, reproduce the monolithic compiler',
            '// byte-for-byte. See scripts/split_main.py (the generator).']
for name, _ in chunks:
    new_main.append(f'#include "src/{name}"')
new_main.append('')
new_main.extend(tail)
(root / 'main.cpp').write_text('\n'.join(new_main))  # tail carries the
# monolith's own trailing newline (or lack of one): add nothing

# --- byte-exact reconstruction check (reads the WRITTEN files back) ---------
recon = []
for name, _ in chunks:
    flines = (out / name).read_text().split('\n')
    assert flines[0] == '#pragma once', name
    recon.extend(flines[1:])
recon.extend(tail)
assert '\n'.join(recon) == text, 'reconstruction mismatch!'
total = sum(len(c) for _, c in chunks) + len(tail)
print(f'split OK: {len(chunks)} headers + main() tail; {total} lines '
      f'round-trip == original {n}; sizes:')
for name, chunk in chunks:
    print(f'  src/{name:<20} {len(chunk):>5} lines')
print(f'  main.cpp (incl. main()) {len(new_main):>5} lines')

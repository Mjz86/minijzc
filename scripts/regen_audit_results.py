#!/usr/bin/env python3
# regen_audit_results.py — regenerate examples/audit/RESULTS.md from the
# current compiler (checkpoint 22: loop rotation changed the optimized IR
# of the audit programs, so the committed table needed a refresh).
#
# The per-program PROSE headers (the "## aaNN — ..." descriptions) are
# preserved verbatim from the existing file; the tables, the output lines
# and the headline aggregates are rebuilt from fresh --reg-audit runs.
# The input lines are preserved too (the tapes do not change).
#
# Usage:  python3 scripts/regen_audit_results.py  (from the repo root)

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
AUD = ROOT / "examples" / "audit"
BIN = ROOT / "build" / "minijz"
RESULTS = AUD / "RESULTS.md"

INTRO = """# examples/audit — the register-allocation audit (checkpoint 21, table refreshed at 22)

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
"""


def run_one(mjz: Path):
    stem = mjz.stem
    tape = AUD / (stem + ".in")
    cmd = [str(BIN), str(mjz), "--reg-audit", "--check-all", "--quiet"]
    if tape.exists():
        cmd.append("--stdin=" + str(tape))
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    if p.returncode != 0:
        print("FAILED %s:\n%s" % (stem, p.stdout + p.stderr), file=sys.stderr)
        sys.exit(1)
    return p.stdout


def parse_table(out: str):
    """extract the audit block: header line .. worst line, plus the output"""
    lines = out.splitlines()
    t0 = None
    t1 = None
    for i, ln in enumerate(lines):
        if ln.startswith("  fn ") and t0 is None:
            t0 = i
        if t0 is not None and (ln.startswith("  totals:")):
            t1 = i  # the table runs to totals (worst only exists when
            # something spills — spill-free rows have no worst line)
            break
    if t0 is None or t1 is None:
        print("no audit table in output:\n" + out, file=sys.stderr)
        sys.exit(1)
    table = lines[t0 : t1 + 1]
    # keep the worst line too (present only when something spills): the
    # line right after totals, when it starts with '  worst:'
    if t1 + 1 < len(lines) and lines[t1 + 1].startswith("  worst:"):
        table.append(lines[t1 + 1])
    # the program output: the LAST "oracle/ssa/lower/vm" line's value part
    out_line = ""
    for ln in lines[t1 + 1 :]:
        if ":" in ln and ln.split(":")[0].strip() in ("oracle", "ssa",
                                                      "lower", "vm"):
            out_line = ln.split(":", 1)[1].strip()
    if not out_line:
        print("no output line found:\n" + out, file=sys.stderr)
        sys.exit(1)
    return table, out_line


def parse_old_sections(text: str):
    """split the existing RESULTS.md into (header-prose, input-line) pairs"""
    secs = {}
    parts = re.split(r"(?=^## aa\d+_)", text, flags=re.M)
    for part in parts:
        m = re.match(r"^## (aa\d+_[a-z0-9_]+)", part)
        if not m:
            continue
        name = m.group(1)
        # the prose: from '##' up to the opening code fence
        prose = part[: part.index("```")].rstrip() + "\n"
        # the input line (unchanged tapes): the 'input (...): ...' line
        im = re.search(r"^input \(.*\): .*$", part, flags=re.M)
        secs[name] = (prose, im.group(0) if im else None)
    return secs


def main():
    old = RESULTS.read_text()
    secs = parse_old_sections(old)
    progs = sorted(AUD.glob("aa*.mjz"))
    if len(progs) != 20:
        print("expected 20 audit programs, found %d" % len(progs), file=sys.stderr)
        sys.exit(1)

    body = [INTRO]
    tot = {"blocks": 0, "vregs": 0, "housed": 0, "spills": 0, "rd": 0, "wr": 0}
    for mjz in progs:
        stem = mjz.stem
        out = run_one(mjz)
        table, out_line = parse_table(out)
        if stem not in secs:
            print("missing prose for %s" % stem, file=sys.stderr)
            sys.exit(1)
        prose, input_line = secs[stem]
        body.append(prose)
        body.append("```\n" + "\n".join(table) + "\n```\n")
        if input_line:
            body.append(input_line + "\n")
        body.append("output: `%s`\n\n" % out_line)
        # aggregate from the totals line
        tm = None
        for ln in table:
            if ln.startswith("  totals:"):
                tm = ln
                break
        if tm:
            tot["blocks"] += int(re.search(r"(\d+) blocks", tm).group(1))
            tot["vregs"] += int(re.search(r"(\d+) vregs", tm).group(1))
            tot["housed"] += int(re.search(r"(\d+) housed", tm).group(1))
            tot["spills"] += int(re.search(r"(\d+) spill slots", tm).group(1))
            t = re.search(r"traffic \d+ \((\d+) rd \+ (\d+) wr\)", tm)
            tot["rd"] += int(t.group(1))
            tot["wr"] += int(t.group(2))

    traffic = tot["rd"] + tot["wr"]
    body.append(
        "The twenty together: %d blocks, %d vregs, %d housed in the five "
        "callee-saved registers, %d spill slots, spill traffic %d+%d "
        "(`%d` reloads + `%d` stores). Zero-spill programs: the horner, "
        "the checksums, the trial division, the Life census and the "
        "deep-recursion pair stay spill-free; the rotation's guard and "
        "latch-edge copies plus the routing phis add pressure at the "
        "loop edges, so the FIR filter (spill-free at checkpoint 21) now "
        "carries eight slots — the measured price of one fewer control "
        "transfer per iteration.\n"
        % (tot["blocks"], tot["vregs"], tot["housed"], tot["spills"],
           tot["rd"], tot["wr"], tot["rd"], tot["wr"]))
    RESULTS.write_text("\n".join(body))
    print("totals: %r" % tot)


if __name__ == "__main__":
    main()

#!/usr/bin/env bash
# include_portability_test.sh -- keep every quoted #include portable.
#
# A quoted include must resolve from the INCLUDER'S OWN directory (search
# rule #1). Anything that only resolves via -I dirs or the MSVC include-stack
# rule is what clang-cl flags as -Wmicrosoft-include -- the 215-warning class
# from warnings.txt, eliminated tree-wide (see warnings.txt's triage ledger).
# This gate keeps the property sticky: a new header that quietly reintroduces
# -I-dependent includes fails the ctest suite (label 'regr').
#
# Pure-bash on purpose: no per-line subprocesses (an earlier printf|sed-per-
# line version timed out under ctest -j load).
set -u
cd "$(dirname "$0")/.." || exit 2

total=0
fail=0
while IFS= read -r -d '' f; do
  dir=${f%/*}
  while IFS= read -r line; do
    [[ $line =~ ^[[:space:]]*\#[[:space:]]*include[[:space:]]*\"([^\"]*)\" ]] || continue
    inc=${BASH_REMATCH[1]}
    total=$((total + 1))
    if [[ ! -e $dir/$inc ]]; then
      echo "NOT PORTABLE: $f: $line"
      fail=$((fail + 1))
    fi
  done < "$f"
done < <(find src tests \( -path 'src/mjz_lib' -o -path 'src/mjz_lib/*' \) -prune -o \
         \( -name '*.hpp' -o -name '*.cpp' \) -print0; \
         find . -maxdepth 1 -name 'main.cpp' -print0)
# (src/mjz_lib/ is the layout-adaptive String bridge: its conditional first
# branch exists only under the old clone layout -- exempt by design.)

if [ "$fail" -ne 0 ]; then
  echo "portable includes: $((total - fail))/$total OK"
  exit 1
fi
echo "portable includes: $total/$total OK (every quoted include resolves in its includer's directory)"

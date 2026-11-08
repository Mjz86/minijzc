#!/usr/bin/env bash
# header_order_test.sh — prove every src/minijz/**/*.hpp header is
# self-contained (include-order independent, README task): each is compiled
# as the ONLY include of its own TU. The umbrella src/minijz.hpp is checked
# the same way. Run from repo root (or via ctest).
set -u
cd "$(dirname "$0")/.."
JOBS=${JOBS:-2}   # the sandbox OOMs under wide g++ parallelism
mkdir -p build/hdrtest
HDRS=$(cd src/minijz && find . -name "*.hpp" | sed 's|^\./||' | sort)
{
  echo '.PHONY: all'
  echo 'all:'
  for h in $HDRS; do
    n=$(echo "$h" | tr '/.' '__')
    echo "all: tu_$n.chk"
  done
  echo 'all: umbrella.chk'
  for h in $HDRS; do
    n=$(echo "$h" | tr '/.' '__')
    cat > "build/hdrtest/tu_$n.cpp" <<EOF
#include "minijz/$h"
int main() { return 0; }
EOF
    echo "tu_$n.chk: tu_$n.cpp"
    echo -e "\t@g++ -std=c++20 -O0 -I../../src -I../../mjz_lib -fsyntax-only tu_$n.cpp && touch tu_$n.chk || echo 'ORDER-FAIL: minijz/$h'"
  done
  cat > "build/hdrtest/umbrella.cpp" <<'EOF'
#include "minijz.hpp"
int main() { return 0; }
EOF
  echo "umbrella.chk: umbrella.cpp"
  echo -e "\t@g++ -std=c++20 -O0 -I../../src -I../../mjz_lib -fsyntax-only umbrella.cpp && touch umbrella.chk || echo 'ORDER-FAIL: minijz.hpp'"
} > build/hdrtest/Makefile
make -C build/hdrtest -j"$JOBS" 2>&1 | grep -E "ORDER-FAIL|error:" | head -60
n_fail=$(make -C build/hdrtest -j"$JOBS" 2>/dev/null | grep -c ORDER-FAIL || true)
n_hdrs=$(echo "$HDRS" | wc -l)
echo "standalone header compile: $n_fail failures of $n_hdrs headers + umbrella"
if [ "$n_fail" = "0" ]; then echo "ALL HEADERS SELF-CONTAINED"; exit 0; else exit 1; fi

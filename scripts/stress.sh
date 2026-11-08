#!/usr/bin/env bash
# stress.sh — hand-written differential stress battery for minijz.
# Each case: compile with --check-all at several -O configs and count
# MISMATCH lines. Run from repo root (or via ctest, which passes
# MINIJZ_BIN pointing at the built binary).
cd "$(dirname "$0")/.."
BIN=${MINIJZ_BIN:-./minijz}
FAIL=0
run_case() { # name, src-file, inputs...
  local name=$1 src=$2; shift 2
  local inp="$*"
  for cfg in "-O3" "-O3 --disable=scctco" "-O3 --disable=tailrec" "-O1" "-O2 --enable=scctco" "-O3 --disable=divmagic" "-O3 --disable=unroll" "-O0"; do
    out=$(echo "$inp" | timeout 120 $BIN "$src" --check-all $cfg 2>&1)
    if echo "$out" | grep -q "MISMATCH"; then
      echo "FAIL $name cfg='$cfg'"
      echo "$out" | grep -E "^(oracle|lower|vm) " | head -3
      FAIL=1
    fi
  done
}

# --- recursion rings ------------------------------------------------------
cat > /tmp/stress_ring3p2.mjz <<'EOF'
fn a(x, y) { if (x <= 0) { return y; } return b(x - 1, y + 2); }
fn b(x, y) { if (x <= 0) { return y + 1; } return c(x - 1, y - 2); }
fn c(x, y) { if (x <= 0) { return y + 2; } return a(x - 1, y * 2); }
print a(input(), input());
print b(input(), 0);
print c(input(), 5);
EOF
run_case ring3p2 /tmp/stress_ring3p2.mjz 7 3 6 9

cat > /tmp/stress_ring5multi.mjz <<'EOF'
fn f0(n, acc) { if (n == 0) { return acc; } if (n % 5 == 1) { return f1(n / 5, acc + 1); } return f0(n / 5, acc + 2); }
fn f1(n, acc) { if (n == 0) { return acc + 10; } if (n % 5 == 2) { return f2(n / 5, acc + 3); } return f0(n / 5, acc + 4); }
fn f2(n, acc) { if (n == 0) { return acc + 20; } if (n % 5 == 3) { return f3(n / 5, acc + 5); } return f4(n / 5, acc + 6); }
fn f3(n, acc) { if (n == 0) { return acc + 30; } return f2(n / 5, acc + 7); }
fn f4(n, acc) { if (n == 0) { return acc + 40; } return f3(n / 5, acc + 8); }
print f0(input(), 0);
print f1(input(), 100);
print f2(input(), 200);
print f3(input(), 300);
print f4(input(), 400);
EOF
run_case ring5multi /tmp/stress_ring5multi.mjz 12345 9876 555 3125 99999

# --- gcd-style arg rotation -----------------------------------------------
cat > /tmp/stress_gcd.mjz <<'EOF'
fn gcd(a, b) { if (b == 0) { return a; } return gcd(b, a % b); }
fn fib(n) { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); }
print gcd(input(), input());
print gcd(input(), input());
print fib(input());
EOF
run_case gcd /tmp/stress_gcd.mjz 1071 462 270 192 15

# --- print side effects inside recursion ----------------------------------
cat > /tmp/stress_printrec.mjz <<'EOF'
fn pe(n) { if (n <= 0) { print 11; return 1; } return po(n - 1); }
fn po(n) { if (n <= 0) { print 22; return 0; } return pe(n - 1); }
fn qe(n) { print 33; if (n <= 0) { return 1; } return qo(n - 1); }
fn qo(n) { print 44; if (n <= 0) { return 0; } return qe(n - 1); }
print pe(input());
print po(input());
print qe(input());
print qo(input());
EOF
run_case printrec /tmp/stress_printrec.mjz 4 5 6 7

# --- div/mod semantics (floor/trunc, negatives, INT64_MIN-ish) -------------
cat > /tmp/stress_divmod.mjz <<'EOF'
let a = input();
let b = input();
if (b != 0) { print a / b; print a % b; }
let c = input();
if (c != 0) { print (0 - a) / c; print (0 - a) % c; }
let d = input();
print d / 4; print d % 4; print d / -4; print d % -4;
print d / 1000000007; print d % 1000000007;
print d / 2; print d % 2; print d / 3; print d % 3;
print (0 - 9223372036854775807 - 1) / d;
print (0 - 9223372036854775807 - 1) % d;
EOF
run_case divmod /tmp/stress_divmod.mjz 100 -7 -3 4

cat > /tmp/stress_divmod2.mjz <<'EOF'
let d = input();
print d / 5; print d % 5;
print d / -5; print d % -5;
print d / 7; print d % 7;
print d / 12; print d % 12;
print d / 1024; print d % 1024;
print d / 65536; print d % 65536;
print d / 1000000000000000000;
print d / -1000000000000000000;
EOF
run_case divmod2 /tmp/stress_divmod2.mjz 123456789
run_case divmod2neg /tmp/stress_divmod2.mjz -987654321

# --- not chains over all ops at boundaries --------------------------------
cat > /tmp/stress_notch.mjz <<'EOF'
let x = input();
let y = input();
print !(!(x < y)); print !(!(x <= y)); print !(!(x > y)); print !(!(x >= y));
print !(!(!(x < y))); print !(!(!(x <= y))); print !(!(!(x > y))); print !(!(!(x >= y)));
print !(x < y); print !(x <= y); print !(x > y); print !(x >= y);
print !(x == y); print !(x != y); print !(!(x == y)); print !(!(x != y));
if (!(x < y)) { print 1; } else { print 2; }
if (!(!(x <= y))) { print 3; } else { print 4; }
while (!(x >= y)) { x = x + 1; }
print x;
EOF
run_case notch /tmp/stress_notch.mjz 5 5
run_case notch2 /tmp/stress_notch.mjz 5 6
run_case notch3 /tmp/stress_notch.mjz -6 5

# --- loops with continue/break inside collapsed regions --------------------
cat > /tmp/stress_loopbreak.mjz <<'EOF'
fn collatz(n) { if (n == 1) { return 0; } if (n % 2 == 0) { return collatz(n / 2); } return collatz(3 * n + 1); }
let n = input();
print collatz(n);
let i = 0;
while (i < n) {
  i = i + 1;
  if (i % 3 == 0) { continue; }
  if (i > 10) { break; }
  print i;
}
print i;
EOF
run_case loopbreak /tmp/stress_loopbreak.mjz 27
run_case loopbreak2 /tmp/stress_loopbreak.mjz 7

# --- arrays + recursion ----------------------------------------------------
cat > /tmp/stress_arrrec.mjz <<'EOF'
fn sum(a, i) { if (i < 0) { return 0; } return a[i] + sum(a, i - 1); }
fn fill(a, i, v) { if (i < 0) { return a; } a[i] = v + i; return fill(a, i - 1, v); }
let a = array(10);
a = fill(a, 9, input());
print sum(a, 9);
print len(a);
print a[input()];
EOF
run_case arrrec /tmp/stress_arrrec.mjz 100 5

if [ $FAIL -eq 0 ]; then echo "ALL STRESS CASES PASS"; else echo "STRESS FAILURES FOUND"; fi
exit $FAIL

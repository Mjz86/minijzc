#!/usr/bin/env bash
# parity_diff.sh — the byte-identical differential harness (the
# code-quality continuation's standing gate). Runs OLD and NEW
# minijz binaries over the whole committed program corpus with
# their stdin tapes, three flag variants each, plus the scripted
# debugger sessions and --demo, and diffs every output.
#
# Usage: bash scripts/parity_diff.sh /path/to/old/minijz /path/to/new/minijz
# Exit 0 + "PARITY OK (N runs)" = every diff byte-identical.

set -u
OLD=${1:?old binary}
NEW=${2:?new binary}
test -x "$OLD" || { echo "old binary not executable: $OLD"; exit 2; }
test -x "$NEW" || { echo "new binary not executable: $NEW"; exit 2; }
cd "$(dirname "$0")/.."

runs=0
fails=0

run_one() { # $1=bin $2=out-prefix $3..=argv (stdin via redirect by caller)
  local bin=$1 pfx=$2; shift 2
  "$bin" "$@" > "$pfx.out" 2> "$pfx.err" < /dev/null
  echo $? > "$pfx.rc"
}

cmp_one() { # $1=label ; compares the three capture files
  local label=$1
  runs=$((runs + 1))
  if ! cmp -s /tmp/pd.old.out /tmp/pd.new.out || \
     ! cmp -s /tmp/pd.old.err /tmp/pd.new.err || \
     ! cmp -s /tmp/pd.old.rc /tmp/pd.new.rc; then
    fails=$((fails + 1))
    echo "MISMATCH: $label"
    echo "  rc: old=$(cat /tmp/pd.old.rc) new=$(cat /tmp/pd.new.rc)"
    diff /tmp/pd.old.out /tmp/pd.new.out | head -5
    diff /tmp/pd.old.err /tmp/pd.new.err | head -5
  fi
}

# ---- the program corpus, stdin-aware --------------------------------------
for f in examples/*.mjz examples/audit/*.mjz demos/*.mjz tests/*.mjz \
         bugrepro/*.mjz; do
  base=${f%.mjz}
  tape=""
  [ -f "$base.in" ] && tape="$base.in"
  for variant in default O3 checkall; do
    case $variant in
      default)  flags=() ;;
      O3)       flags=(-O3) ;;
      checkall) flags=(--check-all) ;;
    esac
    for side in old new; do
      bin=$OLD; [ $side = new ] && bin=$NEW
      if [ -n "$tape" ]; then
        run_one "$bin" /tmp/pd.$side "$f" "${flags[@]}" --stdin="$tape"
      else
        run_one "$bin" /tmp/pd.$side "$f" "${flags[@]}"
      fi
    done
    cmp_one "$f [$variant]"
  done
done

# ---- the scripted debugger sessions ----------------------------------------
# (the ctest debugger_session shape, plus a tour with watchpoints)
for f in demos/bkpt_tour.mjz demos/bif_tour.mjz demos/rotate_tour.mjz; do
  base=${f%.mjz}
  tape=""
  [ -f "$base.in" ] && tape="$base.in"
  cmds='fns\nvm reset\nvm step 5\nvm state\nvm run\ncheck\nopt fold\nquit\n'
  for side in old new; do
    bin=$OLD; [ $side = new ] && bin=$NEW
    if [ -n "$tape" ]; then
      printf "$cmds" | "$bin" "$f" --debugger --stdin="$tape" \
        > /tmp/pd.$side.out 2> /tmp/pd.$side.err
    else
      printf "$cmds" | "$bin" "$f" --debugger \
        > /tmp/pd.$side.out 2> /tmp/pd.$side.err
    fi
    echo $? > /tmp/pd.$side.rc
  done
  cmp_one "$f [debugger tour]"
done

# ---- --demo -----------------------------------------------------------------
for side in old new; do
  bin=$OLD; [ $side = new ] && bin=$NEW
  run_one "$bin" /tmp/pd.$side --demo
done
cmp_one "--demo"

# ---- the pass-statistics report (the counters are observable) ---------------
for side in old new; do
  bin=$OLD; [ $side = new ] && bin=$NEW
  "$bin" examples/general.mjz --stdin=examples/general.in -O3 \
    > /tmp/pd.$side.out 2> /tmp/pd.$side.err < /dev/null
  echo $? > /tmp/pd.$side.rc
done
cmp_one "general -O3 report"

echo "parity: $fails mismatches of $runs runs"
if [ "$fails" = "0" ]; then echo "PARITY OK ($runs runs)"; exit 0; fi
exit 1

#!/usr/bin/env bash
# fuzz_parity.sh — differential fuzz old-vs-new over the committed
# windows, compared modulo timing lines (the [fuzz k/n] progress lines
# and the wall summary). House gate from the ckpt 23-27 protocol.
set -u
OLD=${1:?old binary}
NEW=${2:?new binary}
FROM=${3:?start seed}
TO=${4:?end seed (inclusive)}
cd "$(dirname "$0")/.."
N=$((TO - FROM + 1))
for side in old new; do
  bin=$OLD; [ $side = new ] && bin=$NEW
  "$bin" --fuzz=$N --fuzz-from=$FROM --fuzz-jobs=2 \
    > /tmp/fz.$side.out 2> /tmp/fz.$side.err
  echo "rc=$?" >> /tmp/fz.$side.out
done
for side in old new; do
  grep -vE '^\[fuzz [0-9]+/[0-9]+\] .*elapsed|^ *wall: [0-9.]+s on' \
    /tmp/fz.$side.out > /tmp/fz.$side.filt
done
if cmp -s /tmp/fz.old.filt /tmp/fz.new.filt; then
  echo "FUZZ PARITY OK ($N seeds, $FROM-$TO, modulo timing)"
  exit 0
fi
echo "FUZZ MISMATCH ($N seeds, $FROM-$TO):"
diff /tmp/fz.old.filt /tmp/fz.new.filt | head -20
exit 1

#!/bin/bash
# robust rebuild: the no-op cmake shim skips cmake_depends, so header
# mtimes don't always propagate -- touch every TU, then build
cd "$(dirname "$0")/.."
find src -name '*.cpp' -exec touch {} +
make -C build "$@" 2>&1 | grep -E "error|Error" && exit 1
exit 0

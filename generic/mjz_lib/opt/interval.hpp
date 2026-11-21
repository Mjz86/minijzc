/*MIT License

Copyright (c) 2026 Mjz86

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#ifndef MJZ_SRC_OPT_interval_FILE_
#define MJZ_SRC_OPT_interval_FILE_
#include "base.hpp"

MJZ_EXPORT
//
namespace mjz::opt_ns {

// ---------------------------------------------------------------------------
// iv_ns / ai_ns: the interval lattice + the forward abstract-
// interpretation driver (range analysis)
// ---------------------------------------------------------------------------
// THE SPLIT (the sccp precedent: propagation is the algorithm,
// semantics is policy). iv_ns is the INTERVAL LATTICE -- pure
// int64 math, no op vocabulary: hull (the join), equality, the
// narrowing meet, and the trap-safety predicates. The TRANSFERS
// (what Add does to intervals, what a taken comparison refines)
// stay with the client -- they speak the compiler's op ids.
// ai_ns::solve_forward is the DRIVER: plain rounds growing up
// from bottom, then WIDENING at the client's chosen points only
// (the Cousot discipline -- widening everywhere re-inflates
// refined states), then NARROWING rounds intersecting back down;
// a no-fixpoint give-up is reported and the client falls back to
// sound TOP. Callbacks (per the std::sort-comparator rule):
//   entry_top(st)              -- the entry block's initial state
//   refine_edge(p, b, st)      -- the fact the p->b edge carries
//   phi_join(b, st, exits, nv) -- the phi rule (exits = the pred
//                                exit table, read at p*nv+v)
//   transfer(b, st)            -- walk b's body through st
// The visit order (blocks ascending, preds in row order, rounds
// capped by the client's patience) is the driver's; the states
// are a pure function of that order, which is what keeps the
// rewrite byte-identical when the driver moves into the kernel.
namespace iv_ns {
struct iv_t {
  int64_t lo{INT64_MIN}, hi{INT64_MAX};
  bool bot{}; // empty: unreachable program point
};
MJZ_CX_FN bool iv_equal(const iv_t &a, const iv_t &b) noexcept {
  return a.bot == b.bot && a.lo == b.lo && a.hi == b.hi;
}
MJZ_CX_FN iv_t iv_join(const iv_t &a, const iv_t &b) noexcept { // hull
  if (a.bot)
    return b;
  if (b.bot)
    return a;
  return {a.lo < b.lo ? a.lo : b.lo, a.hi > b.hi ? a.hi : b.hi, false};
}
// narrowing meet: the tighter of two sound approximations, kept
// only when it is still non-empty (bot is "unreachable", not
// "contradiction": the original skips bot operands untouched)
MJZ_CX_FN void iv_narrow(iv_t &old, const iv_t &st) noexcept {
  if (st.bot || old.bot)
    return;
  const int64_t lo = st.lo > old.lo ? st.lo : old.lo;
  const int64_t hi = st.hi < old.hi ? st.hi : old.hi;
  if (lo <= hi)
    old = {lo, hi, false};
}
// trap-safety predicates (a sound over-approximation answers
// them): divisor excludes 0 / excludes -1, dividend excludes MIN
MJZ_CX_FN bool iv_excludes_zero(const iv_t &i) noexcept {
  return i.bot || i.hi < 0 || i.lo > 0;
}
MJZ_CX_FN bool iv_excludes_minus1(const iv_t &i) noexcept {
  return i.bot || i.hi < -1 || i.lo > -1;
}
MJZ_CX_FN bool iv_excludes_min(const iv_t &i) noexcept {
  return i.bot || i.lo > INT64_MIN;
}
} // namespace iv_ns

};     // namespace mjz::opt_ns
#endif // MJZ_SRC_OPT_interval_FILE_

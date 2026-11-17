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


#ifndef MJZ_SRC_OPT_inv_FILE_
#define MJZ_SRC_OPT_inv_FILE_
#include "base.hpp"

MJZ_EXPORT
//
namespace mjz::opt_ns {

// ---------------------------------------------------------------------------
// inv_ns: loop-invariant selection
// ---------------------------------------------------------------------------
// THE CONTRACT. The client states the loop (member row + header +
// in-loop mask), the preheader (from find_unique_outside_pred), the
// def tables (def_block_of_var, the client's const table), the
// dominance intervals, the block's sites in program order with their
// operand rows, and an ELIGIBILITY mask (the policy: purity, trap
// safety, has-a-def -- all compiler rules). The kernel answers the
// FIRST hoistable site under the scan discipline (header FIRST --
// an invariant header test is the classic unswitch enabler -- then
// the member row in order), where "hoistable" = eligible and every
// operand is invariant: absent, or const (position-independent), or
// defined outside the loop by a def that DOMINATES the preheader.
// One site per call: the client hoists, recomputes its tables, and
// calls again (the pass's rescan discipline -- a hoist can make more
// operands invariant).
MJZ_CX_FN std::pair<uintlen_t, uintlen_t>
find_unique_outside_pred(const auto &preds_of_header,
                         const auto &in_loop) noexcept {
  uintlen_t pre = none_v, outside = 0;
  for (const auto &pp : preds_of_header) {
    const uintlen_t p = uintlen_t(pp);
    if (!in_loop[p]) {
      pre = p;
      ++outside;
    }
  }
  return {pre, outside};
}

template <version_t version_v>
MJZ_CX_FN std::pair<uintlen_t, uintlen_t> calculate_first_invariant_site(
    const auto &loop_row, uintlen_t header, uintlen_t preheader,
    const auto &in_loop, const auto &def_block_of_var, const auto &const_mask,
    const auto &dom_begin, const auto &dom_end, const auto &sites_of_block,
    const auto &args_of_site, const auto &eligible) noexcept {
  const uintlen_t n_vars = uintlen_t(def_block_of_var.size());
  const uintlen_t n_blocks = uintlen_t(in_loop.size());
  auto dominates = [&](uintlen_t d, uintlen_t s) noexcept {
    if (d >= n_blocks || s >= n_blocks)
      return false;
    if (dom_begin[d] == 0 || dom_begin[s] == 0)
      return false;
    return dom_begin[d] <= dom_begin[s] && dom_end[d] >= dom_end[s];
  };
  auto invariant = [&](uintlen_t v) noexcept {
    if (v == none_v)
      return true;
    if (v >= n_vars)
      return false;
    const uintlen_t d = uintlen_t(def_block_of_var[v]);
    if (d == none_v)
      return false;
    if (const_mask[v])
      return true; // a Const's VALUE is position-independent
    if (in_loop[d])
      return false;                 // defined inside: not invariant
    return dominates(d, preheader); // reachable into the preheader
  };
  const uintlen_t row_len = uintlen_t(std::ranges::size(loop_row));
  for (uintlen_t bi = 0; bi <= row_len; ++bi) {
    const uintlen_t B = (bi == 0) ? header : uintlen_t(loop_row[bi - 1]);
    if (B >= n_blocks)
      continue;
    for (const auto &ss : sites_of_block[B]) {
      const uintlen_t s = uintlen_t(ss);
      if (!eligible[s])
        continue;
      bool ok = true;
      for (const auto &vv : args_of_site[s])
        ok &= invariant(uintlen_t(vv));
      if (ok)
        return {B, s};
    }
  }
  return {none_v, none_v};
}

};     // namespace mjz::opt_ns
#endif // MJZ_SRC_OPT_inv_FILE_

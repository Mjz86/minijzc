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


#ifndef MJZ_SRC_OPT_escape_FILE_
#define MJZ_SRC_OPT_escape_FILE_
#include "base.hpp"

MJZ_EXPORT
//
namespace mjz::opt_ns {

// ---------------------------------------------------------------------------
// escape_ns: the copy alias closure + the positional escape scan
// ---------------------------------------------------------------------------
// THE MODEL. copy_src/copy_dst are parallel rows of identity-preserving
// moves (a Copy passes the value through without observing it); seeds is
// the row of vars the closure starts from. args_of_site[s] is site s's
// operand vars in ANY order; arg_safe_of_site[s] is the parallel 0/1 row
// -- 1 marks a position where an alias member may legally appear (the
// handle position of a load, the source of a copy), 0 marks a position
// that OBSERVES the value (an index, a stored value, an arithmetic
// operand, a call argument). A missing flag means unsafe: an escape
// analysis must fail closed.
//
// THE CLOSURE (calculate_copy_closure): the seeds plus every var
// reachable from them through copy edges -- a monotone fixpoint, so the
// rescans may stop at the first round that grows nothing. The result is
// order-independent: it is the least fixpoint containing the seeds, the
// same set a graph BFS from the seeds would collect.
//
// THE SCAN (scan_position_escapes): does any closure member appear at
// an UNSAFE position of any site? Unsafe positions are exactly the ones
// whose flag is not 1 -- the client's position policy is the whole
// table, which is what makes this the stack-array analysis for ANY
// client (a different backend with different handle positions passes
// different flags, not a different kernel). none_v and junk operands
// are invisible: they are never members of a well-formed closure.
MJZ_CX_FN std::vector<char> calculate_copy_closure(
    const auto &copy_src, const auto &copy_dst, const auto &seeds,
    uintlen_t n_vars) noexcept {
  std::vector<char> ali(n_vars, 0);
  for (const auto &ss : seeds) {
    const uintlen_t v = uintlen_t(ss);
    if (v < n_vars)
      ali[v] = 1;
  }
  const uintlen_t n_copies = uintlen_t(copy_src.size());
  for (bool grew = true; grew;) {
    grew = false;
    for (uintlen_t i = 0; i < n_copies; ++i) {
      const uintlen_t s = uintlen_t(copy_src[i]);
      const uintlen_t d = uintlen_t(copy_dst[i]);
      if (s >= n_vars || d >= n_vars)
        continue;
      if (ali[s] && !ali[d]) {
        ali[d] = 1;
        grew = true;
      }
    }
  }
  return ali;
}

MJZ_CX_FN bool scan_position_escapes(const auto &args_of_site,
                                     const auto &arg_safe_of_site,
                                     const auto &alias) noexcept {
  const uintlen_t n_sites = uintlen_t(std::ranges::size(args_of_site));
  const uintlen_t n_vars = uintlen_t(std::ranges::size(alias));
  for (uintlen_t s = 0; s < n_sites; ++s) {
    const auto &args = args_of_site[s];
    const auto &safe = arg_safe_of_site[s];
    uintlen_t j = 0;
    for (const auto &vv : args) {
      const uintlen_t v = uintlen_t(vv);
      const bool is_safe =
          (j < uintlen_t(std::ranges::size(safe))) ? bool(safe[j]) : false;
      if (!is_safe && v < n_vars && alias[v])
        return true; // a closure member observed at an unsafe position
      ++j;
    }
  }
  return false;
}

};     // namespace mjz::opt_ns
#endif // MJZ_SRC_OPT_escape_FILE_

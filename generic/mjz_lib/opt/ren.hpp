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

#ifndef MJZ_SRC_OPT_ren_FILE_
#define MJZ_SRC_OPT_ren_FILE_
#include "base.hpp"

MJZ_EXPORT
//
namespace mjz::opt_ns {

// ---------------------------------------------------------------------------
// ren_ns: the copy-rename builder
// ---------------------------------------------------------------------------
// THE CONTRACT. pairs = (dst, src) of every eligible copy in code
// order (dst/src valid, dst != src). Sources resolve transitively
// through the map being built, so chains die in one hop; the
// CROSSED-PAIR guard refuses the closing copy of a cyclic rename
// (resolve(src) == dst -- a self-loop entry that would never
// resolve; the phi-degenerate hazard, fuzz seed 7406). The resolve
// walk is capped at n_vars steps: belt and braces so a future
// multi-def IR cannot resurrect an infinite walk.
MJZ_CX_FN std::vector<uintlen_t> build_copy_rename(uintlen_t n_vars,
                                                   const auto &pairs) noexcept {
  std::vector<uintlen_t> ren(n_vars, none_v);
  auto resolve = [&](uintlen_t v) noexcept {
    uintlen_t steps = 0;
    while (v != none_v && v < n_vars && ren[v] != none_v) {
      v = ren[v];
      if (++steps > n_vars)
        break;
    }
    return v;
  };
  for (const auto &pr : pairs) {
    const uintlen_t d = uintlen_t(pr.first), s = uintlen_t(pr.second);
    if (d >= n_vars || s >= n_vars)
      continue;
    const uintlen_t r = resolve(s);
    if (r != d) // r == d: crossed pair -- do not record
      ren[d] = r;
  }
  return ren;
}
MJZ_CX_FN uintlen_t resolve_capped(const std::vector<uintlen_t> &ren,
                                   uintlen_t v) noexcept {
  const uintlen_t n_vars = uintlen_t(ren.size());
  uintlen_t steps = 0;
  while (v != none_v && v < n_vars && ren[v] != none_v) {
    v = ren[v];
    if (++steps > n_vars)
      break;
  }
  return v;
}

};     // namespace mjz::opt_ns
#endif // MJZ_SRC_OPT_ren_FILE_

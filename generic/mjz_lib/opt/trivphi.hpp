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


#ifndef MJZ_SRC_OPT_trivphi_FILE_
#define MJZ_SRC_OPT_trivphi_FILE_
#include "base.hpp"

MJZ_EXPORT
//
namespace mjz::opt_ns {

// ---------------------------------------------------------------------------
// the trivial-phi pick (dce's collapse rule, N-ary)
// ---------------------------------------------------------------------------
// All non-self incoming args equal (absent args ignored) -> that
// arg; more than one distinct value or nothing but self/absent ->
// none_v (a genuine phi). phi(a,a), phi(a,self), phi(a) pick a --
// the collapse the copyprop pass then feeds on.
MJZ_CX_FN uintlen_t calculate_trivial_phi_pick(uintlen_t dst,
                                               const auto &args_row) noexcept {
  uintlen_t pick = none_v;
  for (const auto &aa : args_row) {
    const uintlen_t a = uintlen_t(aa);
    if (a == none_v || a == dst)
      continue;
    if (pick == none_v)
      pick = a;
    else if (pick != a)
      return none_v; // genuine phi
  }
  return pick;
}

};     // namespace mjz::opt_ns
#endif // MJZ_SRC_OPT_trivphi_FILE_

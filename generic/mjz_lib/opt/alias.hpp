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


#ifndef MJZ_SRC_OPT_alias_FILE_
#define MJZ_SRC_OPT_alias_FILE_
#include "base.hpp"

MJZ_EXPORT
//
namespace mjz::opt_ns {

// ---------------------------------------------------------------------------
// alias_ns: allocation-identity resolution (the copy/phi fixpoint)
// ---------------------------------------------------------------------------
// THE CONTRACT. Per var, the client states its def in CODE ORDER:
// kind alias_copy_v (single[dst] := single[src]), alias_phi_v
// (single iff EVERY arg is a valid var whose single is the SAME
// non-none id -- N-ary: all args must agree; an absent arg is
// obscure), or alias_other_v (leave as seeded). single[] arrives
// PRE-SEEDED (the allocations; the wrapper's Alloc pass) and is
// resolved in place: rounds over the record list (max_rounds is
// the client's patience cap -- the compiler's historical 64,
// kept as policy), each transition sets grew. The values are
// client ids (allocation sites, handle classes, whatever the
// "proves exactly one X" question is about).
inline constexpr uintlen_t alias_other_v = 0;
inline constexpr uintlen_t alias_copy_v = 1;
inline constexpr uintlen_t alias_phi_v = 2;
struct alias_def_t {
  uintlen_t var{};
  uintlen_t kind{alias_other_v};
  uintlen_t src{};                  // copy source (copy only)
  std::vector<uintlen_t> args{};    // phi args, raw (phi only)
};
template <version_t version_v>
MJZ_CX_FN void resolve_single_ids(const std::vector<alias_def_t> &defs,
                                  std::vector<uintlen_t> &single,
                                  uintlen_t n_vars,
                                  uintlen_t max_rounds) noexcept {
  bool grew = true;
  for (uintlen_t guard = 0; grew && guard < max_rounds; ++guard) {
    grew = false;
    for (const alias_def_t &d : defs) {
      if (d.var >= n_vars)
        continue;
      if (d.kind == alias_copy_v) {
        if (d.src < n_vars && single[d.var] != single[d.src]) {
          single[d.var] = single[d.src];
          grew = true;
        }
      } else if (d.kind == alias_phi_v) {
        // single iff EVERY arg is a valid var, none obscure, and
        // all carry the SAME id (an absent arg is obscure)
        bool ok = !d.args.empty();
        uintlen_t want = none_v;
        for (uintlen_t k = 0; k < d.args.size() && ok; ++k) {
          const uintlen_t a = d.args[k];
          if (a >= n_vars || single[a] == none_v) {
            ok = false;
            break;
          }
          if (k == 0)
            want = single[a];
          else
            ok &= (single[a] == want);
        }
        want = ok ? want : none_v;
        if (single[d.var] != want) {
          single[d.var] = want;
          grew = true;
        }
      }
    }
  }
}

};     // namespace mjz::opt_ns
#endif // MJZ_SRC_OPT_alias_FILE_

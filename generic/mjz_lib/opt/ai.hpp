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

#ifndef MJZ_SRC_OPT_ai_FILE_
#define MJZ_SRC_OPT_ai_FILE_
#include "interval.hpp"

MJZ_EXPORT
//
namespace mjz::opt_ns {

namespace ai_ns {
template <version_t version_v> struct ai_result_t {
  std::vector<iv_ns::iv_t> entry{}; // nb * nv, block-entry states
  bool converged{};
};
// preds: raw pred rows; widen_at: the widening points (loop
// headers); entry_block: the start (its state = entry_top);
// plain_cap / widen_cap / narrow_rounds: the client's patience.
template <version_t version_v, class CB>
MJZ_CX_FN ai_result_t<version_v>
solve_forward(uintlen_t nb, uintlen_t nv, const auto &preds,
              const auto &widen_at, uintlen_t entry_block, uintlen_t plain_cap,
              uintlen_t widen_cap, uintlen_t narrow_rounds,
              const CB &cb) noexcept {
  ai_result_t<version_v> R{};
  if (!nb || !nv)
    return R;
  std::vector<iv_ns::iv_t> entry((size_t)nb * nv, iv_ns::iv_t{0, 0, true});
  std::vector<iv_ns::iv_t> exit_((size_t)nb * nv, iv_ns::iv_t{0, 0, true});
  std::vector<char> touched(nb, 0);
  std::vector<iv_ns::iv_t> st(nv), edge(nv);
  bool widening = false;
  uintlen_t round = 0;
  bool converged = false;
  for (;;) {
    bool changed = false;
    for (uintlen_t b = 0; b < nb; ++b) {
      // ---- entry = hull over incoming (refined) pred exits + phi rule
      for (uintlen_t v = 0; v < nv; ++v)
        st[v] = {0, 0, true};
      bool any_pred = false;
      for (const auto &pp : preds[b]) {
        const uintlen_t p = uintlen_t(pp);
        if (p >= nb || !touched[p])
          continue;
        any_pred = true;
        for (uintlen_t v = 0; v < nv; ++v)
          edge[v] = exit_[(size_t)p * nv + v];
        cb.refine_edge(p, b, edge); // the fact the edge carries
        for (uintlen_t v = 0; v < nv; ++v)
          st[v] = iv_ns::iv_join(st[v], edge[v]);
      }
      if (!any_pred && b != entry_block) // unreachable: keep bottom
        for (uintlen_t v = 0; v < nv; ++v)
          st[v] = {0, 0, true};
      if (b == entry_block)
        cb.entry_top(st);
      else
        cb.phi_join(b, st, exit_.data(), nv);
      // ---- convergence bookkeeping (+ widening at the chosen points)
      {
        bool diff = false;
        for (uintlen_t v = 0; v < nv; ++v)
          if (!iv_ns::iv_equal(st[v], entry[(size_t)b * nv + v])) {
            diff = true;
            break;
          }
        if (diff) {
          if (widening && b < widen_at.size() && widen_at[b]) {
            for (uintlen_t v = 0; v < nv; ++v) {
              iv_ns::iv_t &old = entry[(size_t)b * nv + v];
              if (st[v].bot)
                continue;
              if (st[v].lo < old.lo)
                st[v].lo = INT64_MIN; // grow the growing sides
              if (st[v].hi > old.hi)
                st[v].hi = INT64_MAX;
              if (st[v].lo > st[v].hi)
                st[v] = old; // paranoia
            }
          }
          for (uintlen_t v = 0; v < nv; ++v)
            entry[(size_t)b * nv + v] = st[v];
          changed = true;
        }
      }
      // ---- transfer through the block
      for (uintlen_t v = 0; v < nv; ++v)
        st[v] = entry[(size_t)b * nv + v];
      cb.transfer(b, st);
      for (uintlen_t v = 0; v < nv; ++v)
        exit_[(size_t)b * nv + v] = st[v];
      touched[b] = 1;
    }
    if (!changed) {
      converged = true;
      break;
    }
    ++round;
    if (!widening && round >= plain_cap)
      widening = true;
    if (widening && round >= plain_cap + widen_cap)
      break; // give up: the caller falls back to sound TOP
  }
  if (converged) {
    // ---- narrowing: re-transfer and intersect with the widened state
    for (uintlen_t n = 0; n < narrow_rounds; ++n) {
      for (uintlen_t b = 0; b < nb; ++b) {
        for (uintlen_t v = 0; v < nv; ++v)
          st[v] = {0, 0, true};
        for (const auto &pp : preds[b]) {
          const uintlen_t p = uintlen_t(pp);
          if (p >= nb || !touched[p])
            continue;
          for (uintlen_t v = 0; v < nv; ++v)
            edge[v] = exit_[(size_t)p * nv + v];
          cb.refine_edge(p, b, edge);
          for (uintlen_t v = 0; v < nv; ++v)
            st[v] = iv_ns::iv_join(st[v], edge[v]);
        }
        if (b == entry_block)
          cb.entry_top(st);
        else
          cb.phi_join(b, st, exit_.data(), nv);
        for (uintlen_t v = 0; v < nv; ++v)
          iv_ns::iv_narrow(entry[(size_t)b * nv + v], st[v]);
        for (uintlen_t v = 0; v < nv; ++v)
          st[v] = entry[(size_t)b * nv + v];
        cb.transfer(b, st);
        for (uintlen_t v = 0; v < nv; ++v)
          exit_[(size_t)b * nv + v] = st[v];
      }
    }
  }
  R.entry = std::move(entry);
  R.converged = converged;
  return R;
}
} // namespace ai_ns

};     // namespace mjz::opt_ns
#endif // MJZ_SRC_OPT_ai_FILE_

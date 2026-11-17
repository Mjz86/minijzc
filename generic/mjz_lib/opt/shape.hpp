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


#ifndef MJZ_SRC_OPT_shape_FILE_
#define MJZ_SRC_OPT_shape_FILE_
#include "base.hpp"

MJZ_EXPORT
//
namespace mjz::opt_ns {

// ---------------------------------------------------------------------------
// shape_ns: natural-loop shape as pure CFG math
// ---------------------------------------------------------------------------
// THE CONTRACT. classify_header_preds: one sweep over a header's
// pred row against the loop mask -- the LAST inside pred is the
// latch candidate, the LAST outside one the preheader candidate,
// with the counts (the "exactly one of each" test is the caller's
// loop-shape policy). calculate_loop_shape: the canonical test-
// first shape -- unique in-pred (latch), unique out-pred (pre),
// and a TWO-ARM branch with exactly one arm staying (body) and
// one leaving (X); the terminator KIND check stays with the
// client (which instr ends a block is IR knowledge; the arms and
// their in/out classification are CFG math). collect_exit_edges:
// every in-loop -> outside edge except the client's primary edge,
// with the degeneracy rule (two VALID slots on the same outside
// target = a both-arms-out branch -- bypass threading cannot
// represent per-arm values) and the forbidden-target paranoia as
// parameters. Every restructuring pass (unroll / lcf / unswitch /
// rotate / lsimp) asks these three questions; none of them may
// assume the binary terminator -- the row shapes are the client's.
struct header_preds_t {
  uintlen_t pre{none_v}, latch{none_v};
  uintlen_t n_in{}, n_out{};
};
MJZ_CX_FN header_preds_t calculate_header_preds(const auto &pred_row,
                                                const auto &in_loop) noexcept {
  header_preds_t r{};
  for (const auto &pp : pred_row) {
    const uintlen_t p = uintlen_t(pp);
    if (p < in_loop.size() && in_loop[p]) {
      r.latch = p;
      ++r.n_in;
    } else {
      r.pre = p;
      ++r.n_out;
    }
  }
  return r;
}
struct loop_shape_t {
  uintlen_t pre{none_v}, latch{none_v}, body{none_v}, X{none_v};
  bool ok{};
};
// arm0/arm1: the header branch's two VALID targets (slot order).
MJZ_CX_FN loop_shape_t calculate_loop_shape(const auto &pred_row,
                                            const auto &in_loop,
                                            uintlen_t arm0,
                                            uintlen_t arm1) noexcept {
  loop_shape_t sh{};
  const header_preds_t hp = calculate_header_preds(pred_row, in_loop);
  if (hp.n_in != 1 || hp.n_out != 1)
    return sh;
  sh.pre = hp.pre;
  sh.latch = hp.latch;
  const uintlen_t nb = uintlen_t(in_loop.size());
  const bool v0 = arm0 < nb, v1 = arm1 < nb;
  const bool in0 = v0 && in_loop[arm0], in1 = v1 && in_loop[arm1];
  if (in0 && !in1 && v1) {
    sh.body = arm0;
    sh.X = arm1;
  } else if (in1 && !in0 && v0) {
    sh.body = arm1;
    sh.X = arm0;
  } else
    return sh;
  sh.ok = true;
  return sh;
}
struct exit_edge_t {
  uintlen_t from{}, to{};
};
// Every in-loop -> outside edge except (skip_from -> skip_to).
// Returns false on the degeneracy (two valid slots, same outside
// target) or a forbidden target (forbid0/forbid1, none_v = none):
// the caller discards the partial list either way.
MJZ_CX_FN bool collect_exit_edges(const auto &succs_rows, const auto &in_loop,
                                  uintlen_t skip_from, uintlen_t skip_to,
                                  uintlen_t forbid0, uintlen_t forbid1,
                                  std::vector<exit_edge_t> &out) noexcept {
  const uintlen_t nb = uintlen_t(succs_rows.size());
  for (uintlen_t b = 0; b < nb; ++b) {
    if (!(b < in_loop.size() && in_loop[b]))
      continue;
    const auto &row = succs_rows[b];
    uintlen_t valid = 0, distinct = 0;
    uintlen_t first = none_v;
    for (const auto &ss : row) {
      const uintlen_t s = uintlen_t(ss);
      if (s == none_v || s >= nb)
        continue;
      ++valid;
      if (first == none_v) {
        first = s;
        distinct = 1;
      } else if (s != first)
        distinct = 2;
    }
    if (valid >= 2 && distinct == 1 && first < in_loop.size() &&
        !in_loop[first])
      return false; // every arm on ONE outside target: degenerate
    for (const auto &ss : row) {
      const uintlen_t s = uintlen_t(ss);
      if (s == none_v || s >= nb || in_loop[s])
        continue;
      if (b == skip_from && s == skip_to)
        continue; // the edge the caller replaces outright
      out.push_back({.from = b, .to = s});
      if (s == forbid0 || s == forbid1)
        return false; // would tangle the versioned preds; paranoia
    }
  }
  return true;
}

};     // namespace mjz::opt_ns
#endif // MJZ_SRC_OPT_shape_FILE_

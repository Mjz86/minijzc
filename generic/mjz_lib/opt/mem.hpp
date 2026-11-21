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

#ifndef MJZ_SRC_OPT_mem_FILE_
#define MJZ_SRC_OPT_mem_FILE_
#include "base.hpp"

MJZ_EXPORT
//
namespace mjz::opt_ns {

// ---------------------------------------------------------------------------
// mem_ns: the dominator-scoped forwarding table (load elimination)
// ---------------------------------------------------------------------------
// THE CONTRACT. A table of ENTRIES (group, key-tag, key payload,
// value, from_store) lives along the dominator path: entering a
// child watermarks the creation sequence, leaving truncates every
// entry created inside the subtree (seq > watermark -- positions
// would regrow garbage after a deep join drop emptied them, so
// scope death goes by creation order, never by resize(mark)).
// The client supplies, per block, an EVENT list in program order:
//   mem_store_v  -- kill every entry the MAY_KILL predicate names
//                   (the memory model: which remembered writes a
//                   new store can overwrite -- policy), then push
//                   the written slot (val none_v = kill only)
//   mem_load_v   -- first-match lookup on (group, key), the hit
//                   recorded for the client's rewrite, then the
//                   slot re-recorded as holding THIS value
//   mem_clear_v  -- the model lost track (obscure store, a call
//                   with handles): drop everything
// DROP_AT[b] names the blocks whose ENTRY state the client cannot
// treat as the dominator's continuation (joins: the memory state
// merges, a sibling arm's stores are invisible but invalidate the
// ancestor's "last write" claim) -- a mask INPUT, because a
// different memory model (phi-placed, like mem2reg) would pass
// all-zero. OUTPUT: one hit record per load event, in event order.
inline constexpr uintlen_t mem_store_v = 0;
inline constexpr uintlen_t mem_load_v = 1;
inline constexpr uintlen_t mem_clear_v = 2;
struct mem_event_t {
  uintlen_t kind{mem_load_v};
  uintlen_t group{};     // the allocation / slot-space id
  uintlen_t key_tag{};   // client key tag (const-ness, ...)
  uintlen_t key_a{};     // key payload A (the vreg key)
  uintlen_t key_b{};     // key payload B (the const value)
  uintlen_t val{none_v}; // store: the written value; load: the dst
};
struct mem_hit_t {
  bool found{};
  uintlen_t val{};
  bool from_store{};
};
template <version_t version_v, class MayKill>
MJZ_CX_FN std::vector<mem_hit_t>
run_dom_scoped_table(const auto &dominator_tree, const auto &sites_of_block,
                     const auto &drop_at, const auto &events_of_block,
                     const MayKill &may_kill) noexcept {
  struct ent_t {
    uintlen_t group{}, key_tag{}, key_a{}, key_b{}, val{};
    bool from_store{};
    uintlen_t seq{};
  };
  std::vector<ent_t> table;
  uintlen_t seq_ctr = 0;
  auto push_ent = [&](ent_t e) noexcept {
    e.seq = ++seq_ctr;
    table.push_back(e);
  };
  std::vector<mem_hit_t> hits{};
  auto rows = dominator_tree.range();
  const uintlen_t nb = uintlen_t(std::ranges::size(rows));
  struct frame_t {
    uintlen_t b, child_idx, w;
  };
  std::vector<frame_t> st;
  st.push_back({0, 0, seq_ctr});
  while (!st.empty()) {
    frame_t &fr = st.back();
    const uintlen_t b = fr.b;
    if (fr.child_idx == 0) { // entering: scan the block's events once
      if (b < drop_at.size() && drop_at[b])
        table.clear(); // join: the memory state merges
      for (const auto &ee : events_of_block[b]) {
        const mem_event_t &ev = ee;
        if (ev.kind == mem_store_v) {
          size_t w = 0;
          for (size_t r = 0; r < table.size(); ++r)
            if (!may_kill(table[r].group, table[r].key_tag, table[r].key_a,
                          table[r].key_b, ev))
              table[w++] = table[r];
          table.resize(w);
          if (ev.val != none_v)
            push_ent({.group = ev.group,
                      .key_tag = ev.key_tag,
                      .key_a = ev.key_a,
                      .key_b = ev.key_b,
                      .val = ev.val,
                      .from_store = true});
        } else if (ev.kind == mem_load_v) {
          mem_hit_t h{};
          for (const ent_t &e : table)
            if (e.group == ev.group && e.key_tag == ev.key_tag &&
                (ev.key_tag ? e.key_b == ev.key_b : e.key_a == ev.key_a)) {
              h = {.found = true, .val = e.val, .from_store = e.from_store};
              break;
            }
          hits.push_back(h);
          push_ent({.group = ev.group,
                    .key_tag = ev.key_tag,
                    .key_a = ev.key_a,
                    .key_b = ev.key_b,
                    .val = ev.val,
                    .from_store = false});
        } else if (ev.kind == mem_clear_v) {
          table.clear();
        }
      }
    }
    auto ch = rows[b];
    if (fr.child_idx < uintlen_t(std::ranges::size(ch))) {
      const uintlen_t next = uintlen_t(ch[fr.child_idx++]);
      st.push_back({next, 0, seq_ctr});
      continue;
    }
    // leaving b: drop everything created inside its subtree
    {
      size_t wi = 0;
      for (size_t r = 0; r < table.size(); ++r)
        if (table[r].seq <= fr.w)
          table[wi++] = table[r];
      table.resize(wi);
    }
    st.pop_back();
  }
  return hits;
}

};     // namespace mjz::opt_ns
#endif // MJZ_SRC_OPT_mem_FILE_

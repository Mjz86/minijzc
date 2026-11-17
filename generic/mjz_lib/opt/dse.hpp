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


#ifndef MJZ_SRC_OPT_dse_FILE_
#define MJZ_SRC_OPT_dse_FILE_
#include "base.hpp"

MJZ_EXPORT
//
namespace mjz::opt_ns {

// ---------------------------------------------------------------------------
// dse_ns: the in-window dead-store kill table
// ---------------------------------------------------------------------------
// THE MODEL. One row of events per basic block (rows are independent:
// a block boundary is a window break by construction). Events, in
// program order:
//   * dse_store_v {group, key, val} -- a store through an EXACT
//     identity: group = the allocation (single-handle-proven), key =
//     the EXACT slot (a constant index by VALUE -- the client has
//     already proven the index in-bounds, so the store cannot trap
//     and the key is exact), val = the stored value id (SSA: the id
//     IS the value).
//   * dse_load_v {group, key} -- a read of an exact slot, same
//     exactness contract (in-bounds constant index: no trap).
//   * dse_break_v -- the window broke: anything that may trap, may
//     observe, or may write an INEXACT slot (an obscure handle, a
//     runtime index, a call, a print, an input, a division, an
//     allocation, a terminator). The window discipline is the
//     soundness core: deleting a store can only be invisible when
//     NOTHING between it and its killer could have trapped (a moved
//     trap changes the reported line), printed, or read the slot --
//     and every such instruction is a break in this model, decided
//     by the CLIENT's policy. Instrs that are provably pure and
//     non-trapping (consts, copies, plain arithmetic, lengths) emit
//     no event at all: they are window-transparent.
// THE RULES (checked in this order -- a pair fires exactly one):
//   * SAME-VALUE. A store of the SAME value id to the same exact
//     slot as the remembered live store is redundant: the slot
//     provably still holds that value (no break since -- a break
//     would have dropped the entry; no other store to the slot --
//     it would have replaced the entry; stores to OTHER exact slots
//     cannot write this one: constant keys are exact). The redundant
//     store dies; the remembered entry STANDS (the survivor keeps
//     the kill chain alive below).
//   * OVERWRITE. A store of a DIFFERENT value to a slot whose
//     remembered store has not been READ since (no dse_load_v of the
//     key between them -- loads set the read flag) kills the
//     remembered store: nothing observed it, and the new store
//     overwrites the slot on every path (the window guarantees no
//     read in between; writes in between are impossible by
//     exactness). The remembered store dies; the entry becomes the
//     new store, read flag cleared.
// THE OUTPUT. dead[] pairs {site = the store's GLOBAL event ordinal
// (ascending across the rows), rule} -- the client pairs store
// events with instruction indices through its own parallel row
// (built in the same emission order), exactly like ldelim's load_at.
struct dse_event_t {
  uintlen_t kind{};  // dse_store_v / dse_load_v / dse_break_v
  uintlen_t group{}; // the allocation identity (exact)
  uintlen_t key{};   // the slot key (exact; constants by value)
  uintlen_t val{};   // stores: the stored value id
};
inline constexpr uintlen_t dse_store_v = 0;
inline constexpr uintlen_t dse_load_v = 1;
inline constexpr uintlen_t dse_break_v = 2;
struct dse_dead_t {
  uintlen_t site{}; // the dead store's global event ordinal
  uintlen_t rule{}; // dse_overwrite_v / dse_same_v
};
inline constexpr uintlen_t dse_overwrite_v = 0;
inline constexpr uintlen_t dse_same_v = 1;

template <version_t version_v>
MJZ_CX_FN std::vector<dse_dead_t>
calculate_dead_stores(const auto &events_of_block) noexcept {
  std::vector<dse_dead_t> dead{};
  struct ent_t {
    uintlen_t group{}, key{}, site{}, val{};
    bool read{};
  };
  std::vector<ent_t> table{};
  uintlen_t ord{};
  for (const auto &row : events_of_block) {
    table.clear(); // a block boundary is a window break
    for (const auto &e : row) {
      const uintlen_t o = ord++;
      if (e.kind == dse_break_v) {
        table.clear();
        continue;
      }
      ent_t *t = nullptr;
      for (ent_t &c : table)
        if (c.group == e.group && c.key == e.key) {
          t = &c;
          break;
        }
      if (e.kind == dse_load_v) {
        if (t)
          t->read = true; // the slot was observed
        continue;
      }
      // dse_store_v
      if (t) {
        if (t->val == e.val) { // same-value: this store is redundant
          dead.push_back({o, dse_same_v});
          continue; // the survivor keeps the entry
        }
        if (!t->read) // overwrite: the remembered store is dead
          dead.push_back({t->site, dse_overwrite_v});
        t->site = o; // this store is the live writer now
        t->val = e.val;
        t->read = false;
      } else {
        table.push_back({e.group, e.key, o, e.val, false});
      }
    }
  }
  return dead;
}

};     // namespace mjz::opt_ns
#endif // MJZ_SRC_OPT_dse_FILE_

#pragma once
// === ldelim.hpp ========================================
// load elimination: store->load forwarding + redundant-load CSE for
// provably-single allocations.
//
// ALGORITHM: opt_ldelim walks the dominator tree (A.dom_tree.range())
//   in preorder. Entering a block with >1 preds clears the table (the
//   join drop); then per instr: a Store through a single-allocation
//   handle runs kill_for_store, then push_ents slot = the stored
//   vreg; an obscure-handle Store clears the table. A single-handle
//   Load find_exacts; a hit rewrites it to K::Copy of the remembered
//   value (its nt guard dropped), then the slot is re-recorded as
//   holding THIS dst. A Call with args clears everything; leaving
//   a block erases the entries created inside its subtree.
// DATA STRUCTURES:
//   * AI.single[h] (alias_analyze): the ONE allocation h provably
//     touches -- only such handles take part.
//   * kc/kv: const temps; a const index keys by VALUE, not by vreg.
//   * table of Ent {alloc,ckey,v,c,val,from_store,seq}: the scoped
//     memory table; from_store only splits ld_fwds vs ld_cses.
//   * seq_ctr + Fr {b,ci,w}: the preorder stack; w = the seq watermark.
// THEORY: a Load reads what the last dominating Store (or Load) of the
//   SAME slot wrote (or read): the dominating store ran on every path
//   reaching the load -- and if it was out of bounds it trapped first,
//   so dropping the load's own guard loses nothing. Joins drop the
//   whole table (the other pred's state may differ: forwarding past
//   the join would miscompile `if (c) a[0]=1; ... print a[0]`), and
//   scope exit goes by seq watermark, not vector position -- a deep
//   join drop empties slots ancestor frames still reference, so
//   entries die by "created inside this subtree" (seq > w), never by
//   resize(mark).
// ENABLES: every forwarded load becomes a plain K::Copy -- copyprop's
//   food, and no longer a trapping Load root for dce's mark-sweep; the
//   remaining loads are the ones no dominating def could explain.

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../analysis/alias.hpp"
#include "../analysis/an.hpp"
#include "../opt_generic.hpp"

// --- 9d4. LOAD ELIMINATION
// ------------------------------------------------------ For handles that
// provably name EXACTLY ONE allocation (alias_analyze's `single`), a Load reads
// what the last dominating Store (or Load) of the SAME slot wrote (or read):
//   * Load(h,i) with a dominating Store(h,i,v) -> Load becomes `= v`
//     (store-to-load forwarding; the store dominates the load, so on every
//     path that reaches the load the store already ran — and if the store
//     was out of bounds it trapped before the load could run, so dropping
//     the load's own guard loses nothing)
//   * Load(h,i) with a dominating Load(h,i)      -> Load becomes `= w`
//     (redundant-load CSE; the first load dominates, same argument)
// "Same slot" means the SAME index vreg, or the SAME constant index: two
// different index vregs may hold equal values, so they never match (and a
// Store through a different vreg index invalidates every vreg-keyed entry
// of that allocation, because it may write the very slot they name). A
// Store through an OBSCURE handle (any array, or none) invalidates
// everything; a Load through one only reads, so it is safe. A Call with
// arguments hands the callee handles it may store through, so it also
// invalidates everything (a 0-arg call can only touch its OWN fresh
// allocations: the bump arena hands out disjoint ranges).
//
// Scoping is the delicate part. Entries live along the DOMINATOR path only
// (like CSE), PLUS: entering a block with more than one predecessor
// (a join / loop header) drops the whole table -- the memory state arriving
// from the other predecessor may differ (a Store in a sibling arm is NOT
// visible here, but it also means the ancestor's Store is not the last
// write on the sibling's path, so its value cannot simply be forwarded
// below the join; a Phi would be needed, which is mem2reg's job). Without
// the join drop, `if (c) a[0]=1; ... print a[0]` past the merge would
// forward the pre-branch value on the taken path: a miscompile.
// Truncation uses creation SEQUENCE numbers, not vector positions: a join
// drop deep in a subtree empties positions that ancestor frames still
// reference, so `resize(mark)` could regrow garbage -- entries are instead
// erased by "created inside this subtree" (seq > the frame's watermark).
constexpr bool opt_ldelim(Fn &fn, const An &A, OptStats &os) {
  if (A.n != fn.nblocks() || !fn.nblocks())
    return false;
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nv)
    return false;
  AliasInfo AI = alias_analyze(fn);
  // known constants (index operands key by VALUE, not by vreg)
  std::vector<char> kc{};
  std::vector<val_t> kv{};
  seed_consts(fn, nv, kc, kv);
  // ---- the event stream (the wrapper's half of the kernel contract):
  // per block, in program order, which sites the dominator-scoped
  // table must kill / query / re-record, and which blocks drop the
  // table on entry (joins: the memory state merges -- the mask is an
  // INPUT, because a phi-placed model like mem2reg's would pass
  // all-zero). The table mechanics -- first-match lookup, the
  // seq-watermark scope truncation (entries die by "created inside
  // this subtree", never by resize(mark): a deep join drop empties
  // positions ancestor frames still reference) -- are the GENERIC
  // KERNEL (opt_ns::mem_ns::run_dom_scoped_table).
  const uint32_t nb = fn.nblocks();
  std::vector<std::vector<opt_ns::mem_event_t>> evs(nb);
  std::vector<uint32_t> load_at{}; // load-event order -> instr index
  std::vector<char> drop_at(nb, 0);
  for (uint32_t b = 0; b < nb; ++b) {
    drop_at[b] = fn.preds[b].size() > 1;
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k == K::Store) {
        const bool single = I.a != NONE && I.a < nv && AI.single[I.a] != NONE;
        if (!single) { // may write ANY slot
          evs[b].push_back({.kind = opt_ns::mem_clear_v});
          continue;
        }
        const uint8_t ck = (I.b2 != NONE && I.b2 < nv && kc[I.b2]) ? 1 : 0;
        const val_t cv = ck ? kv[I.b2] : 0;
        evs[b].push_back({.kind = opt_ns::mem_store_v,
                          .group = AI.single[I.a],
                          .key_tag = ck,
                          .key_a = (I.b2 != NONE && I.b2 < nv) ? (uintlen_t)I.b2
                                                               : opt_ns::none_v,
                          .key_b = (uintlen_t)(int64_t)cv,
                          .val = (I.c2 != NONE && I.c2 < nv) ? (uintlen_t)I.c2
                                                             : opt_ns::none_v});
      } else if (I.k == K::Load) {
        const bool single = I.a != NONE && I.a < nv && AI.single[I.a] != NONE;
        if (!single)
          continue; // reads any slot: no kill, no key
        if (I.dst == NONE || I.dst >= nv)
          continue;
        const uint8_t ck = (I.b2 != NONE && I.b2 < nv && kc[I.b2]) ? 1 : 0;
        const val_t cv = ck ? kv[I.b2] : 0;
        evs[b].push_back({.kind = opt_ns::mem_load_v,
                          .group = AI.single[I.a],
                          .key_tag = ck,
                          .key_a = (I.b2 != NONE && I.b2 < nv) ? (uintlen_t)I.b2
                                                               : opt_ns::none_v,
                          .key_b = (uintlen_t)(int64_t)cv,
                          .val = I.dst});
        load_at.push_back(i);
      } else if (I.k == K::Call) {
        if (!I.args.empty())
          evs[b].push_back({.kind = opt_ns::mem_clear_v});
      }
    }
  }
  // the kill rule (the memory model, stated once as a predicate): a
  // store through THIS allocation kills the exact const key when
  // const-keyed, every vreg-keyed entry of the allocation otherwise
  // (they may name the stored slot); a vreg-indexed store may equal
  // any key's value, so it kills the allocation's every entry
  auto may_kill = [](uintlen_t eg, uintlen_t etag, uintlen_t, uintlen_t ekb,
                     const opt_ns::mem_event_t &ev) {
    if (eg != ev.group)
      return false;
    if (ev.key_tag == 0)
      return true; // vreg index may equal any key's value
    if (etag == 0)
      return true; // a vreg-keyed entry may name the stored const
    return ekb == ev.key_b;
  };
  const std::vector<opt_ns::mem_hit_t> hits =
      opt_ns::run_dom_scoped_table<V>(A.dom_tree, evs, drop_at, evs, may_kill);
  // ---- the replay: forward every hit load to the remembered value
  // (its nt guard dropped -- the dominating store trapped first if it
  // was out of bounds), then let tier-0 copyprop/dce finish. The
  // walk repeats the kernel's DFS order (hits are in event order =
  // dominator preorder), pairing each hit with its load instr.
  bool changed = false;
  {
    uintlen_t k = 0;
    auto rows = A.dom_tree.range();
    struct Fr {
      uint32_t b, ci;
    };
    std::vector<Fr> st;
    st.push_back({0, 0});
    while (!st.empty()) {
      const uint32_t b = st.back().b;
      const uint32_t ci = st.back().ci;
      if (ci == 0) {
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
          const Instr &I = fn.code[i];
          const bool single_load = I.k == K::Load && I.a != NONE && I.a < nv &&
                                   AI.single[I.a] != NONE && I.dst != NONE &&
                                   I.dst < nv;
          if (!single_load)
            continue;
          if (k >= hits.size())
            break;
          const opt_ns::mem_hit_t &h = hits[k++];
          if (h.found && h.val != opt_ns::none_v && (uint32_t)h.val != I.dst) {
            // forward: the dominating def of `val' already produced
            // (or holds) this slot's value
            Instr &W = fn.code[i];
            W.k = K::Copy;
            W.a = (uint32_t)h.val;
            W.b2 = NONE;
            W.nt = false;
            if (h.from_store)
              os.ld_fwds++;
            else
              os.ld_cses++;
            changed = true;
          }
        }
      }
      auto ch = rows[b];
      if (ci < (uint32_t)ch.size()) {
        st.back().ci = ci + 1;
        st.push_back({(uint32_t)ch[ci], 0});
        continue;
      }
      st.pop_back();
    }
  }
  return changed;
}

#pragma once
// === ldelim.hpp ========================================
// load elimination: store->load forwarding + redundant-load CSE for
// provably-single allocations.

#include "minijz/base/pass_cfg.hpp"
#include "minijz/ir/instr.hpp"
#include "minijz/opt/analysis/an.hpp"
#include "minijz/opt/analysis/alias.hpp"

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
  std::vector<char> kc(nv, 0);
  std::vector<val_t> kv(nv, 0);
  for (const Instr &I : fn.code)
    if (I.k == K::Const && I.dst != NONE && I.dst < nv) {
      kc[I.dst] = 1;
      kv[I.dst] = I.c;
    }
  struct Ent {
    uint32_t alloc{};
    uint8_t ckey{}; // 1: key is the const `c'; 0: key is vreg `v'
    uint32_t v{};
    val_t c{};
    uint32_t val{};    // the value vreg a later same-slot Load yields
    bool from_store{}; // statistics: forwarded store vs CSE'd load
    uint32_t seq{};    // creation order (scope truncation by watermark)
  };
  std::vector<Ent> table;
  uint32_t seq_ctr = 0;
  // a Store kills: everything, if through an obscure handle; else within
  // its alloc: the exact key, every vreg-keyed entry (they may name the
  // stored slot), and every const key equal to the stored index constant
  auto kill_for_store = [&](uint32_t alloc, uint8_t ckey, val_t c) {
    size_t w = 0;
    for (size_t r = 0; r < table.size(); ++r) {
      const Ent &e = table[r];
      bool dead = false;
      if (e.alloc == alloc) {
        if (ckey)
          dead = (e.ckey ? e.c == c : true);
        else
          dead = true; // vreg index may equal any key's value
      }
      if (!dead)
        table[w++] = e;
    }
    table.resize(w);
  };
  auto push_ent = [&](Ent e) {
    e.seq = ++seq_ctr;
    table.push_back(e);
  };
  auto find_exact = [&](uint32_t alloc, uint8_t ckey, uint32_t v, val_t c,
                        uint32_t &val, bool &from_store) -> bool {
    for (const Ent &e : table)
      if (e.alloc == alloc && e.ckey == ckey && (ckey ? e.c == c : e.v == v)) {
        val = e.val;
        from_store = e.from_store;
        return true;
      }
    return false;
  };
  bool changed = false;
  auto rows = A.dom_tree.range();
  struct Fr {
    uint32_t b, ci, w; // w: seq watermark at entry
  };
  std::vector<Fr> st;
  st.push_back({0, 0, seq_ctr});
  while (!st.empty()) {
    uint32_t b = st.back().b;
    uint32_t ci = st.back().ci;
    uint32_t w = st.back().w;
    if (ci == 0) { // entering the block: scan its instrs once
      if (fn.preds[b].size() > 1)
        table.clear(); // join: memory state merges, drop everything
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        Instr &I = fn.code[i];
        if (I.k == K::Store) {
          bool single = I.a != NONE && I.a < nv && AI.single[I.a] != NONE;
          if (!single) {
            table.clear(); // may write ANY slot
            continue;
          }
          uint8_t ck = (I.b2 != NONE && I.b2 < nv && kc[I.b2]) ? 1 : 0;
          val_t cv = ck ? kv[I.b2] : 0;
          kill_for_store(AI.single[I.a], ck, cv);
          if (I.c2 != NONE && I.c2 < nv)
            push_ent({AI.single[I.a], ck, I.b2, cv, I.c2, true, 0});
        } else if (I.k == K::Load) {
          bool single = I.a != NONE && I.a < nv && AI.single[I.a] != NONE;
          if (!single)
            continue; // reads any slot: no kill, no key
          if (I.dst == NONE || I.dst >= nv)
            continue;
          uint8_t ck = (I.b2 != NONE && I.b2 < nv && kc[I.b2]) ? 1 : 0;
          val_t cv = ck ? kv[I.b2] : 0;
          uint32_t val = NONE;
          bool from_store = false;
          if (find_exact(AI.single[I.a], ck, I.b2, cv, val, from_store) &&
              val != I.dst) {
            // forward: the dominating def of `val' already produced (or
            // holds) this slot's value
            I.k = K::Copy;
            I.a = val;
            I.b2 = NONE;
            I.nt = false;
            if (from_store)
              os.ld_fwds++;
            else
              os.ld_cses++;
            changed = true;
          }
          // the slot's current value is now THIS dst (a copy of the
          // dominating value after a forward): later same-slot loads CSE
          // against it
          push_ent({AI.single[I.a], ck, I.b2, cv, I.dst, false, 0});
        } else if (I.k == K::Call) {
          if (!I.args.empty())
            table.clear();
        }
      }
    }
    auto ch = rows[b];
    if (ci < (uint32_t)ch.size()) {
      st.back().ci = ci + 1;
      uint32_t nb = (uint32_t)ch[ci];
      st.push_back({nb, 0, seq_ctr});
      continue;
    }
    // leaving b: drop everything created inside its subtree (seq > w)
    {
      size_t wi = 0;
      for (size_t r = 0; r < table.size(); ++r)
        if (table[r].seq <= w)
          table[wi++] = table[r];
      table.resize(wi);
    }
    st.pop_back();
  }
  return changed;
}

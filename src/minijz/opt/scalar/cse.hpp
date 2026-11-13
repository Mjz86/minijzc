#pragma once
// === cse.hpp ========================================
// dominator-scoped common subexpression elimination.
//
// ALGORITHM: opt_cse has three stages. (1) Seed kconst/kval from every
//   K::Const, so two temps holding 7 key by VALUE, not by vreg. (2) A
//   preorder walk of the dominator tree (A.dom_tree.range()) with an
//   explicit stack; entering a block, scan its instrs once: each pure
//   Bin (Div/Mod only when I.nt proves it safe) builds an Ent key from
//   resolve()-d operands, commutatives order operands by the `lt` tag;
//   a hit records ren[I.dst] = table[t].val and drop[i] = 1, a miss
//   pushes. (3) A second pass rewrites all operands (I.a/I.b2/I.c2,
//   I.args, Phi args included) with the completed map; the sweep then
//   rebuilds fn.code, rewriting each fn.blocks range.
// DATA STRUCTURES:
//   * ren + resolve: renaming map vreg -> dominating def (chains are
//     followed to the end).
//   * kconst/kval: const temps, keyed by VALUE for the Ent key.
//   * table of Ent {op,x,y,xv,yv,val}: the scoped CSE table; x/y NONE
//     means a const operand, compared by xv/yv (tables stay tiny).
//   * st of Fr {b,ci,mark}: the preorder stack; mark = table.size() at
//     entry, so leaving a block truncates via table.resize(mark).
//   * drop: per-instr bytes telling the sweep which instrs to omit.
// THEORY: visibility is exactly "def dominates use" -- table entries
//   live along the dominator path only, so a hit means the dominating
//   def computes the same pure value on every path. Renaming waits for
//   a SECOND pass: a Phi argument is a use on an INCOMING EDGE, and
//   the replaced def's block may be scanned after the phi's block in
//   preorder -- rewriting during the walk would leave the phi reading
//   a dropped def; ren[v] = w implies w's def dominates every use of v.
// ENABLES: duplicate Bins vanish and their operands unify on the
//   surviving def: later passes see fewer instrs and one name per
//   expression, with fn.blocks ranges rewritten in place.

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../analysis/an.hpp"

// --- 9a3. dominator-scoped common subexpression elimination
// ------------------- A pure, non-trapping Bin recomputed under a *dominating*
// identical Bin is replaced by the dominating def. Visibility is exactly "def
// dominates use", checked with the lib's dominance intervals: we walk the
// dominator tree (from the lib) in preorder, keeping a scoped table — entries
// pushed on block entry are truncated on exit. The renaming map is applied in a
// SECOND pass over the whole function after the walk completes: a Phi's
// argument is a use on an INCOMING EDGE, so the block defining the replaced
// vreg can be scanned AFTER the phi's own block in preorder (a merge block does
// not sit below the predecessor in the dominator tree) — rewriting during the
// walk would leave the phi pointing at a dropped def (a ghost vreg reading as
// 0).
constexpr bool opt_cse(Fn &fn, const An &A, OptStats &os) {
  if (A.n != fn.nblocks() || !fn.nblocks())
    return false;
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  std::vector<uint32_t> ren(nv, NONE);
  auto resolve = [&](uint32_t v) -> uint32_t {
    while (v != NONE && v < nv && ren[v] != NONE)
      v = ren[v];
    return v;
  };
  // known constants: two temps holding 7 are the SAME operand. CSE keys
  // compare constants by VALUE, not by vreg, so i*7 hits i*7.
  std::vector<char> kconst(nv, 0);
  std::vector<val_t> kval(nv, 0);
  for (const Instr &I : fn.code)
    if (I.k == K::Const && I.dst != NONE && I.dst < nv) {
      kconst[I.dst] = 1;
      kval[I.dst] = I.c;
    }
  struct Ent {
    B op;
    uint32_t x, y; // vreg operands (NONE if the operand is a known const)
    val_t xv, yv;  // const operand values
    uint32_t val;  // the defining vreg (the CSE replacement)
  };
  std::vector<Ent> table;
  struct Fr {
    uint32_t b, ci, mark;
  };
  std::vector<Fr> st;
  std::vector<char> drop(fn.code.size(), 0);
  bool changed = false;
  auto rows = A.dom_tree.range();
  st.push_back({0, 0, (uint32_t)table.size()});
  while (!st.empty()) {
    uint32_t b = st.back().b;
    uint32_t ci = st.back().ci;
    uint32_t mark = st.back().mark;
    if (ci == 0) { // entering the block: scan its instrs once
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        // resolved operands, LOCAL only (for the CSE key); the mutation
        // happens in the second pass, once the map is complete
        uint32_t ra = resolve(I.a), rb = resolve(I.b2);
        if (I.k != K::Bin || I.dst == NONE)
          continue;
        if (I.b == B::Div || I.b == B::Mod)
          if (!I.nt)
            continue; // may trap: keep (nt: proven safe, pure)
        Ent e{};
        e.op = I.b;
        e.x = ra;
        e.y = rb;
        if (e.x != NONE && e.x < nv && kconst[e.x]) {
          e.x = NONE;
          e.xv = kval[ra];
        }
        if (e.y != NONE && e.y < nv && kconst[e.y]) {
          e.y = NONE;
          e.yv = kval[rb];
        }
        bool comm = I.b == B::Add || I.b == B::Mul || I.b == B::Eq ||
                    I.b == B::Ne || I.b == B::And || I.b == B::Or ||
                    I.b == B::Xor || I.b == B::MulHi;
        auto lt = [](const Ent &e, int i) -> uint64_t { // orderable tag
          if (i == 0)
            return e.x == NONE ? ((uint64_t)1 << 63) ^ (uint64_t)e.xv
                               : (uint64_t)e.x;
          return e.y == NONE ? ((uint64_t)1 << 63) ^ (uint64_t)e.yv
                             : (uint64_t)e.y;
        };
        if (comm && lt(e, 1) < lt(e, 0)) {
          std::swap(e.x, e.y);
          std::swap(e.xv, e.yv);
        }
        e.val = I.dst;
        bool found = false;
        for (uint32_t t = 0; t < table.size(); ++t) // tables stay tiny
          if (table[t].op == e.op && table[t].x == e.x && table[t].y == e.y &&
              (e.x != NONE || table[t].xv == e.xv) &&
              (e.y != NONE || table[t].yv == e.yv)) {
            ren[I.dst] = table[t].val;
            found = true;
            break;
          }
        if (found) {
          drop[i] = 1;
          os.cses++;
          changed = true;
        } else {
          table.push_back(e);
        }
      }
    }
    auto ch = rows[b]; // children of b in the dominator tree (from the lib)
    if (ci < (uint32_t)ch.size()) {
      st.back().ci = ci + 1;
      uint32_t nb = (uint32_t)ch[ci];
      st.push_back({nb, 0, (uint32_t)table.size()});
      continue;
    }
    table.resize(mark); // leaving b: its entries go out of scope
    st.pop_back();
  }
  if (changed) {
    // second pass: rewrite EVERY operand (Phi args included) with the
    // COMPLETED map -- sound because ren[v] = w implies w's def dominates
    // v's def, which dominates every (edge-)use of v
    for (uint32_t b = 0; b < fn.nblocks(); ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        Instr &I = fn.code[i];
        if (I.a != NONE)
          I.a = resolve(I.a);
        if (I.b2 != NONE)
          I.b2 = resolve(I.b2);
        if (I.c2 != NONE)
          I.c2 = resolve(I.c2);
        for (uint32_t &av : I.args)
          av = resolve(av);
      }
    // sweep CSE'd instrs, preserving block ranges
    bool any_drop = false;
    for (char c : drop)
      any_drop |= (c != 0);
    if (any_drop) {
      std::vector<Instr> nc{};
      nc.reserve(fn.code.size());
      for (uint32_t b = 0; b < fn.nblocks(); ++b) {
        uint32_t ob = fn.blocks[b].begin, oe = fn.blocks[b].end;
        fn.blocks[b].begin = (uint32_t)nc.size();
        for (uint32_t i = ob; i < oe; ++i)
          if (!drop[i])
            nc.push_back(fn.code[i]);
        fn.blocks[b].end = (uint32_t)nc.size();
      }
      fn.code = std::move(nc);
    }
  }
  return changed;
}

// --- 9b. unreachable block removal + phi arg pruning -------------------------

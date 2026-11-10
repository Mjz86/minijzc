#pragma once
// === gvn.hpp ========================================
// dominator-based global value numbering -- the wishlist's "GVN proper".
//
// The local cse already unifies pure Bin recomputation under a dominating
// identical Bin; gvn generalizes the SAME dominator-scoped walk to a single
// value-number table with three entry kinds:
//
//   * Const -- two `t = 7` defs (one dominating the other) are ONE value:
//     the later def renames onto the earlier and dies. Unrolling and
//     splice litter blocks with duplicate constants; this folds them.
//   * Phi congruence -- two phis of the SAME block whose incoming maps
//     (per-pred resolved operands) agree are the same value: the later
//     renames onto the earlier and dies. Duplicated merge patterns (hand
//     written and unrolled) produce these; copyprop cannot see them.
//   * Bin -- exactly cse's rules (pure, non-trapping div/mod only with nt,
//     commutative canonicalization, constant operands keyed by value).
//
// Soundness notes (the cse skeleton, one table):
//   ren[v] = w implies w's def DOMINATES v's def (dom-tree preorder with
//   scoped truncation on exit), which dominates every edge-use of v -- so
//   the completed map can rewrite every operand of every instruction, Phi
//   arguments included, in a second pass. A phi pair is only unified when
//   neither references the other's result (mutually-referential header
//   pairs are left to phi-degen/sccp: conservative, never wrong). Const
//   defs that define a PARAMETER vreg (fnspec's clones materialize those)
//   are never touched -- params are defined "virtually".
//
#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../analysis/an.hpp"

// phi-congruence taint guard: does phi I (at instr index i, block starting
// at ob) reference the result of the phi `other`, or does `other` (an
// earlier phi of the same block) reference I's result? Either way the pair
// must not be unified (a rename would create a self-referential phi).
constexpr bool taint_pair(const Fn &fn, uint32_t ob, uint32_t i, const Instr &I,
                          uint32_t other) {
  if (I.a == other || I.b2 == other)
    return true; // I references the other's result
  for (uint32_t j = ob; j < i; ++j)
    if (fn.code[j].k == K::Phi && fn.code[j].dst == other)
      if (fn.code[j].a == I.dst || fn.code[j].b2 == I.dst)
        return true; // the other references I's result
  return false;
}

constexpr bool opt_gvn(Fn &fn, const An &A, OptStats &os) {
  if (A.n != fn.nblocks() || !fn.nblocks())
    return false;
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  std::vector<uint32_t> ren(nv, NONE);
  auto resolve = [&](uint32_t v) -> uint32_t {
    while (v != NONE && v < nv && ren[v] != NONE)
      v = ren[v];
    return v;
  };
  // known constants: operands key by VALUE, not by vreg (cse's rule), so
  // i*7 hits i*7 even when the 7s are different temps
  std::vector<char> kconst(nv, 0);
  std::vector<val_t> kval(nv, 0);
  for (const Instr &I : fn.code)
    if (I.k == K::Const && I.dst != NONE && I.dst < nv) {
      kconst[I.dst] = 1;
      kval[I.dst] = I.c;
    }
  // table entries: kind tag + key + the surviving defining vreg
  enum class EK : uint8_t { Const, Bin, Phi };
  struct Ent {
    EK k;
    B op;              // Bin only
    uint32_t x, y;     // Bin operands (NONE = constant operand); Phi: incomings
    val_t xv, yv;      // Bin constant operand values
    val_t c;           // Const value
    uint32_t val;      // the defining vreg (the replacement)
    uint32_t home;     // Phi only: the block the phi lives in
    uint32_t t{}, f{}; // Phi only: pred ids of the x/y incomings
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
      const uint32_t ob = fn.blocks[b].begin, oe = fn.blocks[b].end;
      for (uint32_t i = ob; i < oe; ++i) {
        const Instr &I = fn.code[i];
        if (I.dst == NONE || I.dst >= nv)
          continue;
        if (I.k == K::Const) {
          if (I.dst < fn.nparams)
            continue; // param-defining placeholder (fnspec clones): keep
          bool found = false;
          for (const Ent &t : table)
            if (t.k == EK::Const && t.c == I.c) {
              ren[I.dst] = t.val;
              found = true;
              break;
            }
          if (found) {
            drop[i] = 1;
            ++os.gvns;
            changed = true;
          } else {
            Ent e{};
            e.k = EK::Const;
            e.c = I.c;
            e.val = I.dst;
            e.home = b;
            table.push_back(e);
          }
          continue;
        }
        if (I.k == K::Phi) {
          // congruence: same incoming map as an EARLIER phi of THIS block.
          // (phis carry <= 2 incomings: I.a from pred I.t, I.b2 from pred
          // I.f -- I.args stays EMPTY for phis; it belongs to Call.) A
          // degenerate phi (a missing incoming, NONE) is phi-degen's
          // business, not ours. Neither phi may reference the other's
          // result: a mutual reference would fold into a self-referential
          // phi -- sound in the loop-header fixed-point case, but
          // phi-degen/sccp already own that pattern; stay conservative.
          if (I.a == NONE || I.b2 == NONE)
            continue;
          const uint32_t ra = resolve(I.a), rb = resolve(I.b2);
          bool found = false;
          for (const Ent &t : table) {
            if (t.k != EK::Phi || t.home != b || t.val == I.dst)
              continue;
            if (taint_pair(fn, ob, i, I, t.val))
              continue; // I references t's result, or t references I's
            if (ra == t.x && rb == t.y && I.t == t.t && I.f == t.f) {
              ren[I.dst] = t.val;
              found = true;
              break;
            }
          }
          if (found) {
            drop[i] = 1;
            ++os.gvns;
            changed = true;
          } else {
            Ent e{};
            e.k = EK::Phi;
            e.home = b;
            e.val = I.dst;
            e.x = ra; // resolved incoming from pred I.t
            e.y = rb; // resolved incoming from pred I.f
            e.t = I.t;
            e.f = I.f;
            table.push_back(e);
          }
          continue;
        }
        if (I.k != K::Bin)
          continue;
        if (I.b == B::Div || I.b == B::Mod)
          if (!I.nt)
            continue; // may trap: keep (nt: proven safe, pure)
        uint32_t ra = resolve(I.a), rb = resolve(I.b2);
        Ent e{};
        e.k = EK::Bin;
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
        auto lt = [](const Ent &e, int i) -> uint64_t {
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
        for (const Ent &t : table)
          if (t.k == EK::Bin && t.op == e.op && t.x == e.x && t.y == e.y &&
              (e.x != NONE || t.xv == e.xv) && (e.y != NONE || t.yv == e.yv)) {
            ren[I.dst] = t.val;
            found = true;
            break;
          }
        if (found) {
          drop[i] = 1;
          ++os.gvns;
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
    // v's def, which dominates every (edge-)use of v (cse's argument)
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
    // sweep dropped instrs, preserving block ranges
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

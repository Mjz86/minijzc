#pragma once
// === argpromo.hpp ========================================
// interprocedural argument promotion (array params -> scalar elements).

// ALGORITHM (opt_argpromo): (1) scan every K::Call into `sites`
//   ({f, i, g}). (2) decide, per callee g ascending under the growth
//   cap -- guards (tce_sel, slot_of, nparams, ARG_BODY_CAP, no
//   internal K::Call, no param redefinition, arity match), then
//   defiG (def-index map), the `fresh` greatest-fixpoint table of
//   provably-new allocations, and the first param p used ONLY by
//   Load(a == p, Const index) with sorted index set S (bounded by
//   ARG_ELEMS_CAP); other Load/Store must name fresh allocations;
//   `good` holds the sites whose argument resolves (resolve_len over
//   per-caller defmaps) to a constant-length Alloc with len > max(S).
//   (3) ONE clone: permutation mp drops p, adds k element params,
//   turns p's Loads into Copies of them (unresolved sites keep the
//   generic). (4) apply per caller in DESCENDING i order: 2k instrs
//   (Const + nt Load pairs) precede each reshaped, retargeted call.
// DATA STRUCTURES: the sites/PSite/RW vectors, defiG, the `fresh`
//   table, per-caller defmaps, and the ARG_BODY_CAP / ARG_ELEMS_CAP /
//   ARG_GROWTH_CAP budgets.
// THEORY: the materialized pre-call loads must equal what the callee
//   would have read: p is never stored through, every other Load/
//   Store in the callee names a provably fresh (non-aliasing)
//   allocation, and the callee makes no calls, so nothing can change
//   the array between call and read. len > max(S) makes each insert
//   provably in bounds (nt holds), and SSA dominance puts the Alloc
//   before the call -- values and trap order match. The caller
//   rewrite and the callee signature change are two halves of one
//   site-atomic edit: each call is reshaped and retargeted together,
//   so no mixed calling convention is ever observable.
// DOWNSTREAM: an array that stops escaping lifts in mem2reg; constant
//   element stores let constcall fold the promoted call; with every
//   site resolved the dead-fn sweep drops the uncalled generic.
#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"

// --- 9e7. ARGUMENT PROMOTION ----------------------------------------------
// A function whose array parameter is only READ at constant indices can
// take those elements as scalars instead: the callee's Loads become plain
// uses of the new element parameters, and every call site materializes
// the element loads right before the call. An array that stops escaping
// the caller's frame then lifts (mem2reg), and with constant element
// stores the promoted call itself folds at compile time (constcall) --
// the fuzzer's aread bait exercises exactly that chain.
//
// Soundness envelope (v1 -- deliberately narrow, airtight):
// * the promoted parameter's ONLY uses are Load.a with a Const-defined
//   index: anything else (Store/Len/Phi-arg/reassignment/arithmetic)
//   disqualifies, because the pre-call loads must equal what the callee
//   would have read -- the array must provably never CHANGE inside the
//   callee between the call and each read;
// * every OTHER Load/Store in the callee names a provably FRESH
//   allocation (Alloc / Copy chain / Phi of fresh handles): fresh
//   bump-arena slots never alias the caller's array, so no aliasing
//   mutation is possible. This also covers accidental int-as-handle
//   collisions, which this dynamically-typed surface language allows
//   (x[3] on an int is legal syntax);
// * the callee contains NO calls: a foreign handle reaching a call
//   argument could be stored through by the callee-of-the-callee (the
//   conservative v1 cut; the fixpoint extension is a known follow-up);
// * at EVERY promoted call site the argument resolves to a
//   constant-length allocation with len > max index (so the materialized
//   loads are provably in bounds: marked nt, and the trap ORDER is
//   preserved -- the original could not have trapped on these reads
//   either, wherever inside the callee they sat);
// * SSA dominance makes the resolution sound: the argument vreg's Alloc
//   dominates the call, so the allocation has executed and is live.
// One parameter per function (the first promotable one); ONE clone serves
// every resolvable site (the others keep calling the generic; when all
// sites resolve, the generic becomes uncalled and the dead-fn sweep right
// after drops it).
constexpr uint32_t ARG_BODY_CAP = 600;
constexpr uint32_t ARG_ELEMS_CAP = 6;
constexpr uint32_t ARG_GROWTH_CAP = 4000;
inline constexpr const char *const ARG_EL_NAMES[ARG_ELEMS_CAP] = {
    "e0", "e1", "e2", "e3", "e4", "e5"};

constexpr bool opt_argpromo(std::vector<Fn> &fns,
                            std::vector<std::string_view> &names,
                            std::vector<OptStats> &ostats,
                            std::vector<uint32_t> &src_index) {
  const uint32_t n0 = (uint32_t)fns.size();
  if (n0 < 2 || names.size() < n0 || ostats.size() < n0 ||
      src_index.size() < n0)
    return false;
  // ---- collect the call sites (f, instr index, callee) --------------------
  struct Site {
    uint32_t f, i, g;
  };
  std::vector<Site> sites{};
  for (uint32_t f = 0; f < n0; ++f) {
    const Fn &F = fns[f];
    for (uint32_t bi = 0; bi < F.nblocks(); ++bi)
      for (uint32_t i = F.blocks[bi].begin; i < F.blocks[bi].end; ++i)
        if (F.code[i].k == K::Call) {
          uint32_t g = (uint32_t)F.code[i].c;
          if (g != 0 && g < n0)
            sites.push_back({f, i, g});
        }
  }
  // the Const value behind an SSA vreg (params have no def -> false)
  auto def_const = [](const Fn &F, uint32_t v, val_t &out) -> bool {
    const uint32_t nv = F.nvars + (uint32_t)F.version_base.size();
    if (v == NONE || v >= nv)
      return false;
    for (const Instr &I : F.code)
      if (I.dst == v) {
        if (I.k == K::Const) {
          out = (val_t)I.c;
          return true;
        }
        return false;
      }
    return false;
  };
  // caller-side resolution: an argument vreg that is (a copy chain of) a
  // constant-length Alloc -- returns the element count
  auto resolve_len = [&](const Fn &F, const std::vector<uint32_t> &defiF,
                         uint32_t h, val_t &len) -> bool {
    const uint32_t nvF = F.nvars + (uint32_t)F.version_base.size();
    uint32_t v = h;
    for (int hop = 0; hop < 64; ++hop) { // Copy chains are short; stay safe
      if (v == NONE || v >= nvF || defiF[v] == NONE)
        return false;
      const Instr &D = F.code[defiF[v]];
      if (D.k == K::Alloc) {
        val_t L{};
        if (D.a == NONE || !def_const(F, D.a, L) || L < 1)
          return false;
        len = L;
        return true;
      }
      if (D.k == K::Copy) {
        v = D.a;
        continue;
      }
      return false; // Phi / Load / Call / ...: not provably one allocation
    }
    return false;
  };
  // a pending call-site rewrite (decide phase -> apply phase)
  struct RW {
    uint32_t f, i, g, p, clone;
    std::vector<val_t> S;
  };
  std::vector<RW> rws{};
  uint32_t growth = 0;
  for (uint32_t g = 1; g < n0 && growth < ARG_GROWTH_CAP; ++g) {
    const Fn G = fns[g]; // BY VALUE: the clone push_backs below reallocate
    if (G.tce_sel != 0 || !G.slot_of.empty() || G.nparams == 0 ||
        G.code.size() > ARG_BODY_CAP || G.nblocks() == 0)
      continue;
    // arity must match at every site (inconsistent programs: skip)
    uint32_t n_sites = 0;
    bool arity_bad = false;
    for (const Site &s : sites)
      if (s.g == g) {
        if (fns[s.f].code[s.i].args.size() != G.nparams) {
          arity_bad = true;
          break;
        }
        ++n_sites;
      }
    if (arity_bad || n_sites == 0)
      continue;
    // no calls inside the callee (v1 envelope)
    bool has_call = false;
    for (const Instr &I : G.code)
      if (I.k == K::Call) {
        has_call = true;
        break;
      }
    if (has_call)
      continue;
    // param redefinition paranoia (would break the vreg permutation)
    bool param_defined = false;
    for (const Instr &I : G.code)
      if (I.dst != NONE && I.dst < G.nparams) {
        param_defined = true;
        break;
      }
    if (param_defined) {
      continue;
    }
    // fresh-handle map: provably-new allocations (greatest fixpoint, so
    // Phi-of-fresh and Copy-of-fresh stay fresh; everything else is not)
    const uint32_t nvG = G.nvars + (uint32_t)G.version_base.size();
    std::vector<uint32_t> defiG(nvG, NONE);
    for (uint32_t i = 0; i < G.code.size(); ++i)
      if (G.code[i].dst != NONE && G.code[i].dst < nvG)
        defiG[G.code[i].dst] = i;
    std::vector<char> fresh(nvG, 1);
    for (uint32_t q = 0; q < G.nparams && q < nvG; ++q)
      fresh[q] = 0; // params are whatever the caller passed
    for (bool again = true; again;) {
      again = false;
      for (uint32_t v = 0; v < nvG; ++v) {
        if (!fresh[v] || defiG[v] == NONE)
          continue;
        const Instr &D = G.code[defiG[v]];
        bool f = false;
        if (D.k == K::Alloc)
          f = true;
        else if (D.k == K::Copy)
          f = (D.a != NONE && D.a < nvG) && fresh[D.a];
        else if (D.k == K::Phi) // both incoming values fresh -> fresh
          f = (D.a != NONE && D.a < nvG) && fresh[D.a] &&
              (D.b2 != NONE && D.b2 < nvG) && fresh[D.b2];
        if (!f) {
          fresh[v] = 0;
          again = true;
        }
      }
    }
    // find the first promotable parameter
    for (uint32_t p = 0; p < G.nparams; ++p) {
      // S: the constant indices read on p (each use must be such a Load)
      std::vector<val_t> S{};
      bool ok = true;
      for (const Instr &I : G.code) {
        bool touches_p =
            (I.a == p) || (I.b2 == p) || (I.c2 == p) || (I.dst == p);
        if (!touches_p) {
          for (uint32_t v : I.args)
            if (v == p)
              touches_p = true;
          if (!touches_p)
            continue;
        }
        if (I.k == K::Load && I.a == p && I.dst != NONE &&
            I.dst != p) { // the one allowed shape
          val_t idx{};
          if (def_const(G, I.b2, idx) && idx >= 0) {
            bool seen = false;
            for (val_t s : S)
              if (s == idx)
                seen = true;
            if (!seen)
              S.push_back(idx);
            continue;
          }
        }
        ok = false; // any other use of p disqualifies the parameter
        break;
      }
      if (!ok || S.empty() || S.size() > ARG_ELEMS_CAP)
        continue;
      // other Load/Store must name provably fresh allocations
      for (const Instr &I : G.code) {
        if ((I.k == K::Load || I.k == K::Store) && I.a != p) {
          if (I.a == NONE || I.a >= nvG || !fresh[I.a]) {
            ok = false;
            break;
          }
        }
      }
      if (!ok) {
        continue;
      }
      // sorted ascending indices + the max
      for (size_t x = 1; x < S.size(); ++x)
        for (size_t y = x; y > 0 && S[y - 1] > S[y]; --y) {
          val_t t = S[y - 1];
          S[y - 1] = S[y];
          S[y] = t;
        }
      const val_t maxs = S.back();
      // ---- caller-side: which sites resolve to a long-enough array -------
      struct PSite {
        uint32_t idx; // index into sites
      };
      std::vector<PSite> good{};
      {
        // per-caller def maps, computed once per distinct caller
        std::vector<std::pair<uint32_t, std::vector<uint32_t>>> defmaps{};
        for (size_t si = 0; si < sites.size(); ++si) {
          const Site &s = sites[si];
          if (s.g != g)
            continue;
          const Fn &F = fns[s.f];
          const uint32_t nvF = F.nvars + (uint32_t)F.version_base.size();
          std::vector<uint32_t> *df = nullptr;
          for (auto &dm : defmaps)
            if (dm.first == s.f) {
              df = &dm.second;
              break;
            }
          if (!df) {
            defmaps.push_back({s.f, std::vector<uint32_t>(nvF, NONE)});
            df = &defmaps.back().second;
            const Fn &F2 = fns[s.f];
            for (uint32_t i = 0; i < F2.code.size(); ++i)
              if (F2.code[i].dst != NONE && F2.code[i].dst < nvF)
                (*df)[F2.code[i].dst] = i;
          }
          const Instr &C = F.code[s.i];
          if (C.args.size() != G.nparams || p >= C.args.size())
            continue; // belt+braces
          val_t len{};
          if (resolve_len(F, *df, C.args[p], len) && len > maxs)
            good.push_back({(uint32_t)si});
        }
      }
      if (good.empty()) {
        continue; // nothing to promote for this parameter
      }
      const uint32_t k = (uint32_t)S.size();
      if (growth + (uint32_t)G.code.size() + 2 * k * (uint32_t)good.size() >
          ARG_GROWTH_CAP)
        continue;
      // ---- build the clone ------------------------------------------------
      // vreg permutation: param p is dropped (params above it shift down),
      // k element params take ids nparams-1 .. nparams-2+k, and every vreg
      // >= nparams shifts up by k-1. version_base/version_no entries keep
      // their INDICES (temps and nvars shift by the same k-1), only the
      // base VALUES are remapped.
      auto mp = [&](uint32_t x) -> uint32_t {
        if (x == NONE || x == p)
          return NONE; // p's uses are all rewritten away; NONE stays NONE
        if (x < G.nparams)
          return (x < p) ? x : x - 1;
        return x + k - 1;
      };
      Fn H = G; // whole-fn copy (code/blocks/succ/preds/versions/arrays)
      H.nvars = G.nvars - 1 + k;
      H.nparams = G.nparams - 1 + k;
      for (uint32_t &bv : H.version_base) // SSA base tags follow the space
        if (bv != NONE)
          bv = mp(bv);
      if (G.var_names.size() == G.nvars) { // names: params, elements, rest
        std::vector<std::string_view> nn{};
        nn.reserve(G.nvars - 1 + k);
        for (uint32_t q = 0; q < G.nparams; ++q)
          if (q != p)
            nn.push_back(G.var_names[q]);
        for (uint32_t j = 0; j < k; ++j)
          nn.push_back(ARG_EL_NAMES[j]);
        for (uint32_t v = G.nparams; v < G.nvars; ++v)
          nn.push_back(G.var_names[v]);
        H.var_names = std::move(nn);
      }
      // body rewrite: p's Loads become Copies of the element params
      for (uint32_t bi = 0; bi < H.nblocks(); ++bi)
        for (uint32_t i = H.blocks[bi].begin; i < H.blocks[bi].end; ++i) {
          Instr &I = H.code[i];
          if (I.k == K::Load && I.a == p) { // the promoted read
            val_t idx{};
            (void)def_const(G, I.b2, idx); // checked above: always true
            uint32_t j = 0;                // position in the sorted S
            while (j < S.size() && S[j] != idx)
              ++j;
            const uint32_t ep = (G.nparams - 1) + j; // element param id
            const uint32_t nd = (I.dst == NONE) ? NONE : mp(I.dst);
            const uint32_t keep_src = I.src; // keep the source annotation
            I = Instr{}; // wipe: a plain Copy of the element parameter
            I.k = K::Copy;
            I.dst = nd;
            I.a = ep;
            I.src = keep_src;
            continue;
          }
          if (I.dst != NONE)
            I.dst = mp(I.dst);
          if (I.a != NONE)
            I.a = mp(I.a);
          if (I.b2 != NONE)
            I.b2 = mp(I.b2);
          if (I.c2 != NONE)
            I.c2 = mp(I.c2);
          for (uint32_t &av : I.args)
            av = mp(av); // (no calls in G; paranoia)
        }
      { // stk_arrays vreg ids follow the permutation
        std::vector<uint32_t> nb{};
        for (auto &sa : H.stk_arrays) {
          sa.root = mp(sa.root);
          nb.clear();
          for (uint32_t u : sa.vregs)
            nb.push_back(mp(u));
          sa.vregs = std::move(nb);
        }
      }
      const uint32_t h = (uint32_t)fns.size();
      fns.push_back(std::move(H));
      names.push_back(names[g]); // same source name (indices disambiguate)
      ostats.push_back(OptStats{});
      src_index.push_back(src_index[g]); // pre-opt SSA = the original's
      growth += (uint32_t)fns[h].code.size() + 2 * k * (uint32_t)good.size();
      for (const PSite &ps : good)
        rws.push_back({sites[ps.idx].f, sites[ps.idx].i, g, p, h, S});
      break; // one parameter per fn: no combinatorial cloning
    }
  }
  if (rws.empty())
    return false;
  // ---- apply: per caller, descending instr index (insertions shift the
  // tail only), 2k fresh instrs before each call (k Consts + k nt Loads)
  std::sort(rws.begin(), rws.end(), [](const RW &x, const RW &y) {
    if (x.f != y.f)
      return x.f < y.f;
    return x.i > y.i;
  });
  for (const RW &r : rws) {
    Fn &F = fns[r.f];
    if (r.i >= F.code.size())
      continue;
    const uint32_t h2v = [&]() -> uint32_t {
      const Instr &C = F.code[r.i];
      if (C.k != K::Call || (uint32_t)C.c != r.g ||
          C.args.size() != fns[r.g].nparams || r.p >= C.args.size())
        return NONE; // belt+braces: the site must be unchanged
      return C.args[r.p];
    }();
    if (h2v == NONE) {
      continue;
    }
    const uint32_t k = (uint32_t)r.S.size();
    const uint32_t base = F.base_of(h2v);  // readable temp names
    const uint32_t csrc = F.code[r.i].src; // annotate with the call's site
    std::vector<Instr> ins{};
    ins.reserve(2 * k);
    std::vector<uint32_t> elems{};
    elems.reserve(k);
    for (uint32_t j = 0; j < k; ++j) {
      const uint32_t cv = new_anon_vreg(F, base);
      ins.push_back(
          {.k = K::Const, .dst = cv, .c = (int64_t)r.S[j], .src = csrc});
      const uint32_t lv = new_anon_vreg(F, base);
      // provably in bounds: one constant-length allocation + constant
      // index < len (the site-resolution precondition), so nt holds and
      // DCE may drop the load if the callee ignores the element
      ins.push_back({.k = K::Load,
                     .dst = lv,
                     .a = h2v,
                     .b2 = cv,
                     .nt = true,
                     .src = csrc});
      elems.push_back(lv);
    }
    F.code.insert(F.code.begin() + r.i, ins.begin(), ins.end());
    for (Block &B : F.blocks) { // ranges after the insertion point shift
      if (B.begin > r.i) {
        B.begin += 2 * k;
        B.end += 2 * k;
      } else if (B.end > r.i) { // the call's own block grows at the tail
        B.end += 2 * k;
      }
    }
    Instr &C2 = F.code[r.i + 2 * k]; // the call, now past the loads
    std::vector<uint32_t> keep{};
    keep.reserve(C2.args.size() - 1 + k);
    for (uint32_t q = 0; q < C2.args.size(); ++q)
      if (q != r.p)
        keep.push_back(C2.args[q]);
    for (uint32_t e : elems)
      keep.push_back(e); // element params come last in the signature
    C2.args = std::move(keep);
    C2.c = (int64_t)r.clone;
    // count on the CALLER: the clone is tiny and typically inlined away
    // right after (its own ostats would die with it)
    ostats[r.f].argpromos++;
  }
  return true;
}

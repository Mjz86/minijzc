#pragma once
// === fnspec.hpp ========================================
// interprocedural function specialization (partial evaluation of a
// constant argument without inlining).

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"

// --- 9e5b. FUNCTION SPECIALIZATION (interprocedural) -------------------
// When EVERY call site of a function passes a CONSTANT in some parameter
// slot, the parameter can be bound inside a specialized clone: the clone
// drops the parameter from its signature, its uses are re-pointed at a
// fresh vreg defined by a `Const` at the entry (SSA-clean: the param had
// no other defs), and the call sites drop the argument. The tier-0 re-fold
// of the final optimize round then propagates the constant through the
// clone (branch pruning, divmagic, the works) WITHOUT duplicating the
// body into every caller the way inlining would.
//
// Distinct argument values get distinct clones (2+ values: one clone per
// value; the generic survives only if still called -- recursion keeps it
// alive, since a clone's internal calls target the generic). One value
// covering all sites: the generic becomes uncalled and the dead-fn sweep
// right after drops it.
//
// Only ONE parameter is specialized per function (the first all-const
// one): multi-param specialization would multiply clones combinatorially
// for diminishing returns.
//
// Guards (any hit disqualifies the fn): main; tailrec merge products
// (tce_sel != 0); post-destruction shapes (slot_of set); bodies over
// SPEC_BODY_CAP instrs; entry block not at code position 0 or containing
// phis (the entry Const prepend below needs both); any instruction
// DEFINING a param vreg (SSA violation paranoia); arity-mismatched call
// sites (inconsistent program -- leave untouched); fewer than 2 call
// sites (one site is the inliner's job, not a cloning job); more than
// SPEC_VALUES_CAP distinct values; program growth beyond SPEC_GROWTH_CAP
// added instrs.
constexpr uint32_t SPEC_BODY_CAP = 600;
constexpr uint32_t SPEC_VALUES_CAP = 4;
constexpr uint32_t SPEC_GROWTH_CAP = 4000;

// the defining Const of an SSA vreg in F, or NONE (params: no def)
constexpr bool spec_def_const(const Fn &F, uint32_t v, val_t &out) {
  const uint32_t nv = F.nvars + (uint32_t)F.version_base.size();
  if (v == NONE || v >= nv)
    return false;
  for (const Instr &I : F.code)
    if (I.dst == v) {
      if (I.k == K::Const) {
        out = (val_t)I.c;
        return true;
      }
      return false; // defined, but not by a Const
    }
  return false;
}

constexpr bool opt_fnspec(std::vector<Fn> &fns,
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
  bool any = false;
  uint32_t growth = 0;
  for (uint32_t g = 1; g < n0; ++g) { // ascending: appended clones keep ids
    if (growth >= SPEC_GROWTH_CAP)
      break;
    // BY VALUE: the clone push_backs below reallocate `fns`, so a reference
    // into it would dangle for the second clone of the same fn
    const Fn G = fns[g];
    if (G.tce_sel != 0 || !G.slot_of.empty() || G.nparams == 0 ||
        G.code.size() > SPEC_BODY_CAP || G.nblocks() == 0)
      continue;
    if (G.blocks[0].begin != 0) // the entry-Const prepend below needs this
      continue;
    bool entry_phi = false; // entry blocks never carry phis; paranoia
    for (uint32_t i = G.blocks[0].begin; i < G.blocks[0].end; ++i)
      if (G.code[i].k == K::Phi) {
        entry_phi = true;
        break;
      }
    if (entry_phi)
      continue;
    // any instruction DEFINING a param vreg (SSA violation paranoia --
    // also fatal for the permutation below, which assumes params have no
    // internal defs) disqualifies the whole fn
    bool param_defined = false;
    for (const Instr &I : G.code)
      if (I.dst != NONE && I.dst < G.nparams) {
        param_defined = true;
        break;
      }
    if (param_defined)
      continue;
    // this fn's sites (arity must match at every one of them)
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
    if (arity_bad || n_sites < 2)
      continue;
    // find the first parameter that is CONST at every site and USED in g
    for (uint32_t p = 0; p < G.nparams; ++p) {
      // used in the body?
      bool used = false;
      for (const Instr &I : G.code) {
        for (uint32_t v : {I.a, I.b2, I.c2})
          if (v == p)
            used = true;
        for (uint32_t v : I.args)
          if (v == p)
            used = true;
      }
      if (!used)
        continue;
      // all sites const?
      std::vector<std::pair<uint32_t, val_t>> vals{}; // (site idx, value)
      bool all_const = true;
      for (size_t si = 0; si < sites.size(); ++si) {
        const Site &s = sites[si];
        if (s.g != g)
          continue;
        const Instr &C = fns[s.f].code[s.i];
        val_t v{};
        if (!spec_def_const(fns[s.f], C.args[p], v)) {
          all_const = false;
          break;
        }
        vals.push_back({(uint32_t)si, v});
      }
      if (!all_const || vals.size() < 2)
        continue;
      // distinct values (order of first appearance)
      std::vector<val_t> dv{};
      for (const auto &sv : vals) {
        bool seen = false;
        for (val_t d : dv)
          if (d == sv.second)
            seen = true;
        if (!seen)
          dv.push_back(sv.second);
      }
      if (dv.empty() || dv.size() > SPEC_VALUES_CAP)
        continue;
      if (growth + (uint32_t)dv.size() * (uint32_t)G.code.size() >
          SPEC_GROWTH_CAP)
        continue;
      // ---- build one clone per distinct value ---------------------------
      for (val_t v : dv) {
        Fn H = G; // whole-fn copy (code/blocks/succ/preds/versions/arrays)
        // base-id space FIRST: params above p shift down into the freed
        // slot, vacating base id nparams-1 (the dropped prefix slot's
        // neighbor) -- the bound constant's fresh vreg claims THAT base,
        // so no two vregs share a base var
        for (uint32_t &b : H.version_base) // SSA versions' base tags shift
          if (b != NONE && p < b && b < G.nparams)
            b = b - 1;
        std::string_view bound_name{};
        if (H.var_names.size() == H.nvars && p < H.var_names.size()) {
          bound_name = H.var_names[p];
          for (uint32_t q = G.nparams - 1; q > p; --q) // names shift down;
            H.var_names[q - 1] = H.var_names[q];       // nparams-1 is vacated
          H.var_names[G.nparams - 1] = bound_name;     // the bound param's name
        }
        // t: fresh vreg holding the bound constant (SSA-version id space),
        // based at the vacated nparams-1
        uint32_t t = new_anon_vreg(H, G.nparams - 1);
        // vreg permutation: param p -> t; params above p shift down into
        // the freed prefix slot; everything else (locals, temps, SSA
        // versions, the fresh t) is untouched
        auto mp = [&](uint32_t x) -> uint32_t {
          if (x == p)
            return t;
          if (x != NONE && p < x && x < G.nparams)
            return x - 1;
          return x;
        };
        for (uint32_t bi = 0; bi < H.nblocks(); ++bi)
          for (uint32_t i = H.blocks[bi].begin; i < H.blocks[bi].end; ++i) {
            Instr &I = H.code[i];
            if (I.dst != NONE)
              I.dst = mp(I.dst);
            if (I.a != NONE)
              I.a = mp(I.a);
            if (I.b2 != NONE)
              I.b2 = mp(I.b2);
            if (I.c2 != NONE)
              I.c2 = mp(I.c2);
            for (uint32_t &a : I.args)
              a = mp(a);
            // Phi's stale base-var tag intentionally left as-is (unread
            // post-ssa_construct, same policy as deadargs)
          }
        {
          std::vector<uint32_t> nb{}; // stk_arrays vregs remap
          for (auto &sa : H.stk_arrays) {
            sa.root = mp(sa.root);
            nb.clear();
            for (uint32_t u : sa.vregs)
              nb.push_back(mp(u));
            sa.vregs = std::move(nb);
          }
        }
        H.nparams -= 1;
        // entry Const: prepend (block 0 sits at code position 0 -- guarded).
        // Block 0 ABSORBS the Const (begin stays 0, end grows); every other
        // block's range shifts by one.
        {
          std::vector<Instr> nc{};
          nc.reserve(H.code.size() + 1);
          nc.push_back({.k = K::Const, .dst = t, .c = (int64_t)v});
          for (const Instr &I : H.code)
            nc.push_back(I);
          H.code = std::move(nc);
          for (uint32_t bi = 0; bi < H.blocks.size(); ++bi) {
            if (bi != 0)
              H.blocks[bi].begin += 1;
            H.blocks[bi].end += 1;
          }
        }
        uint32_t h = (uint32_t)fns.size();
        fns.push_back(std::move(H));
        names.push_back(names[g]); // same source name (indices disambiguate)
        ostats.push_back(OptStats{});
        src_index.push_back(src_index[g]); // pre-opt SSA = the original's
        ostats[h].fnspecs = 1;
        growth += (uint32_t)fns[h].code.size();
        // redirect the sites carrying THIS value: drop the arg, retarget
        for (const auto &sv : vals) {
          if (sv.second != v)
            continue;
          const Site &s = sites[sv.first];
          Instr &C = fns[s.f].code[s.i];
          if (C.k != K::Call || (uint32_t)C.c != g)
            continue; // belt+braces: the site must be unchanged
          std::vector<uint32_t> keep{};
          keep.reserve(C.args.size());
          for (uint32_t q = 0; q < C.args.size(); ++q)
            if (q != p)
              keep.push_back(C.args[q]);
          C.args = std::move(keep);
          C.c = (int64_t)h;
        }
        any = true;
      }
      break; // one parameter per fn: no combinatorial cloning
    }
  }
  return any;
}

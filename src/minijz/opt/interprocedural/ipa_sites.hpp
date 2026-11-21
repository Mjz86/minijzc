#pragma once
// === ipa_sites.hpp ========================================
// the shared interprocedural glue (the ckpt-31 dedup wave): argpromo and
// fnspec hand-rolled identical copies of the call-site census, the
// Const-value-behind-a-vreg lookup and the per-callee arity gate until
// they were lifted here. The passes' POLICIES stay theirs (which callee
// qualifies, how many sites are worth a clone, the growth caps); the
// census and the lookups are the same algorithm.
#include "../../ir/instr.hpp"

#include <utility>
#include <vector>

// one call site: caller fn, instr index, callee
struct IpaSite {
  uint32_t f, i, g;
};

// every Call in fns[0..n0) to a callee in the same window (the clones
// appended beyond n0 are NOT scanned -- each pass census runs on the
// original program)
constexpr std::vector<IpaSite>
ipa_collect_call_sites(const std::vector<Fn> &fns, uint32_t n0) {
  std::vector<IpaSite> sites{};
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
  return sites;
}

// the defining Const of an SSA vreg in F (params have no def -> false);
// a vreg defined by a non-Const is NOT a known constant
constexpr bool ipa_def_const(const Fn &F, uint32_t v, val_t &out) {
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

// g's site count + whether ANY site's arity disagrees with nparams --
// the caller applies its own acceptance gate (argpromo: >= 1 site;
// fnspec: >= 2)
constexpr std::pair<uint32_t, bool>
ipa_count_sites_of(const std::vector<Fn> &fns,
                   const std::vector<IpaSite> &sites, uint32_t g,
                   uint32_t nparams) {
  uint32_t n_sites = 0;
  bool arity_bad = false;
  for (const IpaSite &s : sites)
    if (s.g == g) {
      if (fns[s.f].code[s.i].args.size() != nparams) {
        arity_bad = true;
        break;
      }
      ++n_sites;
    }
  return {n_sites, arity_bad};
}

#pragma once
// === icf.hpp ========================================
// interprocedural identical code folding (ICF).

// ALGORITHM (opt_icf, whole program, four phases):
//   * candidacy: cand[f] is cleared for main (f==0), tailrec merge
//     products (tce_sel != 0), post-destruction shapes (slot_of set)
//     and any body containing K::Depth (frame-count dependent).
//   * seeding: icf_serialize builds each fn's key with EVERY callee
//     wildcarded (ICF_WILD); icf_group merges candidates into classes
//     by first-equal-key, rep pointing every member at its class head.
//   * refinement (8 rounds max): re-serialize with callees encoded as
//     ICF_SELF for self-calls, literal for out-of-range indices, and
//     rep[c] for the callee's CURRENT class representative; icf_group
//     splits classes whose keys diverge. Folding happens only after a
//     round that changed nothing (verified fixpoint); reaching the
//     cap still changing declines the whole pass (return false).
//   * redirect: every K::Call to a non-representative member is
//     retargeted to rep[c]; ostats[rep[f]].icf counts each fold.
// DATA STRUCTURES: keys[f], the vector<uint64_t> canonical serial
//   (nparams/nvars/versions/blocks/succ/preds/stk_arrays plus every
//   Instr field except src), matched by direct equality in icf_group
//   (keys[g] == keys[f], no hashing); the cand eligibility table; the
//   rep representative map; the ICF_WILD/ICF_SELF sentinels.
// THEORY: two bodies with equal keys are structurally identical
//   modulo the callee mapping, so every value, trap and allocation
//   either can produce is the same -- a caller cannot tell a class
//   member from its representative, making the I.c redirect inert.
//   Wildcard seeding can only over-merge, and refinement only splits
//   (equal refined keys imply equal wildcard keys), so the fixpoint
//   classes are exactly the mutually-equivalent sets -- mutual
//   recursion pairs A<->B / C<->D fold this way.
// DOWNSTREAM: the true return tells the driver to run the dead-fn
//   sweep, which deletes the folded duplicates.
#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"

// --- 9e6. IDENTICAL CODE FOLDING (interprocedural) ---------------------
// Functions with structurally identical bodies (identical instructions,
// vreg layouts, CFG, arrays) are interchangeable, so every call to a
// non-representative member can be redirected to the class representative
// and the duplicate deleted by the dead-function sweep that follows.
//
// Equivalence is decided by partition refinement over canonical serial
// keys:
//   * the key serializes EVERYTHING semantic (nparams/nvars/versions/
//     blocks/succ/preds/stk_arrays + every instruction field incl. `nt`
//     and Phi's stale `c` tag) as a uint64 vector;
//   * Call targets cannot be serialized as raw indices (two identical
//     bodies may call different-but-equivalent callees), so a call is
//     encoded as SELF for self-calls and as the CURRENT class
//     representative of the callee otherwise;
//   * round 0 seeds classes with callees wildcarded, then rounds
//     re-serialize with concrete representatives and SPLIT classes whose
//     keys diverge (Hopcroft-style refinement). Folding happens only at a
//     VERIFIED fixpoint (a round that changed nothing); if no fixpoint is
//     reached within the round cap the pass declines to fold anything.
//   * sources (Instr::src) are NOT part of the key: two identical bodies
//     written at different source positions fold. So are names.
//
// This catches the plain duplicates (the same helper typed twice, twin
// arms of generated code) AND the mutual-recursion pairs A<->B / C<->D
// (wildcard seeding merges them, the refinement re-verifies with the
// concrete representatives A and B, and C/D fold away).
//
// Guards (fold candidates only): never main, never tailrec merge products
// (tce_sel != 0: param 0 is the sel dispatch), never post-destruction
// shapes (slot_of set), never bodies containing K::Depth (frame-count
// dependent; conservative). Non-candidates keep class-of-one and stay
// valid fold TARGETS? No -- they are never folded INTO (their key is
// simply never compared), keeping their special invariants untouched.
constexpr uint64_t ICF_WILD = 0xFFFFFFFFFFFFFFFEull; // seeding wildcard
constexpr uint64_t ICF_SELF = 0xFFFFFFFFFFFFFFFDull; // self-call marker

// serialize one fn's semantics; cm(c) maps a callee index to its encoded
// key contribution (SELF / representative / wildcard)
template <class CM>
constexpr void icf_serialize(const Fn &G, uint32_t self, CM &&cm,
                             std::vector<uint64_t> &k) {
  const uint32_t nb = G.nblocks();
  k.push_back(G.nparams);
  k.push_back(G.nvars);
  k.push_back((uint64_t)G.version_base.size());
  k.push_back(nb);
  for (const Block &b : G.blocks) {
    k.push_back(b.begin);
    k.push_back(b.end);
  }
  for (uint32_t s : G.succ)
    k.push_back(s);
  k.push_back((uint64_t)G.preds.size());
  for (const auto &p : G.preds) {
    k.push_back((uint64_t)p.size());
    for (uint32_t q : p)
      k.push_back(q);
  }
  for (uint32_t b : G.version_base)
    k.push_back(b);
  for (uint32_t v : G.version_no)
    k.push_back(v);
  k.push_back((uint64_t)G.stk_arrays.size());
  for (const auto &sa : G.stk_arrays) {
    k.push_back(sa.root);
    k.push_back((uint64_t)sa.len);
    k.push_back((uint64_t)sa.vregs.size());
    for (uint32_t v : sa.vregs)
      k.push_back(v);
  }
  for (const Instr &I : G.code) {
    k.push_back((uint64_t)I.k);
    k.push_back((uint64_t)I.b);
    k.push_back(I.dst);
    k.push_back(I.a);
    k.push_back(I.b2);
    k.push_back(I.t);
    k.push_back(I.f);
    // Call: the callee is context, not content -- map through cm; Phi's c
    // (stale base-var tag) is serialized verbatim: conservative
    k.push_back(I.k == K::Call ? (uint64_t)cm((uint32_t)I.c)
                               : (uint64_t)(uint64_t)(int64_t)I.c);
    k.push_back(I.c2);
    k.push_back(I.nt ? 1u : 0u);
    k.push_back((uint64_t)I.args.size());
    for (uint32_t a : I.args)
      k.push_back(a);
    // I.src deliberately excluded: a source location is not semantics
  }
}

// one refinement round: keys[k] are precomputed; group candidates by
// first-equal-key; returns the new representative map
constexpr bool icf_group(const std::vector<uint64_t> *keys,
                         const std::vector<char> &cand,
                         std::vector<uint32_t> &rep) {
  const uint32_t n = (uint32_t)rep.size();
  std::vector<uint32_t> nrep(n);
  for (uint32_t f = 0; f < n; ++f)
    nrep[f] = f;
  bool changed = false;
  for (uint32_t f = 0; f < n; ++f) {
    if (!cand[f])
      continue;
    for (uint32_t g = 0; g < f; ++g) { // first candidate with an equal key
      if (!cand[g])
        continue;
      if (keys[g] == keys[f]) {
        nrep[f] = nrep[g];
        break;
      }
    }
    changed |= (nrep[f] != rep[f]);
  }
  rep = std::move(nrep);
  return changed;
}

constexpr bool opt_icf(std::vector<Fn> &fns, std::vector<OptStats> &ostats) {
  const uint32_t n = (uint32_t)fns.size();
  if (n < 2 || ostats.size() < n)
    return false;
  // ---- candidacy ----------------------------------------------------------
  std::vector<char> cand(n, 0);
  for (uint32_t f = 0; f < n; ++f) {
    const Fn &G = fns[f];
    if (f == 0 || G.tce_sel != 0 || !G.slot_of.empty())
      continue;
    bool depth = false;
    for (const Instr &I : G.code)
      if (I.k == K::Depth) {
        depth = true;
        break;
      }
    cand[f] = depth ? 0 : 1;
  }
  // ---- seeding: wildcard callees ------------------------------------------
  std::vector<uint32_t> rep(n);
  for (uint32_t f = 0; f < n; ++f)
    rep[f] = f;
  {
    std::vector<std::vector<uint64_t>> keys(n);
    for (uint32_t f = 0; f < n; ++f)
      icf_serialize(fns[f], f, [](uint32_t) { return ICF_WILD; }, keys[f]);
    (void)icf_group(keys.data(), cand, rep);
  }
  // ---- refinement: concrete representatives, split on divergence ---------
  for (int round = 0; round < 8; ++round) {
    std::vector<std::vector<uint64_t>> keys(n);
    for (uint32_t f = 0; f < n; ++f)
      icf_serialize(
          fns[f], f,
          [f, &rep, n](uint32_t c) -> uint64_t {
            if (c >= n)
              return (uint64_t)c; // out-of-range callee: literal (never folds)
            if (c == f)
              return ICF_SELF;       // self-recursion: context-free marker
            return (uint64_t)rep[c]; // callee -> its class representative
          },
          keys[f]);
    bool changed = icf_group(keys.data(), cand, rep);
    if (!changed)
      break; // verified fixpoint
    if (round == 7)
      return false; // no fixpoint within the cap: decline to fold
  }
  // ---- redirect every call to the representative -------------------------
  // (all members of a class behave identically, so callers cannot tell)
  uint32_t folded = 0;
  for (uint32_t f = 0; f < n; ++f)
    if (rep[f] != f)
      ++folded;
  if (!folded)
    return false;
  for (uint32_t f = 0; f < n; ++f) {
    Fn &F = fns[f];
    for (uint32_t bi = 0; bi < F.nblocks(); ++bi)
      for (uint32_t i = F.blocks[bi].begin; i < F.blocks[bi].end; ++i) {
        Instr &I = F.code[i];
        if (I.k != K::Call)
          continue;
        uint32_t c = (uint32_t)I.c;
        if (c >= n || rep[c] == c)
          continue;
        I.c = (int64_t)rep[c];
      }
  }
  for (uint32_t f = 0; f < n; ++f)
    if (rep[f] != f)
      ostats[rep[f]].icf += 1; // folded INTO the representative
  return true;                 // caller follows up with eliminate_dead_fns
}

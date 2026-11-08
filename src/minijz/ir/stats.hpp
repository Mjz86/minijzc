#pragma once
// === stats.hpp ========================================
// the pipeline's per-function statistics records. Standalone and LIGHT
// (no mjz_lib, no IR containers): Program/FnUnit hold these by value, so
// anything that touches Program must be able to name them without pulling
// the analysis machinery that PRODUCES them (an.hpp is the heavy side).

#include <cstdint>

struct SsaStats {
  uint32_t phis{};     // phi instructions placed by SSA construction
  uint32_t versions{}; // fresh SSA versions minted
};
struct DestrStats {
  uint32_t edges_split{};   // critical edges split
  uint32_t copies{};        // sequentialized edge copies emitted
  uint32_t zeros{};         // undef phi args materialized as Const 0
  uint32_t self_copies{};   // d<-d no-ops skipped
  uint32_t cycles_broken{}; // swap cycles broken with a temp
  uint32_t fused{};         // post-destruction copies folded into their source
};

#pragma once
// === graph_lib.hpp ========================================
// the mjz_lib import, isolated: only headers that actually need the
// graph/big-number library include this (analysis, callgraph, divmagic,
// regalloc). The rest of the compiler stays independent of it, so the
// multi-cpp build only pays for mjz_lib in the TUs that use it.

#define MJZ_WITH_iostream false
#define MJZ_TEST_MODE_ 1
#define MJZ_LOG_NEW_ALLOCATIONS_ 0
#define MJZ_aggregate_tuple_workaround false
#include "../mjz_lib/alllib_import.hpp"

using namespace mjz;
namespace gns = mjz::graph_ns;
inline constexpr version_t V{};
using ulen = uintlen_t;

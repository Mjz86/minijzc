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

// the generic-folder families -- every mjz_lib addition this repo
// carries lives in generic/ written as if it was in mjz_lib
// (byte-mergeable upstream; the vendored String clone above stays
// pristine -- see generic/README.md). The graph list mirrors
// merge-patch 0002's algo.hpp block; opt/alllib.hpp is the aggregate
// patch 0001's hook would pull. The quoted paths resolve rule-#1 from
// this directory, same as the bridge above them.
#include "../../generic/mjz_lib/graph/biconnect.hpp"
#include "../../generic/mjz_lib/graph/cut.hpp"
#include "../../generic/mjz_lib/graph/distances.hpp"
#include "../../generic/mjz_lib/graph/euler.hpp"
#include "../../generic/mjz_lib/graph/flow.hpp"
#include "../../generic/mjz_lib/graph/inverse.hpp"
#include "../../generic/mjz_lib/graph/matching.hpp"
#include "../../generic/mjz_lib/graph/mst.hpp"
#include "../../generic/mjz_lib/graph/phis.hpp"
#include "../../generic/mjz_lib/graph/rmas.hpp"
#include "../../generic/mjz_lib/graph/ssa.hpp"
#include "../../generic/mjz_lib/opt/alllib.hpp"

using namespace mjz;
namespace gns = mjz::graph_ns;
inline constexpr version_t V{};
using ulen = uintlen_t;

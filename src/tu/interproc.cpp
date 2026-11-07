// TU 5/7: interprocedural — callgraph/Tarjan (mjz_lib), scctco, tailrec
#include "minijz/opt/interprocedural/callgraph.hpp"
#include "minijz/opt/interprocedural/scctco.hpp"
#include "minijz/opt/interprocedural/tailrec.hpp"
#include "minijz/opt/interprocedural/cdfold.hpp"
#include "minijz/opt/driver.hpp"
#include "minijz/ir/destruct.hpp"
namespace minijz_tu_marker {
// link anchors: inline (constexpr) functions are only EMITTED in a TU that
// uses them; these headers' entry points are called from other TUs, so
// reference them here to force emission of the definitions.
[[maybe_unused]] auto _a1 = &opt_scc_tailcall;
[[maybe_unused]] auto _a2 = &opt_tailrec;
[[maybe_unused]] auto _a3 = &opt_cdfold;
[[maybe_unused]] auto _a4 = &build_callgraph;
}

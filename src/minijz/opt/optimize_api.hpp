#pragma once
// === optimize_api.hpp ========================================
// the pass-manager entry, DECLARED light: TUs that want to re-run the
// optimizer live (the debugger's `opt` command) need neither mjz_lib nor
// the pass bodies — the definition lives with the pipeline TU (driver.hpp,
// included via prog/compile_impl.hpp).

#include "minijz/ir/instr.hpp"
#include "minijz/base/pass_cfg.hpp"

struct An; // opt/analysis/an.hpp

constexpr bool optimize(Fn &fn, OptStats &os, An &final_an, PassCfg cfg);
// runtime-only convenience: same as optimize() but the analysis output
// stays inside the pipeline TU (light callers cannot name an An)
bool optimize_light(Fn &fn, OptStats &os, PassCfg cfg);

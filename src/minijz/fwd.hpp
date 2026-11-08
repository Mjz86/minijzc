#pragma once
// === fwd.hpp ========================================
// forward declarations of the cross-boundary types. Every header still
// includes exactly what it USES (self-containment rule); this file exists
// for DECLARATION-ONLY contexts — signatures that mention a type by
// reference or return it without touching members — so light TUs can call
// into heavy pipeline code without parsing mjz_lib or the optimizer.

struct Fn;          // ir/instr.hpp     — the IR function container
struct Instr;       // ir/instr.hpp     — one IR instruction
struct An;          // opt/analysis/an.hpp — dominators/loops (mjz_lib)
struct Optimized;   // opt/driver.hpp
struct Parsed;      // front/ast.hpp    — parser output
struct Ast;         // front/ast.hpp
struct FnUnit;      // prog/program.hpp — one fn's pipeline journey
struct Program;     // prog/program.hpp — the whole compile
struct EmitPlan;    // back/emit_plan.hpp
struct RaPlan;      // back/emit_x64.hpp
struct GenCtx;      // apps/fuzz_gen.hpp
struct RunOut;      // config.hpp       — the runner contract
struct PassCfg;     // base/pass_cfg.hpp
struct OptStats;    // base/pass_cfg.hpp
struct SsaStats;    // ir/stats.hpp
struct DestrStats;  // ir/stats.hpp
struct Cli;         // apps/cli_parse.hpp

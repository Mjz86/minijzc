#pragma once
// === runners.hpp ========================================
// umbrella: the runner contract + printers + the execution backends.
// Every piece is individually includable; this header keeps the historical
// surface (everything the app layer runs programs with).

#include "minijz/ir/instr.hpp"
#include "minijz/back/vm.hpp"
#include "minijz/back/emit_plan.hpp"
#include "minijz/back/emit_c.hpp"
#include "minijz/back/emit_x64.hpp"
#include "minijz/back/bytecode.hpp"
#include "minijz/prog/printers.hpp"
#include "minijz/prog/program.hpp"

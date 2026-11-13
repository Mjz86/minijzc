#pragma once
// === runners.hpp ========================================
// umbrella: the runner contract + printers + the execution backends.
// Every piece is individually includable; this header keeps the historical
// surface (everything the app layer runs programs with).

#include "../back/bytecode.hpp"
#include "../back/emit_c.hpp"
#include "../back/emit_plan.hpp"
#include "../back/emit_x64.hpp"
#include "../back/vm.hpp"
#include "../ir/instr.hpp"
#include "printers.hpp"
#include "program.hpp"

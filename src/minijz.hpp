// ============================================================================
// MiniJZ compiler — the whole compiler as ordered headers.
// ----------------------------------------------------------------------------
// The project stays a SINGLE translation unit: include this one header (in
// the exact order below) and you get the original monolith back, bit for
// bit. The order is load-bearing (declarations precede uses, exactly as
// the sections were laid out in the original minijz_compiler.cpp).
// ============================================================================
#pragma once

#include "minijz_common.hpp"
#include "minijz_front.hpp"
#include "minijz_ir.hpp"
#include "minijz_ssa.hpp"
#include "minijz_opt_scalar.hpp"
#include "minijz_opt_tailrec.hpp"
#include "minijz_opt_manager.hpp"
#include "minijz_destroy.hpp"
#include "minijz_bc.hpp"
#include "minijz_interp.hpp"
#include "minijz_slots.hpp"
#include "minijz_pipeline.hpp"
#include "minijz_print.hpp"
#include "minijz_backend.hpp"
#include "minijz_demos.hpp"
#include "minijz_fuzz.hpp"
#include "minijz_constexpr.hpp"

#pragma once
// === cli.hpp ========================================
// umbrella: the CLI front (light) + the heavy run/report layer. Prefer the
// specific headers; this one keeps the historical include surface.

#include "minijz/apps/cli_parse.hpp"
#include "minijz/apps/cli_run.hpp"
#include "minijz/apps/fuzz_run.hpp"
#include "minijz/apps/demo.hpp"
#include "minijz/apps/shrink.hpp"
#include "minijz/tests/cx_tours.hpp"

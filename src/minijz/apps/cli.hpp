#pragma once
// === cli.hpp ========================================
// umbrella: the CLI front (light) + the heavy run/report layer. Prefer the
// specific headers; this one keeps the historical include surface.

#include "cli_parse.hpp"
#include "cli_run.hpp"
#include "fuzz_run.hpp"
#include "demo.hpp"
#include "shrink.hpp"
#include "../tests/cx_tours.hpp"

#pragma once
// === app_api.hpp ========================================
// the app-layer boundary: the light option struct plus EXTERNAL decls for
// every heavy app entry (compiled once in their own TUs). tu/main.cpp
// includes ONLY this — parsing and dispatch never pay for the pipeline.

#include "cli_parse.hpp"
#include "../fwd.hpp"
#include "../prog/program.hpp" // Program/RunOut by value in signatures

#include <string>

// the shared main body (defined via app_main.hpp in ONE TU per binary)
int minijz_app_main(Cli C); // by value: the body tunes out_dir etc.
// curated demo suite (tu/demo.cpp)
void run_demos();
// --shrink delta-debugger (tu/shrink.cpp)
int run_shrink(const Cli &C, const std::string &path);
// the constexpr-tour battery entry, runtime flavor (tu/cxtours.cpp)
bool cx_pipeline_rt();
// the differential fuzzer (tu/fuzz.cpp)
void run_fuzz(long n, const std::string &cc, long fuzz_native,
              const std::string &out_dir, long from, bool random_configs,
              bool mjz_fuzz_dumps, long jobs);
long default_fuzz_jobs(bool selftest);
// the interactive debugger (tu/debugger.cpp)
int run_debugger(const Cli &C, const std::string &src);
// per-file compile report (defined with the CLI run layer)
RunOut compile_report(Program &P, const Cli &C, const std::string &src,
                      bool brief);

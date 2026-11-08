// tu/main.cpp (multi-cpp mode) — the THIN entry: option parsing only
// (cli_parse.hpp is light), then dispatch into minijz_app_main(), which
// lives in tu/cli_app.cpp. This TU also defines the process-wide CHECK
// counters (config.hpp declares them extern under MJZ_MULTI_CPP), so
// every TU's checks are counted by the final report.
#include "minijz/apps/app_api.hpp"

int g_checks = 0, g_fails = 0;

int main(int argc, char **argv) {
  Cli C{};
  if (!parse_cli(argc, argv, C)) return 2;
  if (C.help) {
    usage();
    return 0;
  }
  return minijz_app_main(C);
}

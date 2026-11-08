// main.cpp — single-TU entry point.
//
// The compiler is a header-only constexpr library under src/minijz/:
// every header is self-contained (includes exactly what it uses — no
// include-order dependence), so this TU includes the umbrella and gets
// the whole pipeline; the multi-cpp build (CMake -DMINIJZ_MULTI_CPP=ON)
// instead compiles src/tu/*.cpp and links them (tu/main.cpp is a thin
// shell over the very same minijz_app_main).
#include "src/minijz.hpp"
#include "minijz/apps/app_main.hpp"

int main(int argc, char **argv) {
  Cli C{};
  if (!parse_cli(argc, argv, C)) return 2;
  if (C.help) {
    usage();
    return 0;
  }
  return minijz_app_main(C);
}

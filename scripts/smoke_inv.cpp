// smoke: the 16-member inverse family -- compile + behavior check
#define MJZ_WITH_iostream false
#define MJZ_TEST_MODE_ 1
#define MJZ_LOG_NEW_ALLOCATIONS_ 0
#define MJZ_aggregate_tuple_workaround false
#include "../mjz_lib/mjz_lib/alllib_import.hpp"
#include <cstdio>
using namespace mjz;
namespace gns = mjz::graph_ns;
inline constexpr version_t V{};

int main() {
  const std::vector<std::vector<uintlen_t>> rows = {
      {2}, {0, 2}, {1}, {2}, {0, 0}};
  auto show = [](const char *n, const gns::treversal_result_t<V> &f) {
    std::printf("%-58s", n);
    for (auto r : f.range()) {
      std::printf("[");
      for (auto e : r)
        std::printf("%zu,", size_t(e));
      std::printf("]");
    }
    std::printf("\n");
  };
  show("square backward (historical)", gns::make_basic_inv_forest<V>(rows));
  show("square forward", gns::make_basic_inv_forest_forward<V>(rows));
  show("universe forward (historical)",
       gns::make_basic_inv_forest_universe<V>(rows, 3));
  show("universe backward",
       gns::make_basic_inv_forest_universe_backward<V>(rows, 3));
  // payloads
  std::vector<std::vector<uintlen_t>> data = {{10}, {21, 22}, {31}, {43}, {51, 52}};
  auto [fb, bij] = gns::map_make_basic_inv_forest_edges_bijection<V>(rows);
  std::printf("square bij (backward):");
  for (auto b : bij) std::printf(" %zu", size_t(b));
  auto [fbf, bijf] =
      gns::map_make_basic_inv_forest_edges_bijection_forward<V>(rows);
  std::printf(" | forward:");
  for (auto b : bijf) std::printf(" %zu", size_t(b));
  std::printf("\n");
  auto [fd, dd] = gns::map_make_basic_inv_forest_edges_data<V, uintlen_t>(rows, data);
  std::printf("square data (backward):");
  for (auto d : dd) std::printf(" %zu", size_t(d));
  auto [fdf, ddf] =
      gns::map_make_basic_inv_forest_edges_data_forward<V, uintlen_t>(rows, data);
  std::printf(" | forward:");
  for (auto d : ddf) std::printf(" %zu", size_t(d));
  std::printf("\n");
  auto [fdb, restb] =
      gns::map_make_basic_inv_forest_edges_data_bijection<V, uintlen_t>(rows, data);
  auto &[ddb, bijb] = restb;
  std::printf("square both (backward): data");
  for (auto d : ddb) std::printf(" %zu", size_t(d));
  std::printf(" bij");
  for (auto b : bijb) std::printf(" %zu", size_t(b));
  std::printf("\n");
  auto [fuf, restuf] =
      gns::map_make_basic_inv_forest_edges_data_bijection_universe<V, uintlen_t>(
          rows, data, 3);
  auto &[duf, biuf] = restuf;
  std::printf("universe both (forward): data");
  for (auto d : duf) std::printf(" %zu", size_t(d));
  std::printf(" bij");
  for (auto b : biuf) std::printf(" %zu", size_t(b));
  std::printf("\n");
  auto [fub, restub] =
      gns::map_make_basic_inv_forest_edges_data_bijection_universe_backward<
          V, uintlen_t>(rows, data, 3);
  auto &[dub, biub] = restub;
  std::printf("universe both (backward): data");
  for (auto d : dub) std::printf(" %zu", size_t(d));
  std::printf(" bij");
  for (auto b : biub) std::printf(" %zu", size_t(b));
  std::printf("\n");
  auto [fu_d, du] =
      gns::map_make_basic_inv_forest_edges_data_universe<V, uintlen_t>(rows,
                                                                       data, 3);
  auto [fu_db, dub2] =
      gns::map_make_basic_inv_forest_edges_data_universe_backward<V, uintlen_t>(
          rows, data, 3);
  auto [fu_b, bub] =
      gns::map_make_basic_inv_forest_edges_bijection_universe<V>(rows, 3);
  auto [fu_bb, bubb] =
      gns::map_make_basic_inv_forest_edges_bijection_universe_backward<V>(rows,
                                                                          3);
  std::printf("universe singles ok: %zu %zu %zu %zu\n", du.size(), dub2.size(),
              bub.size(), bubb.size());
  return 0;
}

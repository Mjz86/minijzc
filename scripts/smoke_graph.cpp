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
  // 4-node flow network: 0->1 (3), 0->2 (2), 1->2 (1), 1->3 (2), 2->3 (3)
  std::vector<std::vector<uintlen_t>> succs = {{1, 2}, {2, 3}, {3}, {}};
  auto g = gns::make_basic_forest<V>(succs);
  std::vector<uint32_t> caps = {3, 2, 1, 2, 3};
  auto fl = gns::calculate_maximum_flow<V>(0, 3, g, caps);
  std::printf("maxflow=%u\n", unsigned(fl.flow));
  auto cut = gns::calculate_st_min_cut<V>(0, 3, g, caps);
  std::printf("stcut=%u edges=%zu\n", unsigned(cut.weight),
              cut.cut_edges.size());
  auto mc = gns::calculate_min_cost_flow<V>(
      0, 3, g, caps, std::vector<int64_t>{1, 5, 1, 3, 2});
  std::printf("mcf flow=%u cost=%lld\n", unsigned(mc.flow), (long long)mc.cost);
  // undirected global min cut (the classic 8-node Stoer-Wagner example scaled
  // down) rows: 0:[1,2] 1:[0,2] 2:[0,1,3] 3:[2,4] 4:[3,5] 5:[4] -- the
  // undirected edges (01,02,12,23,34,45) with weights (2,3,3,4,3,3),
  // each on BOTH arcs (arc order: row-major)
  std::vector<std::vector<uintlen_t>> usuccs = {{1, 2}, {0, 2}, {0, 1, 3},
                                                {2, 4}, {3, 5}, {4}};
  auto ug = gns::make_basic_forest<V>(usuccs);
  std::vector<uint32_t> w = {2, 3,    // 0->1, 0->2
                             2, 3,    // 1->0, 1->2
                             3, 3, 4, // 2->0, 2->1, 2->3
                             4, 3,    // 3->2, 3->4
                             3, 3,    // 4->3, 4->5
                             3};      // 5->4
  auto gc = gns::calculate_global_min_cut_undirected<V>(ug, w);
  std::printf("global cut=%u side=%d%d%d%d%d%d\n", unsigned(gc.weight),
              gc.side[0], gc.side[1], gc.side[2], gc.side[3], gc.side[4],
              gc.side[5]);
  auto mx = gns::calculate_max_cut_bruteforce<V>(ug, w);
  std::printf("maxcut=%u\n", unsigned(mx.weight));
  auto mxg = gns::calculate_max_cut_greedy<V>(ug, w);
  std::printf("maxcut greedy=%u\n", unsigned(mxg.weight));
  auto gom = gns::calculate_gomory_hu_tree<V>(ug, w);
  std::printf("gomory-hu q(0,5)=%u\n",
              unsigned(gns::gomory_hu_query<V>(gom, 0, 5)));
  auto dgc = gns::calculate_global_min_cut_directed<V>(g, caps);
  std::printf("directed global=%u\n", unsigned(dgc.weight));
  // MST
  auto k = gns::calculate_minimum_spanning_forest_kruskal<V>(ug, w);
  auto p = gns::calculate_minimum_spanning_forest_prim<V>(ug, w);
  auto b = gns::calculate_minimum_spanning_forest_boruvka<V>(ug, w);
  std::printf("mst k=%u/%zu p=%u/%zu b=%u/%zu\n", unsigned(k.total),
              k.edges.size(), unsigned(p.total), p.edges.size(),
              unsigned(b.total), b.edges.size());
  // bipartite matching
  std::vector<std::vector<uintlen_t>> lrows = {{0, 1}, {0}, {1}};
  auto m1 = gns::calculate_maximum_bipartite_matching<V>(lrows, 2);
  auto m2 = gns::calculate_maximum_bipartite_matching_kuhn<V>(lrows, 2);
  std::printf("bipartite hk=%u kuhn=%u\n", unsigned(m1.size),
              unsigned(m2.size));
  auto cov = gns::calculate_bipartite_minimum_vertex_cover<V>(lrows, 2, m1);
  int cs = 0;
  for (char c : cov)
    cs += c;
  std::printf("konig cover=%d\n", cs);
  // blossom: the triangle
  std::vector<std::vector<uintlen_t>> tri = {{1, 2}, {0, 2}, {0, 1}};
  auto tg = gns::make_basic_forest<V>(tri);
  auto bm = gns::calculate_maximum_matching<V>(tg);
  std::printf("blossom triangle size=%u\n", unsigned(bm.size));
  // biconnect: the butterfly
  std::vector<std::vector<uintlen_t>> bf = {
      {1, 2}, {0, 3}, {0, 3}, {1, 2, 4}, {3}};
  auto bg = gns::make_basic_forest<V>(bf);
  auto bi = gns::calculate_biconnectivity<V>(bg);
  auto br = gns::calculate_bridges<V>(bg);
  auto ap = gns::calculate_articulation_points<V>(bg);
  int apn = 0;
  for (char c : ap)
    apn += c;
  std::printf("butterfly bcc=%u bridges=%zu artics=%d\n",
              unsigned(bi.bcc_count), br.size(), apn);
  auto kc = gns::calculate_k_cores<V>(bg);
  std::printf("kcores=%u%u%u%u%u\n", unsigned(kc[0]), unsigned(kc[1]),
              unsigned(kc[2]), unsigned(kc[3]), unsigned(kc[4]));
  // euler: directed circuit on a square
  std::vector<std::vector<uintlen_t>> sq = {{1}, {2}, {3}, {0}};
  auto sg = gns::make_basic_forest<V>(sq);
  auto ec = gns::calculate_eulerian_circuit_directed<V>(sg);
  std::printf("euler dir circuit ok=%d edges=%zu\n", int(ec.ok),
              ec.edges.size());
  auto ep = gns::calculate_eulerian_path_directed<V>(sg);
  std::printf("euler dir path ok=%d edges=%zu\n", int(ep.ok), ep.edges.size());
  std::vector<std::vector<uintlen_t>> pen = {{1, 3}, {0, 2}, {1, 3}, {0, 2}};
  auto pg = gns::make_basic_forest<V>(pen);
  auto uc = gns::calculate_eulerian_circuit_undirected<V>(pg);
  std::printf("euler undir circuit ok=%d edges=%zu\n", int(uc.ok),
              uc.edges.size());
  // the minimum arc set family
  std::vector<std::vector<uintlen_t>> dag = {{1, 2}, {3}, {3}, {}};
  auto dg = gns::make_basic_forest<V>(dag);
  auto tr = gns::calculate_transitive_reduction_dag<V>(dg);
  std::printf("tr keeps=%zu (the 0->3 arc drops)\n", tr.keep_edges.size());
  auto cn = gns::calculate_cycle_nodes<V>(sg);
  std::printf("cycle nodes on square=%d%d%d%d\n", cn[0], cn[1], cn[2], cn[3]);
  std::vector<std::vector<uintlen_t>> cyc = {{1}, {2}, {0}, {}};
  auto cg = gns::make_basic_forest<V>(cyc);
  auto rs = gns::calculate_minimum_equivalent_arc_set<V>(cg);
  std::printf("meg keeps=%zu (exact triangle + the tail)\n",
              rs.keep_edges.size());
  // the selection family: closure + path cover
  auto cl = gns::calculate_maximum_weight_closure<V>(
      cg, std::vector<int64_t>{3, -2, 4, -1});
  std::printf("closure w=%lld in=%d%d%d%d (the whole cycle)\n",
              (long long)cl.weight, cl.in_closure[0], cl.in_closure[1],
              cl.in_closure[2], cl.in_closure[3]);
  auto pcv = gns::calculate_minimum_path_cover_dag<V>(dg);
  std::printf("path cover paths=%u (0->1->3 and 2)\n",
              unsigned(pcv.path_count));
  // fas: one loop -- cutting the body arc (6) loses to dropping the
  // back arc (5); the survivors' topological order rides home
  std::vector<std::vector<uintlen_t>> loop = {{1}, {0}};
  auto lg = gns::make_basic_forest<V>(loop);
  auto r1 = gns::calculate_minimum_feedback_arc_set<V>(
      lg, std::vector<uintlen_t>{6, 5});
  std::printf("fas loop w=%u arcs=%zu order=%u%u (the back arc goes)\n",
              unsigned(r1.total_weight), r1.removed_arcs.size(),
              unsigned(r1.order[0]), unsigned(r1.order[1]));
  // fas: nested loops -- the shared body arc (e1, w6) serves both
  // loops; the inner back arc (5) plus an outer cut, or the outer
  // back arc (1000), all lose to it
  std::vector<std::vector<uintlen_t>> nest = {{1}, {2}, {1, 3}, {0}};
  auto ng = gns::make_basic_forest<V>(nest);
  auto r2 = gns::calculate_minimum_feedback_arc_set<V>(
      ng, std::vector<uintlen_t>{2, 6, 5, 100, 1000});
  std::printf("fas nested w=%u arcs=%zu (the shared body arc)\n",
              unsigned(r2.total_weight), r2.removed_arcs.size());
  // fas: the irreducible diamond -- no reducibility promise asked, no
  // root asked; the exact sweep answers any digraph
  std::vector<std::vector<uintlen_t>> dia = {{1, 2}, {2}, {1}};
  auto dg2 = gns::make_basic_forest<V>(dia);
  auto r3 = gns::calculate_minimum_feedback_arc_set<V>(
      dg2, std::vector<uintlen_t>{1, 1, 1, 1});
  std::printf("fas diamond w=%u arcs=%zu (irreducible, still exact)\n",
              unsigned(r3.total_weight), r3.removed_arcs.size());
  // distances
  auto bf2 = gns::calculate_shortest_path_maybe_negative<V>(
      0, dg, std::vector<int64_t>{5, 3, -1, 2});
  std::printf("bellman d3=%lld\n", (long long)bf2.dist[3]);
  auto ap2 = gns::calculate_all_pairs_shortest_paths<V>(
      dg, std::vector<int64_t>{5, 3, -1, 2});
  std::printf("floyd d(0,3)=%lld\n", (long long)ap2.dist[0 * 4 + 3]);
  auto dsp = gns::calculate_dag_shortest_paths<V>(
      0, dg, std::vector<int64_t>{5, 3, -1, 2});
  std::printf("dagsp d3=%lld\n", (long long)dsp[3]);
  auto ecc = gns::calculate_eccentricities_unweighted<V>(dg);
  std::printf("diam=%u radius=%u\n", unsigned(ecc.diameter),
              unsigned(ecc.radius));
  return 0;
}

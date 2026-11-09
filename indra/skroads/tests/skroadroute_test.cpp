/**
 * @file skroadroute_test.cpp
 * @brief The route planner on small graphs, and on the dataset's sample release where it is on disk.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */

#include "skroadroute.h"
#include "skroadroutedata.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>

using namespace skroute;
using Catch::Matchers::WithinAbs;

namespace
{
    int32_t join(Graph& g, int32_t a, int32_t b, uint8_t flags = 0)
    {
        return g.addEdge(a, b, { g.node(a).p, g.node(b).p }, flags);
    }

    // an n x n grid of 100 m blocks
    Graph grid(int n)
    {
        Graph g;
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                g.addNode({ x * 100.0, y * 100.0, 0 });
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
            {
                if (x + 1 < n) join(g, y * n + x, y * n + x + 1);
                if (y + 1 < n) join(g, y * n + x, (y + 1) * n + x);
            }
        g.finalize();
        return g;
    }
}

TEST_CASE("route on a grid is the Manhattan distance", "[skroadroute]")
{
    Graph g = grid(5);
    Route r = plan(g, shortestCost(), 0, 0, 400, 300);
    REQUIRE(r.ok);
    CHECK_THAT(r.length, WithinAbs(700.0, 1e-6));
    CHECK_THAT(r.cost, WithinAbs(700.0, 1e-6));
    CHECK(r.line.front().x == 0);
    CHECK(r.line.back().x == 400);
    // Dijkstra finds the same cost, taking more states
    Route d = plan(g, shortestCost(), 0, 0, 400, 300, dijkstra());
    REQUIRE(d.ok);
    CHECK_THAT(d.cost, WithinAbs(r.cost, 1e-6));
    CHECK(d.expanded >= r.expanded);
}

TEST_CASE("route starts and ends mid-way, projected and split", "[skroadroute]")
{
    Graph g = grid(3);
    // from beside the bottom road at x 30, to beside the top road at x 170
    Route r = plan(g, shortestCost(), 30, -4, 170, 204);
    REQUIRE(r.ok);
    CHECK_THAT(r.start_off, WithinAbs(4.0, 1e-6));
    CHECK_THAT(r.target_off, WithinAbs(4.0, 1e-6));
    // 30 back or 70 on to a crossroad, 200 up, then across: the shortest is 30 + 200 + 170 or 70 + 200 + 70
    CHECK_THAT(r.cost, WithinAbs(340.0, 1e-6));
    // the same edge both ends: straight along it
    Route s = plan(g, shortestCost(), 20, 0, 80, 0);
    REQUIRE(s.ok);
    CHECK_THAT(s.cost, WithinAbs(60.0, 1e-6));
    CHECK(s.nodes.empty());
}

TEST_CASE("route at a T and an X gives each junction's outgoing bearing", "[skroadroute]")
{
    // a T: west end 0, the junction 1, east end 2, south end 3
    Graph t;
    t.addNode({ -100, 0, 0 });
    t.addNode({ 0, 0, 0 });
    t.addNode({ 100, 0, 0 });
    t.addNode({ 0, -100, 0 });
    join(t, 0, 1);
    join(t, 1, 2);
    join(t, 1, 3);
    t.finalize();
    Route r = plan(t, shortestCost(), -90, 0, 0, -90);
    REQUIRE(r.ok);
    CHECK_THAT(r.length, WithinAbs(180.0, 1e-6));
    REQUIRE(r.legs.size() == 2);
    CHECK_THAT(r.legs[0].bearing, WithinAbs(90.0, 1e-6));     // east
    CHECK(r.legs[1].node == 1);
    CHECK_THAT(r.legs[1].bearing, WithinAbs(180.0, 1e-6));    // south
    CHECK_THAT(r.legs[1].along, WithinAbs(90.0, 1e-6));

    // an X: north from the west
    Graph x;
    x.addNode({ 0, 0, 0 });
    for (auto p : { Point{ 100, 0, 0 }, Point{ -100, 0, 0 }, Point{ 0, 100, 0 }, Point{ 0, -100, 0 } })
        join(x, 0, x.addNode(p));
    x.finalize();
    Route q = plan(x, shortestCost(), -100, 0, 0, 100);
    REQUIRE(q.ok);
    REQUIRE(q.legs.size() == 2);
    CHECK_THAT(q.legs[1].bearing, WithinAbs(0.0, 1e-6));
    CHECK(legNear(q, 3, 2, 20) == &q.legs[1]);
    CHECK(bearingDiff(350, 10) == 20);
}

TEST_CASE("route costs are pluggable: penalties, one-way, turns", "[skroadroute]")
{
    // two ways from 0 to 3: a direct candidate (200 m) and a confirmed detour (250 m)
    Graph g;
    g.addNode({ 0, 0, 0 });
    g.addNode({ 200, 0, 0 });
    g.addNode({ 100, 75, 0 });
    g.addNode({ 300, 0, 0 });
    join(g, 0, 1, EDGE_CANDIDATE);
    join(g, 0, 2);
    join(g, 2, 1);
    join(g, 1, 3);
    g.finalize();
    Route r = plan(g, shortestCost(), 0, 0, 300, 0);
    REQUIRE(r.ok);
    CHECK_THAT(r.length, WithinAbs(350.0, 1e-6));    // 125 + 125 + 100 beats 200 * 1.3 + 100
    // a cost of plain length takes the candidate
    CostModel plain;
    plain.edge = [](const Graph& gr, int32_t e, bool, const Request&) { return gr.edge(e).length; };
    CHECK_THAT(plan(g, plain, 0, 0, 300, 0).length, WithinAbs(300.0, 1e-6));
    // a one-way against the trip is not taken
    Graph o;
    o.addNode({ 0, 0, 0 });
    o.addNode({ 100, 0, 0 });
    join(o, 1, 0, EDGE_ONEWAY);
    o.finalize();
    CHECK_FALSE(plan(o, shortestCost(), 0, 0, 100, 0).ok);
    CHECK(plan(o, shortestCost(), 100, 0, 0, 0).ok);
    // fastest: seconds, with a junction's cost at each turn
    Graph gg = grid(3);
    Route f = plan(gg, fastestCost(), 0, 0, 200, 200);
    REQUIRE(f.ok);
    CHECK_THAT(f.cost, WithinAbs(400.0 / 16.0 + 2 * 1.5, 1e-6));     // round the edge: two junctions passed straight, the corner a bend
    // a turn penalty large enough makes the straightest of equal routes win: one turn, not three
    CostModel turny = shortestCost();
    turny.turn = [](const Graph&, int32_t, int32_t in, int32_t, double deg, const Request&) { return in >= 0 && std::fabs(deg) > 30 ? 50.0 : 0.0; };
    Route tr = plan(gg, turny, 0, 0, 200, 200);
    REQUIRE(tr.ok);
    CHECK_THAT(tr.cost, WithinAbs(450.0, 1e-6));
}

TEST_CASE("a disconnected target has no route", "[skroadroute]")
{
    Graph g;
    g.addNode({ 0, 0, 0 });
    g.addNode({ 100, 0, 0 });
    g.addNode({ 0, 500, 0 });
    g.addNode({ 100, 500, 0 });
    join(g, 0, 1);
    join(g, 2, 3);
    g.finalize();
    CHECK_FALSE(plan(g, shortestCost(), 0, 0, 100, 500).ok);
}

TEST_CASE("route on the sample release", "[skroadroute]")
{
    const char* env = std::getenv("SK_ROAD_DATASET");
    const std::string dir = env ? env : "V:/Scratch/worldmap/dataset/release/0.1.0-sample";
    if (!std::filesystem::exists(dir + "/nodes.geojson"))
    {
        SKIP("no sample release at " << dir);
    }
    Graph g;
    LoadStats st;
    REQUIRE(loadDataset(dir, g, st));
    g.finalize();
    CHECK(st.nodes == 12227);
    CHECK(st.candidates == 16783);
    // two nodes about 8 km apart in the largest component
    const double ax = 286546.42, ay = 265823.33, bx = 282345.6, by = 258731.2;
    Route r = plan(g, shortestCost(), ax, ay, bx, by);
    REQUIRE(r.ok);
    CHECK(r.start_off < 0.5);
    CHECK(r.target_off < 0.5);
    CHECK(r.length >= std::hypot(ax - bx, ay - by));
    CHECK(r.legs.size() > 2);
    Route d = plan(g, shortestCost(), ax, ay, bx, by, dijkstra());
    REQUIRE(d.ok);
    CHECK_THAT(d.cost, WithinAbs(r.cost, 1e-3));
    Route f = plan(g, fastestCost(), ax, ay, bx, by);
    REQUIRE(f.ok);
    UNSCOPED_INFO("shortest " << r.length << " m, " << r.legs.size() << " legs, " << r.expanded << " states (Dijkstra "
                  << d.expanded << "); fastest " << f.length << " m, " << f.cost << " s");
    CHECK(true);
}

/**
 * @file skroadroute.h
 * @brief Spike: route planning over a road graph. The graph knows nothing of costs; a cost model (how much an
 * edge and a turn at a node cost) and a heuristic are handed to the search, so the same graph serves a car's
 * shortest or fastest route, and later walking, flying or the map's routes.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace skroute
{
    struct Point
    {
        double x = 0, y = 0, z = 0;
    };

    enum EdgeFlags : uint8_t
    {
        EDGE_CANDIDATE = 1 << 0,    // derived, not confirmed (the dataset's candidates layer)
        EDGE_PRIVATE = 1 << 1,      // a private road
        EDGE_ONEWAY = 1 << 2,       // forward (a to b) only
        EDGE_CONFIRMED = 1 << 3,    // driven: the live model confirmed it
    };

    struct Node
    {
        Point p;
        std::string id;
    };

    struct Edge
    {
        int32_t a = -1, b = -1;
        double length = 0;          // along its line, 3D
        double speed = 0;           // m/s measured on it, 0 unknown
        uint8_t flags = 0;
        uint8_t kind = 0;           // 0 road, 1 jump (unused yet)
        uint32_t first = 0, count = 0;  // its line in Graph::mShape, a to b, the end nodes included
    };

    // Junctions and ends (and any shape points the source keeps as nodes) joined by edges carrying a line.
    class Graph
    {
    public:
        int32_t addNode(const Point& p, const std::string& id = {});
        int32_t addEdge(int32_t a, int32_t b, const std::vector<Point>& line, uint8_t flags = 0, double speed = 0);
        // An edge from a line whose ends are snapped to a node within snap_m, else made a node (the live model's
        // confirmed roads); a line on an edge already there only marks it confirmed.
        int32_t addLine(const std::vector<Point>& line, uint8_t flags, double snap_m = 3.0);
        void finalize();            // builds the adjacency; call after adding, before searching

        size_t nodeCount() const { return mNodes.size(); }
        size_t edgeCount() const { return mEdges.size(); }
        const Node& node(int32_t i) const { return mNodes[i]; }
        const Edge& edge(int32_t i) const { return mEdges[i]; }
        Edge& edge(int32_t i) { return mEdges[i]; }
        int32_t degree(int32_t n) const { return mAdjStart[n + 1] - mAdjStart[n]; }
        // the edges at a node (CSR, finalize's)
        const int32_t* adjBegin(int32_t n) const { return mAdj.data() + mAdjStart[n]; }
        const int32_t* adjEnd(int32_t n) const { return mAdj.data() + mAdjStart[n + 1]; }
        const Point& shape(const Edge& e, uint32_t i) const { return mShape[e.first + i]; }
        int32_t nearestNode(double x, double y, double within_m) const;

        // The nearest point on any edge to (x, y): its edge, the share along it from a, where, how far.
        struct Projection
        {
            int32_t edge = -1;
            double t = 0;           // 0 at a, 1 at b, by length
            Point at;
            double dist = 1e18;
        };
        Projection project(double x, double y) const;
        // The compass bearing (degrees, 0 north, clockwise) leaving node `from` along edge e, over its first
        // look_m metres.
        double bearingFrom(int32_t e, int32_t from, double look_m = 15.0) const;

    private:
        std::vector<Node> mNodes;
        std::vector<Edge> mEdges;
        std::vector<Point> mShape;
        std::vector<int32_t> mAdjStart, mAdj;
    };

    // What the search knows of a request, for the costs that depend on it (a private road free where the target is).
    struct Request
    {
        double from_x = 0, from_y = 0, to_x = 0, to_y = 0;
        int32_t from_edge = -1, to_edge = -1;
    };

    // The costs a route is weighed by, in any unit (metres, seconds, ...). edge: the cost of the whole edge
    // travelled forward (a to b) or not; a negative cost forbids it. turn: the cost of going on from in_edge to
    // out_edge at node (in_edge -1 at the start), turn_deg signed (+ left); may be empty. min_cost_per_m: the
    // least an edge can cost a metre, which keeps the default heuristic admissible.
    struct CostModel
    {
        std::function<double(const Graph&, int32_t edge, bool forward, const Request&)> edge;
        std::function<double(const Graph&, int32_t node, int32_t in_edge, int32_t out_edge, double turn_deg, const Request&)> turn;
        double min_cost_per_m = 1.0;
        std::string name;
    };

    // The estimate of the cost left from a point to the target: empty, the straight line times min_cost_per_m.
    using Heuristic = std::function<double(const Point& at, const Point& target)>;
    Heuristic dijkstra();          // 0 everywhere

    // "shortest": length, a candidate x1.3, a private road x1.5 unless the target is on it.
    CostModel shortestCost();
    // "fastest": seconds, length over a measured speed or one per kind of way, plus a junction's and a turn's.
    CostModel fastestCost();
    CostModel costByName(const std::string& name);  // "fastest", else shortest

    struct Leg
    {
        int32_t node = -1;          // where it leaves (-1: the start, on an edge)
        Point at;
        int32_t edge = -1;          // the edge it leaves on
        double bearing = 0;         // compass degrees leaving along it
        double along = 0;           // metres from the start
        bool junction = false;      // the node has three or more ways
    };

    struct Route
    {
        bool ok = false;
        std::vector<int32_t> nodes; // the chain of graph nodes passed
        std::vector<int32_t> edges; // the edges travelled, the start's and the target's partial ones included
        std::vector<Leg> legs;      // the start and every junction passed, with the way it leaves on
        std::vector<Point> line;    // the route as driven, start to target
        double length = 0;          // metres
        double cost = 0;            // in the cost model's unit
        double start_off = 0, target_off = 0;  // how far start and target lie from the graph
        int32_t expanded = 0;       // states the search took
    };

    Route plan(const Graph& g, const CostModel& cost, double from_x, double from_y, double to_x, double to_y,
               const Heuristic& h = {});

    // The planned line's nearest point to (x, y): how far off it, and how far along it.
    double offRoute(const Route& r, double x, double y, double* along = nullptr);
    // The leg the route takes at the junction nearest (x, y) within r_m, ahead of along_m (null if none).
    const Leg* legNear(const Route& r, double x, double y, double r_m, double along_m = -1);
    // The smallest angle between two compass bearings, degrees.
    double bearingDiff(double a, double b);
}

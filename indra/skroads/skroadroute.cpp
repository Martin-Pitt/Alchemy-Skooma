/**
 * @file skroadroute.cpp
 * @brief Spike: A* over a road graph with a pluggable cost model and heuristic.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */

#include "skroadroute.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace skroute
{
namespace
{
    constexpr double PI = 3.14159265358979323846;

    double dist2d(const Point& a, const Point& b) { return std::hypot(b.x - a.x, b.y - a.y); }
    double dist3d(const Point& a, const Point& b) { return std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y) + (b.z - a.z) * (b.z - a.z)); }

    double compass(double dx, double dy)
    {
        double b = std::atan2(dx, dy) * 180.0 / PI;
        return b < 0 ? b + 360.0 : b;
    }

    // signed turn, + left, from compass bearing `in` to `out`
    double turnDeg(double in, double out)
    {
        double d = in - out;
        while (d > 180.0) d -= 360.0;
        while (d <= -180.0) d += 360.0;
        return d;
    }

    // the point a share t (by length) along edge e, a to b
    Point pointAt(const Graph& g, const Edge& e, double t)
    {
        const double want = std::clamp(t, 0.0, 1.0) * e.length;
        double run = 0;
        for (uint32_t i = 0; i + 1 < e.count; ++i)
        {
            const Point& p = g.shape(e, i);
            const Point& q = g.shape(e, i + 1);
            const double L = dist3d(p, q);
            if (run + L >= want && L > 0)
            {
                const double f = (want - run) / L;
                return { p.x + f * (q.x - p.x), p.y + f * (q.y - p.y), p.z + f * (q.z - p.z) };
            }
            run += L;
        }
        return g.shape(e, e.count - 1);
    }

    // the line of edge e from share t0 to t1 (backwards where t1 < t0), its first point left out
    void appendPart(const Graph& g, int32_t ei, double t0, double t1, std::vector<Point>& out)
    {
        const Edge& e = g.edge(ei);
        const double s0 = t0 * e.length, s1 = t1 * e.length;
        std::vector<std::pair<double, Point>> inner;
        double run = 0;
        for (uint32_t i = 0; i < e.count; ++i)
        {
            if (i > 0)
            {
                run += dist3d(g.shape(e, i - 1), g.shape(e, i));
            }
            if (run > std::min(s0, s1) && run < std::max(s0, s1))
            {
                inner.push_back({ run, g.shape(e, i) });
            }
        }
        if (s1 < s0)
        {
            std::reverse(inner.begin(), inner.end());
        }
        for (auto& p : inner)
        {
            out.push_back(p.second);
        }
        out.push_back(pointAt(g, e, t1));
    }

    double lineLength(const std::vector<Point>& l)
    {
        double s = 0;
        for (size_t i = 1; i < l.size(); ++i)
        {
            s += dist3d(l[i - 1], l[i]);
        }
        return s;
    }
}

int32_t Graph::addNode(const Point& p, const std::string& id)
{
    mNodes.push_back({ p, id });
    return int32_t(mNodes.size() - 1);
}

int32_t Graph::addEdge(int32_t a, int32_t b, const std::vector<Point>& line, uint8_t flags, double speed)
{
    Edge e;
    e.a = a;
    e.b = b;
    e.flags = flags;
    e.speed = speed;
    e.first = uint32_t(mShape.size());
    mShape.push_back(mNodes[a].p);
    for (size_t i = 1; i + 1 < line.size(); ++i)
    {
        mShape.push_back(line[i]);
    }
    mShape.push_back(mNodes[b].p);
    e.count = uint32_t(mShape.size() - e.first);
    for (uint32_t i = 1; i < e.count; ++i)
    {
        e.length += dist3d(mShape[e.first + i - 1], mShape[e.first + i]);
    }
    mEdges.push_back(e);
    return int32_t(mEdges.size() - 1);
}

int32_t Graph::nearestNode(double x, double y, double within_m) const
{
    int32_t best = -1;
    double bd = within_m;
    for (size_t i = 0; i < mNodes.size(); ++i)
    {
        const double d = std::hypot(mNodes[i].p.x - x, mNodes[i].p.y - y);
        if (d <= bd)
        {
            bd = d;
            best = int32_t(i);
        }
    }
    return best;
}

int32_t Graph::addLine(const std::vector<Point>& line, uint8_t flags, double snap_m)
{
    if (line.size() < 2)
    {
        return -1;
    }
    int32_t a = nearestNode(line.front().x, line.front().y, snap_m);
    int32_t b = nearestNode(line.back().x, line.back().y, snap_m);
    if (a >= 0 && b >= 0)
    {
        for (Edge& e : mEdges)
        {
            if ((e.a == a && e.b == b) || (e.a == b && e.b == a))
            {
                e.flags = uint8_t((e.flags & ~EDGE_CANDIDATE) | flags);
                return int32_t(&e - mEdges.data());
            }
        }
    }
    a = a >= 0 ? a : addNode(line.front());
    b = b >= 0 ? b : addNode(line.back());
    return a == b ? -1 : addEdge(a, b, line, flags);
}

void Graph::finalize()
{
    mAdjStart.assign(mNodes.size() + 1, 0);
    for (const Edge& e : mEdges)
    {
        ++mAdjStart[e.a + 1];
        ++mAdjStart[e.b + 1];
    }
    for (size_t i = 1; i < mAdjStart.size(); ++i)
    {
        mAdjStart[i] += mAdjStart[i - 1];
    }
    mAdj.assign(mAdjStart.back(), -1);
    std::vector<int32_t> fill(mAdjStart.begin(), mAdjStart.end() - 1);
    for (size_t i = 0; i < mEdges.size(); ++i)
    {
        mAdj[fill[mEdges[i].a]++] = int32_t(i);
        mAdj[fill[mEdges[i].b]++] = int32_t(i);
    }
}

Graph::Projection Graph::project(double x, double y) const
{
    Projection best;
    for (size_t ei = 0; ei < mEdges.size(); ++ei)
    {
        const Edge& e = mEdges[ei];
        double run = 0;
        for (uint32_t i = 0; i + 1 < e.count; ++i)
        {
            const Point& p = mShape[e.first + i];
            const Point& q = mShape[e.first + i + 1];
            const double sx = q.x - p.x, sy = q.y - p.y, L2 = sx * sx + sy * sy;
            const double f = L2 > 0 ? std::clamp(((x - p.x) * sx + (y - p.y) * sy) / L2, 0.0, 1.0) : 0.0;
            const double px = p.x + f * sx, py = p.y + f * sy;
            const double d = std::hypot(x - px, y - py);
            const double L = dist3d(p, q);
            if (d < best.dist)
            {
                best.dist = d;
                best.edge = int32_t(ei);
                best.t = e.length > 0 ? (run + f * L) / e.length : 0.0;
                best.at = { px, py, p.z + f * (q.z - p.z) };
            }
            run += L;
        }
    }
    return best;
}

double Graph::bearingFrom(int32_t ei, int32_t from, double look_m) const
{
    const Edge& e = mEdges[ei];
    const bool fwd = from == e.a;
    const Point& o = mNodes[from].p;
    Point ahead = o;
    for (uint32_t k = 1; k < e.count; ++k)
    {
        ahead = mShape[e.first + (fwd ? k : e.count - 1 - k)];
        if (dist2d(o, ahead) >= look_m)
        {
            break;
        }
    }
    return compass(ahead.x - o.x, ahead.y - o.y);
}

Heuristic dijkstra()
{
    return [](const Point&, const Point&) { return 0.0; };
}

CostModel shortestCost()
{
    CostModel c;
    c.name = "shortest";
    c.min_cost_per_m = 1.0;
    c.edge = [](const Graph& g, int32_t ei, bool forward, const Request& r) {
        const Edge& e = g.edge(ei);
        if (!forward && (e.flags & EDGE_ONEWAY))
        {
            return -1.0;
        }
        double k = 1.0;
        if ((e.flags & EDGE_CANDIDATE) && !(e.flags & EDGE_CONFIRMED))
        {
            k *= 1.3;
        }
        if ((e.flags & EDGE_PRIVATE) && ei != r.to_edge && ei != r.from_edge)
        {
            k *= 1.5;
        }
        return e.length * k;
    };
    return c;
}

CostModel fastestCost()
{
    // m/s where nothing was measured; a measured speed is held under MAX_MPS, which bounds the heuristic
    constexpr double CONFIRMED_MPS = 18.0, PUBLIC_MPS = 16.0, CANDIDATE_MPS = 12.0, PRIVATE_MPS = 9.0, MAX_MPS = 22.0;
    constexpr double JUNCTION_S = 1.5, TURN_S_PER_90 = 3.0;
    CostModel c;
    c.name = "fastest";
    c.min_cost_per_m = 1.0 / MAX_MPS;
    c.edge = [=](const Graph& g, int32_t ei, bool forward, const Request&) {
        const Edge& e = g.edge(ei);
        if (!forward && (e.flags & EDGE_ONEWAY))
        {
            return -1.0;
        }
        double v = e.flags & EDGE_CONFIRMED ? CONFIRMED_MPS : e.flags & EDGE_PRIVATE ? PRIVATE_MPS
                 : e.flags & EDGE_CANDIDATE ? CANDIDATE_MPS : PUBLIC_MPS;
        if (e.speed > 0)
        {
            v = std::min(e.speed, MAX_MPS);
        }
        return e.length / v;
    };
    c.turn = [=](const Graph& g, int32_t node, int32_t in_edge, int32_t, double turn_deg, const Request&) {
        if (in_edge < 0 || g.degree(node) < 3)
        {
            return 0.0;     // a bend in the road, not a manoeuvre
        }
        return JUNCTION_S + TURN_S_PER_90 * std::fabs(turn_deg) / 90.0;
    };
    return c;
}

CostModel costByName(const std::string& name)
{
    return name == "fastest" ? fastestCost() : shortestCost();
}

Route plan(const Graph& g, const CostModel& cost, double from_x, double from_y, double to_x, double to_y, const Heuristic& h_in)
{
    Route route;
    if (g.edgeCount() == 0 || !cost.edge)
    {
        return route;
    }
    const Graph::Projection ps = g.project(from_x, from_y);
    const Graph::Projection pt = g.project(to_x, to_y);
    route.start_off = ps.dist;
    route.target_off = pt.dist;
    Request req{ from_x, from_y, to_x, to_y, ps.edge, pt.edge };
    const Point target = pt.at;
    const Heuristic h = h_in ? h_in : Heuristic([k = cost.min_cost_per_m](const Point& a, const Point& b) { return dist2d(a, b) * k; });

    // a state is a directed edge travelled to its end: 2e (a to b, at b) or 2e+1 (b to a, at a); GOAL the target
    const int32_t E = int32_t(g.edgeCount());
    const int32_t GOAL = 2 * E;
    const double INF = std::numeric_limits<double>::infinity();
    std::vector<double> best(size_t(2 * E + 1), INF);
    std::vector<int32_t> parent(size_t(2 * E + 1), -2);    // -1: from the start
    using Item = std::pair<double, int32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    auto at = [&](int32_t s) { const Edge& e = g.edge(s / 2); return s % 2 ? e.a : e.b; };
    auto push = [&](int32_t s, double gc, int32_t from) {
        if (gc < best[s])
        {
            best[s] = gc;
            parent[s] = from;
            open.push({ gc + (s == GOAL ? 0.0 : h(g.node(at(s)).p, target)), s });
        }
    };
    auto partial = [&](int32_t ei, bool fwd, double share) {
        const double c = cost.edge(g, ei, fwd, req);
        return c < 0 ? -1.0 : c * share;
    };
    // from the start, along its edge either way
    {
        const double cf = partial(ps.edge, true, 1.0 - ps.t), cb = partial(ps.edge, false, ps.t);
        if (cf >= 0) push(2 * ps.edge, cf, -1);
        if (cb >= 0) push(2 * ps.edge + 1, cb, -1);
        if (ps.edge == pt.edge)
        {
            const bool fwd = pt.t >= ps.t;
            const double c = partial(ps.edge, fwd, std::fabs(pt.t - ps.t));
            if (c >= 0) push(GOAL, c, -1);
        }
    }
    while (!open.empty())
    {
        const auto [f, s] = open.top();
        open.pop();
        if (s == GOAL)
        {
            break;
        }
        const double gs = best[s];
        if (f - h(g.node(at(s)).p, target) > gs + 1e-9)
        {
            continue;   // stale
        }
        ++route.expanded;
        const int32_t n = at(s), in = s / 2;
        const double in_bearing = std::fmod(g.bearingFrom(in, n) + 180.0, 360.0);
        for (const int32_t* it = g.adjBegin(n); it != g.adjEnd(n); ++it)
        {
            const int32_t out = *it;
            if (out == in && g.degree(n) > 1)
            {
                continue;   // no turning back on the way just come
            }
            const bool fwd = g.edge(out).a == n;
            double tc = 0;
            if (cost.turn)
            {
                tc = cost.turn(g, n, in, out, turnDeg(in_bearing, g.bearingFrom(out, n)), req);
                if (tc < 0) continue;
            }
            if (out == pt.edge)
            {
                const double c = partial(out, fwd, fwd ? pt.t : 1.0 - pt.t);
                if (c >= 0) push(GOAL, gs + tc + c, s);
            }
            const double c = partial(out, fwd, 1.0);
            if (c >= 0) push(2 * out + (fwd ? 0 : 1), gs + tc + c, s);
        }
    }
    if (best[GOAL] == INF)
    {
        return route;
    }
    route.ok = true;
    route.cost = best[GOAL];
    // the states back from the goal
    std::vector<int32_t> chain;
    for (int32_t s = parent[GOAL]; s >= 0; s = parent[s])
    {
        chain.push_back(s);
    }
    std::reverse(chain.begin(), chain.end());
    route.line.push_back({ from_x, from_y, ps.at.z });
    route.line.push_back(ps.at);
    if (chain.empty())
    {
        route.edges.push_back(ps.edge);
        appendPart(g, ps.edge, ps.t, pt.t, route.line);
    }
    else
    {
        const int32_t s0 = chain.front();
        route.edges.push_back(ps.edge);
        appendPart(g, ps.edge, ps.t, s0 % 2 ? 0.0 : 1.0, route.line);
        route.nodes.push_back(at(s0));
        for (size_t i = 1; i < chain.size(); ++i)
        {
            const int32_t s = chain[i];
            route.edges.push_back(s / 2);
            appendPart(g, s / 2, s % 2 ? 1.0 : 0.0, s % 2 ? 0.0 : 1.0, route.line);
            route.nodes.push_back(at(s));
        }
        const int32_t last = route.nodes.back();
        route.edges.push_back(pt.edge);
        appendPart(g, pt.edge, g.edge(pt.edge).a == last ? 0.0 : 1.0, pt.t, route.line);
    }
    route.line.push_back({ to_x, to_y, pt.at.z });
    route.length = lineLength(route.line);

    // the legs: the start, and each junction with the edge it leaves on
    {
        Leg l;
        l.at = ps.at;
        l.edge = ps.edge;
        const Point& p = route.line[1];
        Point q = p;
        for (size_t i = 2; i < route.line.size(); ++i)
        {
            q = route.line[i];
            if (dist2d(p, q) >= 15.0) break;
        }
        l.bearing = compass(q.x - p.x, q.y - p.y);
        route.legs.push_back(l);
        double along = dist2d({ from_x, from_y, 0 }, ps.at) + (chain.empty() ? 0.0 : (chain.front() % 2 ? ps.t : 1.0 - ps.t) * g.edge(ps.edge).length);
        for (size_t i = 0; i < route.nodes.size(); ++i)
        {
            const int32_t n = route.nodes[i];
            const int32_t out = route.edges[i + 1];
            if (g.degree(n) >= 3)
            {
                Leg j;
                j.node = n;
                j.at = g.node(n).p;
                j.edge = out;
                j.bearing = g.bearingFrom(out, n);
                j.along = along;
                j.junction = true;
                route.legs.push_back(j);
            }
            if (i + 1 < route.nodes.size())
            {
                along += g.edge(out).length;
            }
        }
    }
    return route;
}

double offRoute(const Route& r, double x, double y, double* along)
{
    double best = 1e18, run = 0, at = 0;
    for (size_t i = 0; i + 1 < r.line.size(); ++i)
    {
        const Point& p = r.line[i];
        const Point& q = r.line[i + 1];
        const double sx = q.x - p.x, sy = q.y - p.y, L2 = sx * sx + sy * sy, L = std::sqrt(L2);
        const double f = L2 > 0 ? std::clamp(((x - p.x) * sx + (y - p.y) * sy) / L2, 0.0, 1.0) : 0.0;
        const double d = std::hypot(x - (p.x + f * sx), y - (p.y + f * sy));
        if (d < best)
        {
            best = d;
            at = run + f * L;
        }
        run += L;
    }
    if (along)
    {
        *along = at;
    }
    return best;
}

const Leg* legNear(const Route& r, double x, double y, double r_m, double along_m)
{
    const Leg* best = nullptr;
    double bd = r_m;
    for (const Leg& l : r.legs)
    {
        if (!l.junction || (along_m >= 0 && l.along < along_m - r_m))
        {
            continue;
        }
        const double d = std::hypot(l.at.x - x, l.at.y - y);
        if (d <= bd)
        {
            bd = d;
            best = &l;
        }
    }
    return best;
}

double bearingDiff(double a, double b)
{
    double d = std::fmod(std::fabs(a - b), 360.0);
    return d > 180.0 ? 360.0 - d : d;
}
}

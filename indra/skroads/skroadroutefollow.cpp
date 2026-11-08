/**
 * @file skroadroutefollow.cpp
 * @brief Spike: the road autopilot's committed route.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "skroadroutefollow.h"
#include "skroadroutedata.h"

#include "lltimer.h"

#include <cmath>
#include <memory>

namespace skroute
{
namespace follow
{
namespace
{
    std::unique_ptr<Graph> sGraph;
    std::string sGraphFrom;
    bool sGraphFailed = false;
    Route sRoute;
    bool sHave = false;
    Config sFor;                    // what the plan was made for
    double sPlannedAt = -100.0;
    double sNow = 0.0;
    bool sWantReplan = false;
    std::string sWhy;
    double sCarX = 0, sCarY = 0;

    bool loadGraph(const std::string& dir)
    {
        if (sGraph && sGraphFrom == dir)
        {
            return true;
        }
        if (sGraphFailed && sGraphFrom == dir)
        {
            return false;
        }
        sGraphFrom = dir;
        LLTimer t;
        auto g = std::make_unique<Graph>();
        LoadStats st;
        if (!loadDataset(dir, *g, st) || g->edgeCount() == 0)
        {
            LL_WARNS("SKRoadDriver") << "route: no road graph from " << dir << " (" << st.error << ")" << LL_ENDL;
            sGraphFailed = true;
            return false;
        }
        g->finalize();
        sGraph = std::move(g);
        sGraphFailed = false;
        LL_INFOS("SKRoadDriver") << "route: road graph " << st.nodes << " nodes, " << st.core << " public, " << st.priv
                                 << " private, " << st.candidates << " candidate ways from " << dir << " in "
                                 << S32(t.getElapsedTimeF64() * 1000) << " ms" << LL_ENDL;
        return true;
    }

    void makePlan(double car_x, double car_y, const Config& cfg, const char* what)
    {
        sPlannedAt = sNow;
        sWantReplan = false;
        sFor = cfg;
        LLTimer t;
        Route r = plan(*sGraph, costByName(cfg.cost), car_x, car_y, cfg.x, cfg.y);
        const S32 ms = S32(t.getElapsedTimeF64() * 1000);
        if (!r.ok)
        {
            LL_INFOS("SKRoadDriver") << "route: no " << cfg.cost << " route from " << S32(car_x) << "," << S32(car_y)
                                     << " to " << S32(cfg.x) << "," << S32(cfg.y) << " (" << what << ", " << ms << " ms)" << LL_ENDL;
            sHave = false;
            return;
        }
        LL_INFOS("SKRoadDriver") << "route " << what << (sWhy.empty() ? "" : " (" + sWhy + ")") << ": " << S32(r.length)
                                 << " m, " << r.legs.size() - 1 << " junctions, cost " << r.cost << " (" << cfg.cost << "), on "
                                 << S32(r.start_off) << " m from the car, target " << S32(r.target_off) << " m off the roads, "
                                 << r.expanded << " states, " << ms << " ms" << LL_ENDL;
        sRoute = std::move(r);
        sHave = true;
        sWhy.clear();
    }
}

void tick(double car_x, double car_y, double now, const Config& cfg)
{
    sNow = now;
    sCarX = car_x;
    sCarY = car_y;
    if (!cfg.has_target || cfg.dataset.empty() || !loadGraph(cfg.dataset))
    {
        sHave = false;
        return;
    }
    const bool changed = cfg.x != sFor.x || cfg.y != sFor.y || cfg.cost != sFor.cost || cfg.dataset != sFor.dataset;
    if (changed)
    {
        makePlan(car_x, car_y, cfg, "planned");
        return;
    }
    if (now - sPlannedAt < REPLAN_EVERY_S)
    {
        return;
    }
    if (sHave)
    {
        const double off = offRoute(sRoute, car_x, car_y);
        if (off > OFF_ROUTE_M)
        {
            sWhy = std::to_string(S32(off)) + " m off it";
            sWantReplan = true;
        }
    }
    else
    {
        sWantReplan = true;     // none yet: the car may have reached the graph since
    }
    if (sWantReplan)
    {
        makePlan(car_x, car_y, cfg, "replan");
    }
}

int pickBranch(double x, double y, const std::vector<double>& bearings)
{
    if (!sHave || bearings.empty())
    {
        return -1;
    }
    // the plan's way on: a planned junction's leg where one lies near, else the line about 30 m past where the
    // junction meets it (the model's junction and the dataset's need not be the same point)
    double want = -1;
    double along = 0;
    offRoute(sRoute, sCarX, sCarY, &along);
    if (const Leg* leg = legNear(sRoute, x, y, 25.0, along))
    {
        want = leg->bearing;
    }
    else
    {
        double at = 0;
        if (offRoute(sRoute, x, y, &at) > OFF_ROUTE_M)
        {
            sWhy = "a junction off the plan";
            sWantReplan = true;
            return -1;
        }
        double run = 0;
        for (size_t i = 0; i + 1 < sRoute.line.size(); ++i)
        {
            const Point& p = sRoute.line[i];
            const Point& q = sRoute.line[i + 1];
            run += std::hypot(q.x - p.x, q.y - p.y);
            if (run >= at + 30.0)
            {
                double b = std::atan2(q.x - x, q.y - y) * 180.0 / 3.14159265358979323846;
                want = b < 0 ? b + 360.0 : b;
                break;
            }
        }
        if (want < 0)
        {
            return -1;  // the target is within reach of the junction: any way on
        }
    }
    int best = -1;
    double bd = BRANCH_MATCH_DEG;
    for (size_t i = 0; i < bearings.size(); ++i)
    {
        const double d = bearingDiff(bearings[i], want);
        if (d <= bd)
        {
            bd = d;
            best = int(i);
        }
    }
    if (best < 0)
    {
        sWhy = "no branch within " + std::to_string(S32(BRANCH_MATCH_DEG)) + " deg of the plan's " + std::to_string(S32(want));
        sWantReplan = true;
        sPlannedAt = -100.0;    // at the next tick
    }
    else
    {
        LL_DEBUGS("SKRoadDriver") << "route: junction at " << S32(x) << "," << S32(y) << ": the branch bearing "
                                  << S32(bearings[best]) << " (plan " << S32(want) << ")" << LL_ENDL;
    }
    return best;
}

const Route* route()
{
    return sHave ? &sRoute : nullptr;
}

void reset()
{
    sHave = false;
    sFor = Config();
    sPlannedAt = -100.0;
}

void confirmLine(const std::vector<Point>& line)
{
    if (sGraph && sGraph->addLine(line, EDGE_CONFIRMED) >= 0)
    {
        sGraph->finalize();
    }
}
}
}

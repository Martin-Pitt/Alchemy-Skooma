/**
 * @file skroadroutefollow.h
 * @brief Spike: the road autopilot's committed route (SKRoadAutopilotGoal "route"): planned over the dataset's
 * graph at the start, again when the car is off it or took another branch, and asked at each junction which
 * branch carries it on.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */

#pragma once

#include "skroadroute.h"

#include <string>
#include <vector>

namespace skroute
{
namespace follow
{
    struct Config
    {
        std::string dataset;        // a release folder
        std::string cost;           // "shortest" | "fastest"
        bool has_target = false;
        double x = 0, y = 0;        // the target, global metres
    };

    constexpr double OFF_ROUTE_M = 30.0;        // further than this from the planned line: plan again
    constexpr double BRANCH_MATCH_DEG = 45.0;   // a branch this close to the plan's leg is the plan's
    constexpr double REPLAN_EVERY_S = 5.0;      // no sooner than this after the last plan

    // Once a frame while the goal is "route": loads the graph once, plans or plans again.
    void tick(double car_x, double car_y, double now, const Config& cfg);
    // The branch (compass bearings leaving the junction at (x, y)) the plan takes, -1 with no plan or none
    // within BRANCH_MATCH_DEG (which asks for a new plan).
    int pickBranch(double x, double y, const std::vector<double>& bearings);
    const Route* route();           // null without a plan
    void reset();                   // drops the plan (the graph stays loaded)
    // A road the car drove, confirmed by the live model, added to the graph.
    void confirmLine(const std::vector<Point>& line);
}
}

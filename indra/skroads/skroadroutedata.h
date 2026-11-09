/**
 * @file skroadroutedata.h
 * @brief Spike: the road dataset's network (schema 0.1.0: nodes.geojson, {core,private,candidates}/ways.geojson)
 * read into a route graph. Kept apart from the graph so other sources (the live model's confirmed roads) add to it.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */

#pragma once

#include "skroadroute.h"

namespace skroute
{
    struct LoadStats
    {
        int nodes = 0, core = 0, priv = 0, candidates = 0, skipped = 0;
        std::string error;
    };

    // Reads a release folder into g (not finalized). Candidate ways are included and flagged EDGE_CANDIDATE.
    bool loadDataset(const std::string& release_dir, Graph& g, LoadStats& stats, bool candidates = true);
}

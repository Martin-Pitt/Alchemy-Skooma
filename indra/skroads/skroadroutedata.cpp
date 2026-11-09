/**
 * @file skroadroutedata.cpp
 * @brief Spike: the road dataset's network read into a route graph.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */

#include "skroadroutedata.h"

#include <simdjson.h>

#include <unordered_map>

namespace skroute
{
namespace
{
    using simdjson::dom::element;

    Point pointOf(element c)
    {
        Point p;
        simdjson::dom::array a;
        if (c.get_array().get(a))
        {
            return p;
        }
        size_t i = 0;
        for (element v : a)
        {
            double d = 0;
            const bool num = !v.get_double().get(d);
            (i == 0 ? p.x : i == 1 ? p.y : p.z) = num ? d : 0.0;
            if (++i == 3)
            {
                break;
            }
        }
        return p;
    }

    std::string text(element obj, const char* key)
    {
        std::string_view v;
        return obj[key].get_string().get(v) ? std::string() : std::string(v);
    }
}

bool loadDataset(const std::string& dir, Graph& g, LoadStats& stats, bool candidates)
{
    simdjson::dom::parser parser;
    simdjson::dom::element nodes;
    simdjson::dom::array features;
    if (parser.load(dir + "/nodes.geojson").get(nodes) || nodes["features"].get_array().get(features))
    {
        stats.error = "no nodes.geojson in " + dir;
        return false;
    }
    std::unordered_map<std::string, int32_t> index;
    for (element f : features)
    {
        const std::string id = text(f, "id");
        element coords;
        if (id.empty() || f["geometry"]["coordinates"].get(coords))
        {
            continue;
        }
        index[id] = g.addNode(pointOf(coords), id);
        ++stats.nodes;
    }
    struct Layer
    {
        const char* file;
        uint8_t flags;
        int* count;
    };
    const Layer layers[] = {
        { "/core/ways.geojson", 0, &stats.core },
        { "/private/ways.geojson", EDGE_PRIVATE, &stats.priv },
        { "/candidates/ways.geojson", EDGE_CANDIDATE, &stats.candidates },
    };
    for (const Layer& layer : layers)
    {
        if (!candidates && (layer.flags & EDGE_CANDIDATE))
        {
            continue;
        }
        simdjson::dom::parser wp;
        element ways;
        simdjson::dom::array wf;
        if (wp.load(dir + layer.file).get(ways) || ways["features"].get_array().get(wf))
        {
            continue;   // a layer may be absent
        }
        for (element f : wf)
        {
            element pr;
            if (f["properties"].get(pr))
            {
                ++stats.skipped;
                continue;
            }
            const auto a = index.find(text(pr, "from_node"));
            const auto b = index.find(text(pr, "to_node"));
            const std::string kind = text(pr, "kind");
            simdjson::dom::array coords;
            if (a == index.end() || b == index.end() || a->second == b->second || (!kind.empty() && kind != "road")
                || f["geometry"]["coordinates"].get_array().get(coords))
            {
                ++stats.skipped;
                continue;
            }
            std::vector<Point> line;
            for (element c : coords)
            {
                line.push_back(pointOf(c));
            }
            uint8_t flags = layer.flags;
            if (text(pr, "access") == "private")
            {
                flags |= EDGE_PRIVATE;
            }
            bool oneway = false;
            if (!pr["oneway"].get_bool().get(oneway) && oneway)
            {
                flags |= EDGE_ONEWAY;
            }
            g.addEdge(a->second, b->second, line, flags);
            ++*layer.count;
        }
    }
    return true;
}
}

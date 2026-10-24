/**
 * @file skroomsflights_test.cpp
 * @brief Stairs, ramps and climbs found on the spaces' floor cells, over scenes the air's own raster builds.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Alchemy Viewer Source Code
 * Copyright (C) 2026, Alchemy Viewer Project.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include <catch2/catch_test_macros.hpp>

#include "../../skair/tests/skairtestscene.h"
#include "skairskymap.h"
#include "../skroomsenclosure.h"
#include "skroomstestworld.h"
#include "../skroomsflights.h"
#include "../skroomsgraph.h"
#include "../skroomsspaces.h"

#include "llformat.h"
#include "llmath.h"

#include <cmath>
#include <filesystem>
#include <functional>
#include <map>
#include <fstream>
#include <iostream>
#include <sstream>

namespace
{
    using namespace skworld;
    using namespace airscene;

    constexpr F32 CELL = 0.125f;   // the viewer's cell: the fit's radius is three of them

    /// A scene's records over flat land, rasterised tile by tile into air, then enclosure and spaces.
    struct World
    {
        std::vector<RecordPtr> mRecords;
        std::vector<std::pair<LLUUID, U8>> mCues;
        TerrainWindow mTerrain = flat(0.f, 72, -4.f, -4.f);
        S32 mTilesX = 1;
        std::shared_ptr<EnclosureProduct> mEnc;
        RoomsProductPtr mRooms;
        std::function<void(const FlightGraph&)> mGraphSink;    ///< given the floor-cell graph each build detects flights on

        void add(const RecordPtr& r) { mRecords.push_back(r); }

        void build()
        {
            // SKROOMS_FLIGHTS_DUMP names a file each build writes its floor-cell graph to, for the oracle.
            static const std::string dump = LLStringUtil::getenv("SKROOMS_FLIGHTS_DUMP");
            auto enc = std::make_shared<EnclosureInput>();
            enc->mGeneration = 1;
            enc->mParams.mWalkHeadM = 2.f;
            auto sky = std::make_shared<SkyMap>();
            for (S32 tx = 0; tx < mTilesX; ++tx)
            {
                TileInput in = airTile(tx, 0, CELL);
                in.mZMax = 14.f;
                in.mRecords = mRecords;
                in.mTerrain = mTerrain;
                in.mRootCues = mCues;
                std::sort(in.mRootCues.begin(), in.mRootCues.end());
                TileAirResult result;
                REQUIRE(rasteriseTile(in, result));
                REQUIRE(result.mAir);
                enc->mTiles.push_back({ TileKey{ tx, 0 }, result.mAir, static_cast<U64>(tx + 1) });
                sky->set(tx + 1, LLVector3d(result.mAir->mOrigin), *result.mAir);
            }
            enc->mSky = sky;
            EnclosurePass pass(enc);
            while (!pass.step(1u << 30))
            {
            }
            mEnc = pass.takeProduct();
            auto rooms = std::make_shared<RoomsInput>();
            rooms->mGeneration = 1;
            rooms->mParams.mWalkHeadM = 2.f;
            if (mGraphSink)
            {
                rooms->mFlightGraphSink = mGraphSink;
            }
            else if (!dump.empty())
            {
                rooms->mFlightGraphSink = [](const FlightGraph& g) {
                    std::ofstream f(dump.c_str(), std::ios::binary);
                    writeFlightGraph(f, g);
                };
            }
            mRooms = computeSpaces(rooms, mEnc);
        }

        std::string describe() const
        {
            std::ostringstream o;
            const FlightStats& st = mRooms->mFlightStats;
            o << "cells " << st.mCells << " candidates " << st.mCandidates << " regions " << st.mRegions << " flights " << mRooms->mFlights.size()
              << "\n";
            writeFlightsJson(o, mRooms->mFlights);
            return o.str();
        }

        std::vector<const AirFlight*> ofKind(FlightKind kind) const
        {
            std::vector<const AirFlight*> out;
            for (const AirFlight& f : mRooms->mFlights)
            {
                if (f.mKind == kind)
                {
                    out.push_back(&f);
                }
            }
            return out;
        }
    };

    /// Steps up +x from x0, each riser high and going deep, y0 to y1 wide, from z0; returns the top.
    F32 steps(World& w, F32 x0, F32 y0, F32 y1, F32 z0, S32 n, F32 riser, F32 going)
    {
        for (S32 k = 1; k <= n; ++k)
        {
            w.add(box(x0 + (k - 1) * going, y0, z0, x0 + k * going, y1, z0 + k * riser));
        }
        return z0 + n * riser;
    }

    /// The same up +y.
    F32 stepsY(World& w, F32 y0, F32 x0, F32 x1, F32 z0, S32 n, F32 riser, F32 going)
    {
        for (S32 k = 1; k <= n; ++k)
        {
            w.add(box(x0, y0 + (k - 1) * going, z0, x1, y0 + k * going, z0 + k * riser));
        }
        return z0 + n * riser;
    }

    /// A closed wedge rising +x from z0 at x0 to z1 at x1, y0 to y1 wide, under its slope.
    RecordPtr wedge(F32 x0, F32 y0, F32 x1, F32 y1, F32 z0, F32 z1, const LLUUID& root = LLUUID::null)
    {
        std::vector<LLVector3> t;
        const LLVector3 a(x0, y0, z0), b(x1, y0, z0), c(x1, y0, z1), d(x0, y1, z0), e(x1, y1, z0), f(x1, y1, z1);
        quad(t, a, d, f, c);       // the slope, wound up
        quad(t, a, b, e, d);       // the base, wound down
        quad(t, b, c, f, e);       // the high end
        t.insert(t.end(), { a, c, b, d, e, f });    // the two sides
        auto r = std::make_shared<Record>(*hull(t));
        r->mRoot = root;
        return r;
    }

    // The detection over a dumped graph (SKROOMS_FLIGHTS_GRAPH), each region traced in the oracle's verbose form to
    // SKROOMS_FLIGHTS_TRACE and the flights to SKROOMS_FLIGHTS_OUT: what the sweep diffs against flights.py.
    TEST_CASE("skroomsflights: a dumped graph, traced for the oracle", "[skroomsflights][.oracle]")
    {
        const std::string path = LLStringUtil::getenv("SKROOMS_FLIGHTS_GRAPH");
        if (path.empty())
        {
            return;
        }
        FlightGraph g;
        std::ifstream in(path.c_str(), std::ios::binary);
        REQUIRE(readFlightGraph(in, g));
        std::vector<AirFlight> flights;
        std::vector<U16> cell_flight;
        std::vector<FlightRegionTrace> trace;
        FlightStats st;
        detectFlights(g, flights, cell_flight, &st, &trace);
        std::ofstream tr(LLStringUtil::getenv("SKROOMS_FLIGHTS_TRACE").c_str());
        for (size_t i = 0; i < trace.size(); ++i)
        {
            const FlightRegionTrace& r = trace[i];
            tr << llformat("cells %u u (%.2f %.2f) s %.2f..%.2f rise %.2f run %.2f grade %.1f width %.2f area %.1f going %.2f decks %s %s fb %.2f ft %.2f cue %d -> %s\n",
                           r.mCells, r.mUx, r.mUy, r.mS0, r.mS1, r.mRise, r.mRun, r.mGrade, r.mWidth, r.mArea, r.mGoing, r.mDeckBottom ? "True" : "False",
                           r.mDeckTop ? "True" : "False", r.mFloorBottom, r.mFloorTop, r.mCue, r.mVerdict);
        }
        std::ofstream out(LLStringUtil::getenv("SKROOMS_FLIGHTS_OUT").c_str());
        writeFlightsJson(out, flights);
        std::cerr << "fitted " << st.mFitted << " candidates " << st.mCandidates << " regions " << st.mRegions << ", fit " << st.mFitMs << " ms, grow " << st.mGrowMs << " ms, regions " << st.mRegionsMs << " ms over " << st.mCells << " cells\n";
    }

    // A census built offline (SKROOMS_FLIGHTS_CENSUS, ';' between files) into air and rooms, every tile with its air:
    // the flights found per kind, and each mesh's floor-cell graph written to SKROOMS_FLIGHTS_SWEEP for the oracle.
    TEST_CASE("skroomsflights: flights over exported censuses, counted and dumped for the sweep", "[skroomsflights][.sweep]")
    {
        const std::string list = LLStringUtil::getenv("SKROOMS_FLIGHTS_CENSUS");
        const std::string dir = LLStringUtil::getenv("SKROOMS_FLIGHTS_SWEEP");
        if (list.empty())
        {
            return;
        }
        std::stringstream paths(list);
        std::string path;
        while (std::getline(paths, path, ';'))
        {
            std::map<LLUUID, U32> batches;
            const auto graphs = [&](const LLUUID& mesh, const FlightGraph& g) {
                if (dir.empty())
                {
                    return;
                }
                const std::string name = dir + "/" + std::filesystem::path(path).stem().string() + "_" + mesh.asString() + "_" +
                                         std::to_string(batches[mesh]++) + ".skfg";
                std::ofstream f(name.c_str(), std::ios::binary);
                writeFlightGraph(f, g);
            };
            CensusFile census;
            skrooms_test::World world;
            std::string error;
            REQUIRE(skrooms_test::buildWorld(path, skrooms_test::AirMode::EVERYWHERE, census, world, &error, graphs));
            U32 kinds[3] = {}, rules[3] = {}, cued = 0, decked = 0;
            F32 fit = 0.f, grow = 0.f, regions = 0.f;
            U32 cells = 0;
            for (const auto& m : world.mMeshes)
            {
                for (const AirFlight& f : m->mRooms->mFlights)
                {
                    ++kinds[static_cast<S32>(f.mKind)];
                    ++rules[static_cast<S32>(f.mRule)];
                    cued += f.mCue ? 1 : 0;
                    decked += f.mDecked ? 1 : 0;
                }
                const FlightStats& st = m->mRooms->mFlightStats;
                fit += st.mFitMs;
                grow += st.mGrowMs;
                regions += st.mRegionsMs;
                cells += st.mCells;
            }
            std::cerr << llformat("%s: %u tiles, %zu meshes; flights stair %u ramp %u climb %u (by name %u, grade %u, narrow %u; cued %u, decked %u); "
                                  "%u cells, fit %.1f ms, grow %.1f ms, regions %.1f ms\n",
                                  std::filesystem::path(path).stem().string().c_str(), world.mTiles, world.mMeshes.size(), kinds[0], kinds[1], kinds[2],
                                  rules[0], rules[1], rules[2], cued, decked, cells, fit, grow, regions);
        }
    }

    TEST_CASE("skroomsflights: a straight stepped stair is one flight, its rise, run and ends where it meets its floors", "[skroomsflights]")
    {
        World w;
        const F32 top = steps(w, 10.f, 10.f, 12.f, 0.f, 12, 0.2f, 0.3f);
        w.add(box(13.6f, 8.f, 0.f, 20.f, 14.f, top));
        w.build();
        INFO(w.describe());
        REQUIRE(w.mRooms->mFlights.size() == 1);
        const AirFlight& f = w.mRooms->mFlights[0];
        CHECK(f.mKind == FlightKind::STAIR);
        CHECK(std::fabs(f.mRise - top) < 0.2f);
        CHECK(std::fabs(f.mRun - 3.6f) < 0.5f);
        CHECK(std::fabs(f.mWidth - 2.f) < 0.3f);
        CHECK(std::fabs(f.mBottom.mV[VX] - 10.f) < 0.5f);
        CHECK(std::fabs(f.mTop.mV[VX] - 13.6f) < 0.5f);
        CHECK(f.mDecked);
        CHECK(f.mId != 0);
        CHECK(w.mRooms->flight(f.mId) == &f);
        // The spaces give it stair portals onto both floors, and its space is in no room.
        U32 stair_portals = 0;
        for (const Portal& p : w.mRooms->mPortals)
        {
            stair_portals += p.mKind == PortalKind::STAIR ? 1 : 0;
        }
        CHECK(stair_portals >= 2);
        CHECK(f.mSpaces.size() >= 2);
        const RoomGraphPtr graph = buildRoomGraph(*w.mRooms);
        for (U32 i = 0; i < w.mRooms->mSpaces.size(); ++i)
        {
            if (w.mRooms->mSpaces[i].mFlight)
            {
                CHECK(graph->ofSpace(i) == nullptr);
            }
        }
    }

    TEST_CASE("skroomsflights: smooth wedges by grade: 15 degrees a ramp, 30 a stair, 60 with a large face a climb", "[skroomsflights]")
    {
        struct Case
        {
            F32 mDeg;
            F32 mRise;
            F32 mWidth;
            FlightKind mKind;
        };
        for (const Case& c : { Case{ 15.f, 1.5f, 2.f, FlightKind::RAMP }, Case{ 30.f, 2.f, 2.f, FlightKind::STAIR }, Case{ 60.f, 4.f, 4.f, FlightKind::CLIMB } })
        {
            World w;
            const F32 run = c.mRise / std::tan(c.mDeg * DEG_TO_RAD);
            w.add(wedge(8.f, 10.f, 8.f + run, 10.f + c.mWidth, 0.f, c.mRise));
            w.add(box(8.f + run, 8.f, 0.f, 8.f + run + 6.f, 16.f, c.mRise));
            w.build();
            INFO(c.mDeg << " degrees: " << w.describe());
            REQUIRE(w.mRooms->mFlights.size() == 1);
            const AirFlight& f = w.mRooms->mFlights[0];
            CHECK(f.mKind == c.mKind);
            CHECK(std::fabs(f.mRise - c.mRise) < 0.25f);
            CHECK(std::fabs(std::atan2(f.mRise, f.mRun) * RAD_TO_DEG - c.mDeg) < 4.f);
        }
    }

    // A detection keeps scratch on its thread and, untraced, leaves a refused region as soon as its verdict is
    // known: the same graph gives the same flights and cells traced or not, and again after another graph.
    TEST_CASE("skroomsflights: a graph gives the same flights traced or not, and again after another graph", "[skroomsflights]")
    {
        FlightGraph graph, other;
        World w;
        w.mGraphSink = [&graph](const FlightGraph& g) { graph = g; };
        const F32 top = steps(w, 10.f, 10.f, 12.f, 0.f, 12, 0.2f, 0.3f);
        w.add(box(13.6f, 8.f, 0.f, 20.f, 14.f, top));
        w.add(wedge(2.f, 18.f, 30.f, 31.f, 0.f, 5.f));      // a hillside, over the uncued area: refused as it grows
        w.add(wedge(2.f, 2.f, 4.f, 4.f, 0.f, 0.3f));         // a bump, decked at neither end
        w.build();
        World v;
        v.mGraphSink = [&other](const FlightGraph& g) { other = g; };
        v.add(wedge(8.f, 10.f, 11.f, 12.f, 0.f, 1.5f));
        v.add(box(11.f, 8.f, 0.f, 17.f, 16.f, 1.5f));
        v.build();
        REQUIRE(!graph.mCells.empty());
        REQUIRE(!other.mCells.empty());

        const auto run = [](const FlightGraph& g, std::vector<FlightRegionTrace>* trace, std::vector<U16>& cells) {
            std::vector<AirFlight> flights;
            detectFlights(g, flights, cells, nullptr, trace);
            std::ostringstream o;
            writeFlightsJson(o, flights);
            return o.str();
        };
        std::vector<U16> plain_cells, traced_cells, again_cells, other_cells;
        std::vector<FlightRegionTrace> trace;
        const std::string plain = run(graph, nullptr, plain_cells);
        const std::string traced = run(graph, &trace, traced_cells);
        run(other, nullptr, other_cells);
        const std::string again = run(graph, nullptr, again_cells);
        INFO(plain);
        CHECK(!plain.empty());
        CHECK(traced == plain);
        CHECK(traced_cells == plain_cells);
        CHECK(again == plain);
        CHECK(again_cells == plain_cells);
        // The trace still holds every region: the hillside refused by its area, and one refused once measured.
        bool hillside = false, refused = false;
        for (const FlightRegionTrace& t : trace)
        {
            const std::string verdict = t.mVerdict;
            hillside = hillside || (verdict == "area" && t.mArea > FLIGHT_MAX_AREA_UNCUED_M2);
            refused = refused || verdict == "not decked" || verdict == "rise";
        }
        CHECK(hillside);
        CHECK(refused);
    }

    TEST_CASE("skroomsflights: an L-stair with a landing is two flights, each decked on the landing", "[skroomsflights]")
    {
        World w;
        const F32 mid = steps(w, 4.f, 4.f, 5.6f, 0.f, 8, 0.15f, 0.3f);
        w.add(box(6.4f, 4.f, 0.f, 8.f, 5.6f, mid));
        const F32 top = stepsY(w, 5.6f, 6.4f, 8.f, mid, 8, 0.15f, 0.3f);
        w.add(box(4.f, 8.f, 0.f, 12.f, 14.f, top));
        w.build();
        INFO(w.describe());
        REQUIRE(w.mRooms->mFlights.size() == 2);
        for (const AirFlight& f : w.mRooms->mFlights)
        {
            CHECK(f.mKind == FlightKind::STAIR);
            CHECK(std::fabs(f.mRise - 1.2f) < 0.2f);
            CHECK(f.mDecked);
        }
    }

    TEST_CASE("skroomsflights: a flight across a tile seam is one flight, whole", "[skroomsflights]")
    {
        World w;
        w.mTilesX = 2;
        const F32 top = steps(w, 30.f, 10.f, 12.f, 0.f, 16, 0.2f, 0.3f);
        w.add(box(34.8f, 8.f, 0.f, 40.f, 14.f, top));
        w.build();
        INFO(w.describe());
        REQUIRE(w.mRooms->mFlights.size() == 1);
        const AirFlight& f = w.mRooms->mFlights[0];
        CHECK(std::fabs(f.mBottom.mV[VX] - 30.f) < 0.5f);
        CHECK(std::fabs(f.mTop.mV[VX] - 34.8f) < 0.5f);
        CHECK(std::fabs(f.mRise - top) < 0.2f);
    }

    TEST_CASE("skroomsflights: land is no flight: a hill of terrain finds none", "[skroomsflights]")
    {
        World w;
        for (S32 y = 0; y < w.mTerrain.mHeight; ++y)
        {
            for (S32 x = 0; x < w.mTerrain.mWidth; ++x)
            {
                const F32 dx = w.mTerrain.mOrigin.mV[VX] + x - 16.f, dy = w.mTerrain.mOrigin.mV[VY] + y - 16.f;
                w.mTerrain.mZ[static_cast<size_t>(y) * w.mTerrain.mWidth + x] = 4.f * std::exp(-(dx * dx + dy * dy) / 40.f);
            }
        }
        w.build();
        INFO(w.describe());
        CHECK(w.mRooms->mFlights.empty());
    }

    TEST_CASE("skroomsflights: a gable roof with no deck at its ridge or eaves is no flight", "[skroomsflights]")
    {
        World w;
        w.add(box(10.f, 10.f, 0.f, 18.f, 18.f, 2.5f));
        w.add(wedge(10.f, 10.f, 14.f, 18.f, 2.5f, 4.5f));
        auto right = wedge(-18.f, 10.f, -14.f, 18.f, 2.5f, 4.5f);     // mirrored below
        std::vector<LLVector3> t = *right->mTriangles;
        for (LLVector3& p : t)
        {
            p.mV[VX] = -p.mV[VX];
        }
        for (size_t i = 0; i + 2 < t.size(); i += 3)
        {
            std::swap(t[i + 1], t[i + 2]);     // mirroring turns the winding
        }
        w.add(hull(t));
        w.build();
        INFO(w.describe());
        CHECK(w.mRooms->mFlights.empty());
    }

    TEST_CASE("skroomsflights: bleachers, treads deeper than a stair's, are seating, not a flight", "[skroomsflights]")
    {
        World w;
        const F32 top = steps(w, 8.f, 8.f, 16.f, 0.f, 5, 0.45f, 0.9f);
        w.add(box(12.5f, 8.f, 0.f, 18.f, 16.f, top));
        w.build();
        INFO(w.describe());
        CHECK(w.mRooms->mFlights.empty());
    }

    TEST_CASE("skroomsflights: a plank 0.6 m wide between two floors is no flight", "[skroomsflights]")
    {
        World w;
        w.add(wedge(8.f, 10.f, 12.f, 10.6f, 0.f, 1.5f));
        w.add(box(12.f, 8.f, 0.f, 18.f, 14.f, 1.5f));
        w.build();
        INFO(w.describe());
        CHECK(w.mRooms->mFlights.empty());
    }

    TEST_CASE("skroomsflights: a ramp named one is a ramp by its name, though too low to be one by its shape", "[skroomsflights]")
    {
        World w;
        LLUUID root;
        root.generate();
        w.mCues.push_back({ root, static_cast<U8>(CUE_RAMP) });
        w.add(wedge(8.f, 10.f, 11.4f, 12.f, 0.f, 0.9f, root));      // 15 degrees, under the metre an unnamed one needs
        w.add(box(11.4f, 8.f, 0.f, 18.f, 14.f, 0.9f));
        w.build();
        INFO(w.describe());
        REQUIRE(w.mRooms->mFlights.size() == 1);
        CHECK(w.mRooms->mFlights[0].mKind == FlightKind::RAMP);
        CHECK(w.mRooms->mFlights[0].mRule == FlightRule::NAME);
        CHECK(w.mRooms->mFlights[0].mCue == CUE_RAMP);

        World plain;
        plain.add(wedge(8.f, 10.f, 11.4f, 12.f, 0.f, 0.9f));
        plain.add(box(11.4f, 8.f, 0.f, 18.f, 14.f, 0.9f));
        plain.build();
        INFO(plain.describe());
        CHECK(plain.mRooms->mFlights.empty());
    }

    TEST_CASE("skroomsflights: a stair under a slab reports the headroom it leaves", "[skroomsflights]")
    {
        World w;
        const F32 top = steps(w, 10.f, 10.f, 12.f, 0.f, 12, 0.2f, 0.3f);
        w.add(box(13.6f, 8.f, 0.f, 20.f, 14.f, top));
        w.add(box(8.f, 9.f, 4.6f, 12.f, 13.f, 4.8f));        // over the lower half, 2.4 m and more above its steps
        w.build();
        INFO(w.describe());
        REQUIRE(w.mRooms->mFlights.size() == 1);
        const AirFlight& f = w.mRooms->mFlights[0];
        CHECK(f.mHeadroomMin > 2.f);
        CHECK(f.mHeadroomMin < 4.7f);
    }

    TEST_CASE("skroomsflights: a spiral stair, recorded whether found or not", "[skroomsflights]")
    {
        World w;
        // A quarter-turn-per-four-steps spiral about (16, 16), treads between radii 1 and 3.
        F32 z = 0.f;
        for (S32 k = 0; k < 16; ++k)
        {
            const F32 a0 = k * (F_PI / 8.f), a1 = (k + 1) * (F_PI / 8.f);
            z += 0.2f;
            std::vector<LLVector3> t;
            const LLVector3 p0(16.f + std::cos(a0), 16.f + std::sin(a0), 0.f), p1(16.f + 3.f * std::cos(a0), 16.f + 3.f * std::sin(a0), 0.f);
            const LLVector3 p2(16.f + 3.f * std::cos(a1), 16.f + 3.f * std::sin(a1), 0.f), p3(16.f + std::cos(a1), 16.f + std::sin(a1), 0.f);
            const LLVector3 up(0.f, 0.f, z);
            quad(t, p0 + up, p1 + up, p2 + up, p3 + up);
            quad(t, p0, p3, p2, p1);
            quad(t, p0, p0 + up, p3 + up, p3);
            quad(t, p1, p2, p2 + up, p1 + up);
            quad(t, p0, p1, p1 + up, p0 + up);
            quad(t, p3, p3 + up, p2 + up, p2);
            w.add(hull(t));
        }
        w.add(box(17.f, 12.f, 0.f, 24.f, 16.f, z));
        w.build();
        // A known gap when it finds none: a spiral's rise turns faster than a region joins across.
        WARN("spiral: " << w.describe());
        SUCCEED();
    }
}

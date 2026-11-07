/**
 * @file skroadhost.cpp
 * @brief The road model's host: what it reads of the viewer (census, properties, LDPW, meshes, air).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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

#include "llviewerprecompiledheaders.h"
#include "skroadhost.h"
#include "skroadmodel.h"
#include "skroadland.h"
#include "alobjectproperties.h"
#include "llagent.h"
#include "lldir.h"
#include "llfile.h"
#include "llglheaders.h"
#include "llnotificationsutil.h"
#include "llrender.h"
#include "llselectmgr.h"
#include "lltimer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvolume.h"
#include "llvovolume.h"
#include "llvoavatarself.h"
#include "llavatarnamecache.h"
#include "llsdjson.h"
#include "llparcel.h"
#include "llviewerparcelmgr.h"
#include "llworld.h"
#include "llviewerparceloverlay.h"
#include "llremoteparcelrequest.h"
#include <memory>
#include "llavatarpropertiesprocessor.h"
#include "skaircells.h"
#include "skairdistil.h"
#include "skworldcensus.h"
#include "skworldoverlay.h"
#include <boost/iterator/indirect_iterator.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace skroad;

namespace skroad::host
{
    // ---------------------------------------------------------------- LDPW's members, remembered

    // Who is in LDPW: the moles and the Lindens who build the roads. Learnt two ways, and kept across sessions
    // in sk-road-ldpw-owners.json (logs), so it grows into a list of them:
    //  - only a member can set an object to a group, so the owner of any object in LDPW is a member;
    //  - an owner's profile listing LDPW (asked once per owner; one hiding it reads as not listed).
    // {"members": {"<id>": {"name", "how": "object" | "profile", "t": unix}}, "not_listed": {"<id>": unix}}
    struct LdpwMembers
    {
        LLSD members = LLSD::emptyMap();
        LLSD not_listed = LLSD::emptyMap();
        bool loaded = false;
        bool dirty = false;
        LLTimer since_save;

        std::string path() const { return gDirUtilp->getExpandedFilename(LL_PATH_LOGS, "sk-road-ldpw-owners.json"); }

        void load()
        {
            if (loaded)
            {
                return;
            }
            loaded = true;
            llifstream in(path().c_str());
            if (!in.is_open())
            {
                return;
            }
            std::stringstream text;
            text << in.rdbuf();
            LLSD data;
            if (LlsdFromJsonString(text.str(), data) && data.isMap())
            {
                members = data.has("members") ? data["members"] : LLSD::emptyMap();
                not_listed = data.has("not_listed") ? data["not_listed"] : LLSD::emptyMap();
            }
        }

        void save()
        {
            if (!dirty || since_save.getElapsedTimeF64() < 10.0)
            {
                return;
            }
            dirty = false;
            since_save.reset();
            LLSD data;
            data["members"] = members;
            data["not_listed"] = not_listed;
            llofstream out(path().c_str(), std::ios::out | std::ios::trunc);
            out << LlsdToJson(data);
        }

        // 1 member, 0 not listed, -1 not known
        S32 status(const LLUUID& who)
        {
            load();
            const std::string k = who.asString();
            return members.has(k) ? 1 : not_listed.has(k) ? 0 : -1;
        }

        void learn(const LLUUID& who, bool member, const char* how)
        {
            load();
            const std::string k = who.asString();
            if (member && !members.has(k))
            {
                LLSD e;
                LLAvatarName name;
                e["name"] = LLAvatarNameCache::get(who, &name) ? name.getUserName() : std::string();
                e["how"] = how;
                e["t"] = LLSD::Real(LLTimer::getTotalSeconds().value());
                members[k] = e;
                not_listed.erase(k);
                dirty = true;
                LL_INFOS("SKRoadDriver") << "LDPW member learnt (" << how << "): " << e["name"].asString() << " " << k << LL_ENDL;
            }
            else if (!member && !members.has(k) && !not_listed.has(k))
            {
                not_listed[k] = LLSD::Real(LLTimer::getTotalSeconds().value());
                dirty = true;
            }
        }

        // a member learnt before its name was known: the name once it is
        void fillNames()
        {
            for (auto it = members.beginMap(); it != members.endMap(); ++it)
            {
                if (it->second["name"].asString().empty())
                {
                    LLAvatarName name;
                    if (LLAvatarNameCache::get(LLUUID(it->first), &name))
                    {
                        it->second["name"] = name.getUserName();
                        dirty = true;
                    }
                }
            }
        }
    };
    LdpwMembers sLdpw;

    // Owners' public groups, from their profiles: a mole is in LDPW whatever its name (moles may now change
    // their last names), so an owner listing LDPW is a Linden owner.
    struct OwnerGroups : public LLAvatarPropertiesObserver
    {
        std::unordered_map<LLUUID, S32> state;     // -1 asked, 0 not in LDPW, 1 in LDPW
        void processProperties(void* data, EAvatarProcessorType type) override
        {
            LLUUID who;
            bool ldpw = false;
            if (type == APT_PROPERTIES)
            {
                const LLAvatarData* d = static_cast<const LLAvatarData*>(data);
                who = d->avatar_id;
                for (const auto& g : d->group_list)
                {
                    ldpw = ldpw || g.group_id == LDPW;
                }
            }
            else if (type == APT_GROUPS)
            {
                const LLAvatarGroups* d = static_cast<const LLAvatarGroups*>(data);
                who = d->avatar_id;
                for (const auto& g : d->group_list)
                {
                    ldpw = ldpw || g.group_id == LDPW;
                }
            }
            else
            {
                return;
            }
            auto it = state.find(who);
            if (it != state.end())
            {
                it->second = ldpw ? 1 : 0;
                sLdpw.learn(who, ldpw, "profile");
                LLAvatarPropertiesProcessor::getInstance()->removeObserver(who, this);
            }
        }
    };
    OwnerGroups sOwnerGroups;

    // Is the owner a Linden or a mole: Governor Linden (most older roads), an account whose legacy last name
    // is Linden or Mole, or a member of LDPW (ldpwMember). False until its name or profile has arrived (both
    // asked for here).
    bool lindenOwner(const LLUUID& owner)
    {
        if (owner == GOVERNOR_LINDEN)
        {
            return true;
        }
        if (owner.isNull())
        {
            return false;
        }
        const S32 known = sLdpw.status(owner);
        if (known == 1)
        {
            return true;
        }
        auto g = sOwnerGroups.state.find(owner);
        if (known < 0 && g == sOwnerGroups.state.end())
        {
            sOwnerGroups.state[owner] = -1;
            LLAvatarPropertiesProcessor::getInstance()->addObserver(owner, &sOwnerGroups);
            LLAvatarPropertiesProcessor::getInstance()->sendAvatarPropertiesRequest(owner);
        }
        else if (g->second == 1)
        {
            return true;
        }
        LLAvatarName name;
        if (!LLAvatarNameCache::get(owner, &name))
        {
            LLAvatarNameCache::get(owner, [](const LLUUID&, const LLAvatarName&) {});     // ask; known next time
            return false;
        }
        std::string legacy = name.getLegacyName();
        LLStringUtil::toLower(legacy);
        return LLStringUtil::endsWith(legacy, " linden") || LLStringUtil::endsWith(legacy, " mole");
    }
    // lindenOwner in three states: 1 a Linden or a mole, 0 known not, -1 still being learnt (its profile or name
    // not in yet), which the road guard takes as not known, never as someone else's.
    S32 lindenOwnerState(const LLUUID& owner)
    {
        if (lindenOwner(owner))
        {
            return 1;
        }
        if (owner.isNull())
        {
            return 0;
        }
        auto g = sOwnerGroups.state.find(owner);
        const bool profile_pending = sLdpw.status(owner) < 0 && g != sOwnerGroups.state.end() && g->second < 0;
        LLAvatarName name;
        return profile_pending || !LLAvatarNameCache::get(owner, &name) ? -1 : 0;
    }
    void sortAsks(const LLVector3d& car);

    // The part's textures, once its object is live.
    // The texture of the part's face on top: the face whose normal points most up, as the object lies.
    void learnTop(PieceShapes& piece, const LLViewerObject& object)
    {
        const LLVolume* volume = object.getVolume();
        if (!volume || volume->getNumVolumeFaces() <= 0)
        {
            return;
        }
        const LLQuaternion rot = object.getRotationRegion();
        const LLVector3 scale = object.getScale();
        S32 best = -1;
        F32 best_up = -2.f;
        for (S32 i = 0; i < volume->getNumVolumeFaces(); ++i)
        {
            const LLVolumeFace& f = volume->getVolumeFace(i);
            if (f.mNumVertices <= 0 || !f.mNormals)
            {
                continue;
            }
            LLVector4a sum;
            sum.clear();
            for (S32 k = 0; k < f.mNumVertices; ++k)
            {
                sum.add(f.mNormals[k]);
            }
            const F32* n4 = sum.getF32ptr();
            LLVector3 n(n4[0] / llmax(scale.mV[VX], 1e-3f), n4[1] / llmax(scale.mV[VY], 1e-3f), n4[2] / llmax(scale.mV[VZ], 1e-3f));
            n = n * rot;
            if (n.normVec() < 1e-6f)
            {
                continue;
            }
            if (n.mV[VZ] > best_up)
            {
                best_up = n.mV[VZ];
                best = f.mID >= 0 ? f.mID : i;
            }
        }
        if (best >= 0 && best < object.getNumTEs())
        {
            if (const LLTextureEntry* te = object.getTE(U8(best)))
            {
                piece.top_tex = te->getID();
                piece.top_known = true;
            }
        }
    }

    void learnTextures(PieceShapes& piece)
    {
        if (piece.textures_known && piece.top_known)
        {
            return;
        }
        const LLViewerObject* object = gObjectList.findObject(piece.part);
        if (object && !piece.top_known)
        {
            learnTop(piece, *object);
        }
        if (object && !piece.textures_known)
        {
            piece.textures.clear();
            for (U8 i = 0; i < object->getNumTEs(); ++i)
            {
                if (const LLTextureEntry* te = object->getTE(i))
                {
                    piece.textures.push_back(te->getID());
                }
            }
            std::sort(piece.textures.begin(), piece.textures.end());
            piece.textures.erase(std::unique(piece.textures.begin(), piece.textures.end()), piece.textures.end());
            piece.textures_known = !piece.textures.empty();
            piece.match_gen = 0;
        }
    }
    // A few properties requests a second for pieces not yet known to be road or not.
    // What the linksets in reach are (name, group), nearest first: until it is known a linkset counts as road
    // by its shape, so a car park or a station floor beside the road is read as road while unasked. One not
    // loaded yet, or not answered within ASK_RETRY_S, is asked again.
    constexpr F64 ASK_EVERY_S = 0.05;
    constexpr F64 ASK_RETRY_S = 5.0;
    // The linksets waiting to be asked, nearest the car first; one not loaded last.
    void sortAsks(const LLVector3d& car)
    {
        static std::vector<std::pair<F64, LLUUID>> by_d;
        by_d.clear();
        for (const LLUUID& root : sToAsk)
        {
            const LLViewerObject* o = gObjectList.findObject(root);
            by_d.push_back({ o ? (o->getPositionGlobal() - car).magVecSquared() : 1e17, root });
        }
        std::stable_sort(by_d.begin(), by_d.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        sToAsk.clear();
        for (const auto& [d, root] : by_d)
        {
            sToAsk.push_back(root);
        }
    }

    void askProperties()
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - askProperties");
        const F64 now = sAskClock.getElapsedTimeF64();
        if (sSinceAsk.getElapsedTimeF64() < ASK_EVERY_S)
        {
            return;
        }
        static F64 swept = -1.0;
        if (now - swept >= 1.0)
        {
            swept = now;
            for (auto it = sAsked.begin(); it != sAsked.end();)
            {
                if (it->second >= 0 && now - it->second > ASK_RETRY_S && !ALObjectPropertiesCache::instance().get(it->first))
                {
                    it = sAsked.erase(it);  // no answer: queued again when next in reach
                }
                else
                {
                    ++it;
                }
            }
        }
        // the road ahead first: pieces the chain reached with no name yet, unless asked a moment ago
        while (!sAskFirst.empty())
        {
            const LLUUID root = sAskFirst.front();
            sAskFirst.pop_front();
            sAskFirstSet.erase(root);
            auto asked = sAsked.find(root);
            if (ALObjectPropertiesCache::instance().get(root) || (asked != sAsked.end() && asked->second >= 0 && now - asked->second < 1.0))
            {
                continue;
            }
            if (LLViewerObject* object = gObjectList.findObject(root); object && object->getRegion())
            {
                LLSelectMgr::getInstance()->requestObjectPropertiesFamily(object);
                sAsked[root] = now;
                sSinceAsk.reset();
                return;
            }
        }
        if (sToAsk.empty())
        {
            return;
        }
        // the nearest waiting (sortAsks, every shapes refresh)
        const LLUUID root = sToAsk.front();
        sToAsk.pop_front();
        if (ALObjectPropertiesCache::instance().get(root))
        {
            return;
        }
        LLViewerObject* object = gObjectList.findObject(root);
        if (object && object->getRegion())
        {
            LLSelectMgr::getInstance()->requestObjectPropertiesFamily(object);
            sAsked[root] = now;
            sSinceAsk.reset();
        }
        else
        {
            sAsked.erase(root);     // not loaded yet: queued again when it is
        }
    }
    // The published air tiles about the car, again whenever the air changes (every half second at most).
    void refreshAir(const LLVector3d& car, F64 reach)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("sk road - refreshAir");
        if (!SKAirCells::instanceExists())
        {
            return;
        }
        SKAirCells& cells = SKAirCells::instance();
        const LLVector3d lo(car.mdV[VX] - reach, car.mdV[VY] - reach, car.mdV[VZ] - 20);
        const LLVector3d hi(car.mdV[VX] + reach, car.mdV[VY] + reach, car.mdV[VZ] + 20);
        if (sSinceAir.getElapsedTimeF64() > 2.0)
        {
            cells.requestAir(AIR_DEMAND, lo, hi);       // keep the tiles about the car built in detail
        }
        if (cells.revision() == sAirRevision && sSinceAir.getElapsedTimeF64() < 2.0)
        {
            return;
        }
        if (sSinceAir.getElapsedTimeF64() < 0.5 && !sAirTiles.empty())
        {
            return;
        }
        sSinceAir.reset();
        sAirRevision = cells.revision();
        sAirTiles.clear();
        std::vector<SKAirCells::MeshInfo> meshes;
        cells.snapshotMeshes(meshes);
        for (const SKAirCells::MeshInfo& m : meshes)
        {
            const SKAirCells::AirSnapshot snap = cells.airSnapshot(m.mId);
            for (const SKAirCells::AirSnapshotTile& t : snap.mTiles)
            {
                if (!t.mAir || t.mMax.mdV[VX] < lo.mdV[VX] || t.mMin.mdV[VX] > hi.mdV[VX] || t.mMax.mdV[VY] < lo.mdV[VY]
                    || t.mMin.mdV[VY] > hi.mdV[VY])
                {
                    continue;   // only detail air: an evicted tile's coarse air has no floors fine enough
                }
                AirTile a;
                a.air = t.mAir;
                a.ox = snap.mOrigin.mdV[VX] + t.mAir->mOrigin.mV[VX];
                a.oy = snap.mOrigin.mdV[VY] + t.mAir->mOrigin.mV[VY];
                a.oz = snap.mOrigin.mdV[VZ] + t.mAir->mOrigin.mV[VZ];
                a.x0 = a.ox;
                a.y0 = a.oy;
                a.x1 = a.ox + t.mAir->mWidth * t.mAir->mCell;
                a.y1 = a.oy + t.mAir->mHeight * t.mAir->mCell;
                sAirTiles.push_back(std::move(a));
            }
        }
    }

    void tickLdpw()
    {
        static LLTimer since_names;
        if (since_names.getElapsedTimeF64() >= 1.0)
        {
            since_names.reset();
            sLdpw.fillNames();
        }
        sLdpw.save();
    }
}

// The road model's host (skroadmodel.h): what it reads of the viewer.
namespace skroad::host
{

    CensusPtr census()
    {
        static SKCensusSnapshotPtr last;
        static std::shared_ptr<const CensusView> view;
        SKCensusSnapshotPtr snap = SKWorldCensus::instance().snapshot();
        if (!snap)
        {
            last.reset();
            view.reset();
            return nullptr;
        }
        if (snap != last)
        {
            last = snap;
            view = std::make_shared<const CensusView>(CensusView{ snap->mStamp, snap->mRecords, snap->mAdded, snap->mRemoved, snap->mChanged });
        }
        return CensusPtr(snap, view.get());     // owned as the scan is: one scan, one owner
    }

    const Props* properties(const LLUUID& root)
    {
        static Props props;
        const ALObjectPropertiesCache::ServerProps* p = ALObjectPropertiesCache::instance().get(root);
        if (!p)
        {
            return nullptr;
        }
        props.mName = p->mName;
        props.mOwnerId = p->mOwnerId;
        props.mGroupId = p->mGroupId;
        return &props;
    }

    bool hasProperties(const LLUUID& root)
    {
        return ALObjectPropertiesCache::instance().get(root) != nullptr;
    }

    bool watchProperties(std::function<void(const LLUUID&)> changed)
    {
        static boost::signals2::connection props = ALObjectPropertiesCache::instance().setChangeCallback(changed);
        return props.connected();
    }


    void learnLdpw(const LLUUID& owner)
    {
        sLdpw.learn(owner, true, "object");
    }

    S32 parcelOwnership(F64 x, F64 y)
    {
        const LLVector3d at(x, y, 0);
        LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(at);
        const LLViewerParcelOverlay* overlay = region ? region->getParcelOverlay() : nullptr;
        return overlay ? S32(overlay->ownership(region->getPosRegionFromGlobal(at)) & PARCEL_COLOR_MASK) : -1;
    }

    S32 roadLandAt(F64 x, F64 y, std::string* name)
    {
        const land::ParcelRoad p = land::parcelRoadAt(x, y);
        if (name)
        {
            *name = p.named ? p.name : std::string("(name not in)");
        }
        return p.destination ? 2 : p.road == land::Road::Linden ? 1 : p.road == land::Road::Other ? 0 : -1;
    }

    bool privateRoadsAllowed()
    {
        static LLCachedControl<bool> allowed(gSavedSettings, "SKRoadAutopilotPrivateRoads", false);
        return allowed;
    }


    bool meshPlacement(const skworld::Record& r, MeshPlacement& out)
    {
        LLViewerObject* object = gObjectList.findObject(r.mPart);
        if (!object || object->getPCode() != LL_PCODE_VOLUME || !(object->isMesh() || object->isSculpted()) || !object->getVolume()
            || !object->getRegion() || (object->getRegion()->getOriginGlobal() - r.mOrigin).magVec() > 0.5)
        {
            return false;
        }
        const LLVolumeParams& params = object->getVolume()->getParams();
        out.sculpt_id = params.getSculptID();
        out.sculpt_type = params.getSculptType();
        out.physics_shape = object->getPhysicsShapeType();
        out.pos = object->getPositionRegion();
        out.rot = object->getRotationRegion();
        out.scale = object->getScale();
        return true;
    }

    bool rootRotation(const LLUUID& root, LLQuaternion& rot)
    {
        const LLViewerObject* ro = gObjectList.findObject(root);
        if (!ro)
        {
            return false;
        }
        rot = ro->getRotationRegion();
        return true;
    }


    F32 lookAheadSetting()
    {
        static LLCachedControl<F32> least(gSavedSettings, "SKRoadAutopilotLookAhead", 100.f);
        return least;
    }

    std::string goal()
    {
        static LLCachedControl<std::string> goal(gSavedSettings, "SKRoadAutopilotGoal", "explore");
        return goal;
    }

    bool surveyOn()
    {
        static LLCachedControl<bool> survey(gSavedSettings, "SKRoadSurvey", false);
        return survey;
    }

    std::string logPath(const std::string& file)
    {
        return gDirUtilp->getExpandedFilename(LL_PATH_LOGS, file);
    }

    TestCourse sTestCourse;
    void setTestCourse(TestCourse course)
    {
        sTestCourse = std::move(course);
    }

    std::vector<LanePoint> testCourse(const std::string& name, F64 x, F64 y, F64 z, F64 hx, F64 hy, F64 v)
    {
        return sTestCourse ? sTestCourse(name, x, y, z, hx, hy, v) : std::vector<LanePoint>();
    }
}

/**
 * @file skmapallies_test.cpp
 * @brief Mini-map ally rules: the faction table, candidates, the probe state machine, the budget.
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

#include "../test/lltut.h"

#include "../skmapallies.h"

#include "llfile.h"

#include <iterator>

namespace
{
    using skmap::ProbeState;

    const LLUUID ASH_G("64064c89-25aa-008b-cfe7-0c34e28ae523");
    const LLUUID ASH_M("86376bab-a5c9-d551-1866-7f6ac222c96f");
    const LLUUID ASH_L("127ed99a-37c2-c620-4908-85da85180ea9");
    const LLUUID ASH_CIVILIANS("a15cd794-e272-a2af-cfd3-faa66b68dc1e");
    const LLUUID MEGIDDO_CIVILIANS("4660039a-3c80-7982-5f01-e36b56636c69");
    const LLUUID ROME_G("f77172ef-4382-365a-2f3f-13463271527d");
    const LLUUID ROME_M("e22ec527-b3d1-3abe-106c-2b9d467fb920");
    const LLUUID SH_L("bf7d2293-ed35-9dc9-141b-d73ed562c548");
    const LLUUID CDF_L("0a22668d-9443-125c-8d42-2a681fd6eabe");
    const LLUUID OTHER("0f0f0f0f-0000-1111-2222-333333333333");

    std::string shipped()
    {
        return std::string(SKMAP_APP_SETTINGS) + "/sk_slmc_factions.xml";
    }

    skmap::FactionTable shippedTable()
    {
        skmap::FactionTable table;
        tut::ensure("the shipped table loads", table.load(shipped()));
        return table;
    }

    LLUUID root(S32 i)
    {
        LLUUID id;
        id.mData[0] = static_cast<U8>(0xa0 + i);
        return id;
    }
}

namespace tut
{
    struct skmapallies_data {};
    typedef test_group<skmapallies_data> skmapallies_group;
    typedef skmapallies_group::object skmapallies_object;
    tut::skmapallies_group g("skmapallies");

    // An inline table: null, empty and invalid ids are dropped; a key holds one id or an array.
    template<> template<>
    void skmapallies_object::test<1>()
    {
        LLSD sd;
        LLSD a;
        a["name"] = "A";
        a["g"] = LLSD(ASH_G);
        a["m"] = "not a uuid";
        LLSD l = LLSD::emptyArray();
        l.append(LLSD(LLUUID::null));
        l.append("");
        l.append(ASH_L.asString());
        a["l"] = l;
        a["c"] = ASH_CIVILIANS.asString();
        sd["factions"].append(a);
        LLSD empty;
        empty["name"] = "nothing valid";
        empty["g"] = "";
        sd["factions"].append(empty);
        const skmap::FactionTable table = skmap::FactionTable::fromLLSD(sd);
        ensure_equals("one faction", table.factions().size(), static_cast<size_t>(1));
        ensure("g and l, nothing else", table.factions()[0].mGroups == std::set<LLUUID>({ ASH_G, ASH_L }));
        ensure("c is not read", table.factionOf(ASH_CIVILIANS) == nullptr);
    }

    // Candidates from the shipped table.
    template<> template<>
    void skmapallies_object::test<2>()
    {
        const skmap::FactionTable table = shippedTable();
        ensure("Ashguard militia gives the faction", table.candidatesFor(ASH_M) == std::set<LLUUID>({ ASH_G, ASH_M, ASH_L }));
        ensure("New Rome deduped, its militia gives it", table.candidatesFor(ROME_M) == std::set<LLUUID>({ ROME_G, ROME_M }));
        ensure("another group is its own", table.candidatesFor(OTHER) == std::set<LLUUID>({ OTHER }));
        ensure("no group is nobody", table.candidatesFor(LLUUID::null).empty());
        ensure("Ashguard civilians are just themselves", table.candidatesFor(ASH_CIVILIANS) == std::set<LLUUID>({ ASH_CIVILIANS }));
        ensure("Megiddo civilians are just themselves", table.candidatesFor(MEGIDDO_CIVILIANS) == std::set<LLUUID>({ MEGIDDO_CIVILIANS }));
    }

    // The shipped file: 13 factions, no c key, SH and CDF with their land groups.
    template<> template<>
    void skmapallies_object::test<3>()
    {
        const skmap::FactionTable table = shippedTable();
        ensure_equals("13 factions", table.factions().size(), static_cast<size_t>(13));
        const skmap::Faction* sh = table.factionOf(SH_L);
        ensure("SH holds its land group", sh && sh->mShort == "SH");
        const skmap::Faction* cdf = table.factionOf(CDF_L);
        ensure("CDF holds its land group", cdf && cdf->mShort == "CDF");
        llifstream file(shipped().c_str());
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        ensure("no c key", text.find("<key>c</key>") == std::string::npos);
        skmap::FactionTable missing;
        ensure("a missing file fails", !missing.load(shipped() + ".nope"));
    }

    // The state machine, every transition.
    template<> template<>
    void skmapallies_object::test<4>()
    {
        const std::vector<LLUUID> roots = { root(1), root(2) };
        skmap::AllyRecord r;
        skmap::observe(r, "Soldier", roots, 0.0, false);
        ensure("unknown first", r.mState == ProbeState::UNKNOWN);
        ensure("wants a probe", skmap::wantsProbe(r, 0.0, true));
        ensure("not without roots", !skmap::wantsProbe(r, 0.0, false));
        ensure("the first root", skmap::chooseTarget({}, roots) == root(1));
        skmap::probeSent(r, root(1), roots, 0.0);
        ensure("pending", r.mState == ProbeState::PENDING && !skmap::wantsProbe(r, 0.1, true));
        skmap::probeReply(r, root(1), ASH_G, 0.5);
        ensure("resolved", r.mState == ProbeState::RESOLVED && r.mGroup == ASH_G);

        // A new root re-probes it while the group holds.
        const std::vector<LLUUID> more = { root(1), root(2), root(3) };
        skmap::observe(r, "Soldier", more, 10.0, false);
        ensure("re-probe on a new root", skmap::wantsProbe(r, 10.0, true));
        ensure("an unasked root is the target", skmap::chooseTarget(r.mAsked, more) == root(2));
        skmap::probeSent(r, root(2), more, 10.0);
        ensure("the group holds meanwhile", r.mState == ProbeState::RESOLVED && r.mGroup == ASH_G);
        skmap::probeReply(r, root(2), LLUUID::null, 10.5);
        ensure("a null group is none", r.mState == ProbeState::NONE);
        ensure("none waits", !skmap::wantsProbe(r, 20.0, true));
        ensure("then retries after 30 s", skmap::wantsProbe(r, 40.6, true));

        // A timeout drops back to unknown and retries later.
        skmap::probeSent(r, root(1), more, 41.0);
        skmap::probeTimeout(r, 45.0);
        ensure("not yet timed out", r.mInFlight);
        skmap::probeTimeout(r, 46.0);
        ensure("timed out to unknown", r.mState == ProbeState::UNKNOWN && !r.mInFlight);
        ensure("retry waits 30 s", !skmap::wantsProbe(r, 50.0, true) && skmap::wantsProbe(r, 76.0, true));

        // An empty title is not probed unless asked.
        skmap::AllyRecord u;
        skmap::observe(u, "", roots, 0.0, false);
        ensure("untitled", u.mState == ProbeState::UNTITLED && !skmap::wantsProbe(u, 0.0, true));
        skmap::AllyRecord p;
        skmap::observe(p, "", roots, 0.0, true);
        ensure("probed when untitled ones are", p.mState == ProbeState::UNKNOWN && skmap::wantsProbe(p, 0.0, true));
    }

    // A title change on a resolved record drops the group in the same step and asks afresh.
    template<> template<>
    void skmapallies_object::test<5>()
    {
        const std::vector<LLUUID> roots = { root(1) };
        skmap::AllyRecord r;
        skmap::observe(r, "Soldier", roots, 0.0, false);
        skmap::probeSent(r, root(1), roots, 0.0);
        skmap::probeReply(r, root(1), ASH_G, 0.2);
        skmap::observe(r, "Civilian", roots, 5.0, false);
        ensure("unknown at once", r.mState == ProbeState::UNKNOWN && r.mGroup.isNull());
        ensure("asks at once", skmap::wantsProbe(r, 5.0, true));
        ensure("with a fresh request", r.mFresh);
        ensure("no longer an ally", skmap::allyVerdict(r, { ASH_G }) == skmap::AllyVerdict::UNKNOWN);

        // A pending record also drops on a title change; a cleared tag goes untitled.
        skmap::probeSent(r, root(1), roots, 5.0);
        skmap::observe(r, "", roots, 6.0, false);
        ensure("untitled", r.mState == ProbeState::UNTITLED && !r.mInFlight);

        // A reply for a root that is not pending changes nothing.
        skmap::AllyRecord q;
        skmap::observe(q, "Soldier", roots, 0.0, false);
        skmap::probeSent(q, root(1), roots, 0.0);
        skmap::probeReply(q, root(9), ASH_G, 0.1);
        ensure("ignored", q.mState == ProbeState::PENDING && q.mGroup.isNull());
        skmap::AllyRecord idle;
        skmap::observe(idle, "Soldier", roots, 0.0, false);
        skmap::probeReply(idle, root(1), ASH_G, 0.1);
        ensure("nothing asked, nothing taken", idle.mState == ProbeState::UNKNOWN);
    }

    // An answer that is no candidate asks the next root at once, until one is or none is left.
    template<> template<>
    void skmapallies_object::test<8>()
    {
        const std::set<LLUUID> candidates = { ASH_G, ASH_M, ASH_L };
        const std::vector<LLUUID> roots = { root(1), root(2), root(3) };
        skmap::AllyRecord r;
        skmap::observe(r, "Soldier", roots, 0.0, false);
        skmap::probeSent(r, skmap::chooseTarget(r.mAsked, roots), roots, 0.0);
        skmap::probeReply(r, root(1), OTHER, 0.1);
        skmap::askNextRoot(r, roots, candidates);
        ensure("asks again at once", skmap::wantsProbe(r, 0.1, true));
        ensure("the second root", skmap::chooseTarget(r.mAsked, roots) == root(2));
        skmap::probeSent(r, root(2), roots, 0.1);
        skmap::probeReply(r, root(2), LLUUID::null, 0.2);
        skmap::askNextRoot(r, roots, candidates);
        ensure("past a null group too", skmap::wantsProbe(r, 0.2, true) && skmap::chooseTarget(r.mAsked, roots) == root(3));
        skmap::probeSent(r, root(3), roots, 0.2);
        skmap::probeReply(r, root(3), ASH_M, 0.3);
        skmap::askNextRoot(r, roots, candidates);
        ensure("found the ally", skmap::allyVerdict(r, candidates) == skmap::AllyVerdict::ALLY && !skmap::wantsProbe(r, 0.3, true));
        skmap::AllyRecord n;
        skmap::observe(n, "Soldier", { root(1) }, 0.0, false);
        skmap::probeSent(n, root(1), { root(1) }, 0.0);
        skmap::probeReply(n, root(1), OTHER, 0.1);
        skmap::askNextRoot(n, { root(1) }, candidates);
        ensure("nothing left, nothing asked", !skmap::wantsProbe(n, 0.1, true));
        skmap::observe(n, "Ashguard", { root(1) }, 1.0, false);
        ensure("a title change asks the same root afresh", skmap::wantsProbe(n, 1.0, true) && n.mFresh && n.mAsked.empty());
    }

    // The verdict: only a resolved candidate group is an ally, and a title change takes it away;
    // nothing yields anything but ally or unknown.
    template<> template<>
    void skmapallies_object::test<6>()
    {
        const std::set<LLUUID> candidates = { ASH_G, ASH_M, ASH_L };
        skmap::AllyRecord r;
        skmap::observe(r, "Soldier", { root(1) }, 0.0, false);
        skmap::probeSent(r, root(1), { root(1) }, 0.0);
        skmap::probeReply(r, root(1), OTHER, 0.1);
        ensure("a non-candidate is unknown", skmap::allyVerdict(r, candidates) == skmap::AllyVerdict::UNKNOWN);
        skmap::AllyRecord a;
        skmap::observe(a, "Soldier", { root(1) }, 0.0, false);
        skmap::probeSent(a, root(1), { root(1) }, 0.0);
        skmap::probeReply(a, root(1), ASH_M, 0.1);
        ensure("a candidate is an ally", skmap::allyVerdict(a, candidates) == skmap::AllyVerdict::ALLY);
        skmap::observe(a, "Other tag", { root(1) }, 1.0, false);
        ensure("a tag switch takes it away", skmap::allyVerdict(a, candidates) == skmap::AllyVerdict::UNKNOWN);
        for (S32 s = 0; s <= static_cast<S32>(ProbeState::NONE); ++s)
        {
            skmap::AllyRecord any;
            any.mState = static_cast<ProbeState>(s);
            any.mGroup = s % 2 ? ASH_G : OTHER;
            const skmap::AllyVerdict v = skmap::allyVerdict(any, candidates);
            ensure("ally or unknown", v == skmap::AllyVerdict::ALLY || v == skmap::AllyVerdict::UNKNOWN);
        }
    }

    // The budget: never past its rate or its in-flight cap, a 5 s timeout, 64 queued at most.
    template<> template<>
    void skmapallies_object::test<7>()
    {
        skmap::ProbeBudget budget;
        for (S32 i = 0; i < 100; ++i)
        {
            LLUUID id;
            id.mData[0] = static_cast<U8>(i);
            id.mData[1] = 0x55;
            const bool queued = budget.enqueue(id);
            ensure("queue capped at 64", queued == (i < 64));
        }
        // Replies come at once: only the rate limits.
        S32 sent = 0;
        for (F64 t = 0.0; t < 10.0; t += 0.01)
        {
            for (LLUUID id = budget.next(t); id.notNull(); id = budget.next(t))
            {
                budget.sent(id, t);
                budget.done(id);
                ++sent;
            }
        }
        ensure("about 4 a second", sent >= 40 && sent <= 44);

        // No replies: the in-flight cap holds, and they time out at 5 s.
        skmap::ProbeBudget stuck;
        for (S32 i = 0; i < 10; ++i)
        {
            LLUUID id;
            id.mData[0] = static_cast<U8>(i);
            id.mData[2] = 0x77;
            stuck.enqueue(id);
        }
        for (F64 t = 0.0; t < 4.9; t += 0.1)
        {
            for (LLUUID id = stuck.next(t); id.notNull(); id = stuck.next(t))
            {
                stuck.sent(id, t);
            }
            ensure("in flight capped", stuck.inFlight() <= 4);
        }
        ensure_equals("four in flight", stuck.inFlight(), static_cast<size_t>(4));
        ensure("not before 5 s", stuck.expire(4.9).empty());
        ensure_equals("all four at 5 s", stuck.expire(5.0).size(), static_cast<size_t>(4));
    }
}

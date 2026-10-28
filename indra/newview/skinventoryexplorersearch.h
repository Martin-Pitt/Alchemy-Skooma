/**
 * @file skinventoryexplorersearch.h
 * @brief Inventory Explorer's search: the query parser, matching and the time-sliced walk
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef SK_SKINVENTORYEXPLORERSEARCH_H
#define SK_SKINVENTORYEXPLORERSEARCH_H

#include "llframetimer.h"
#include "lluuid.h"

#include <ctime>
#include <functional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

class LLInventoryObject;

// One term of a query; terms are ANDed.
struct SKSearchTerm
{
    enum EKind
    {
        KIND_TEXT,      // a plain word: the name, else the description at a lower rank
        KIND_TYPE,
        KIND_BRAND,
        KIND_CREATOR,
        KIND_PERM,
        KIND_IS,
        KIND_AFTER,
        KIND_BEFORE,
    };

    EKind mKind = KIND_TEXT;
    std::string mSource;    // the term as typed, for the flag
    std::string mValue;     // lower case, quotes removed
    bool mFlagged = false;  // a value that names nothing: the term matches nothing
    LLUUID mCreator;
    time_t mTime = 0;
};

// A parsed query: type:, brand:, creator: (a UUID or a username), perm:, is: (and a bare "worn"), after:, before:;
// an unknown key:value is plain text.
class SKInventorySearchQuery
{
public:
    enum ERank
    {
        NO_MATCH = -1,
        RANK_NAME = 0,
        RANK_DESCRIPTION = 1,
    };

    SKInventorySearchQuery() = default;
    explicit SKInventorySearchQuery(const std::string& text);

    bool empty() const { return mTerms.empty(); }
    const std::vector<SKSearchTerm>& terms() const { return mTerms; }
    // The first term as typed whose value names nothing, empty when every term is sound.
    std::string flaggedTerm() const;

    // NO_MATCH, or how the object matched.
    S32 match(const LLInventoryObject* obj) const;

    // Splits a query into its terms as typed, quotes kept: the parser and the dropdown share it.
    static std::vector<std::pair<size_t, size_t>> termSpans(const std::string& text);
    // The kind of an object for type: ("folder", "object", "clothing", ...).
    static const char* typeKey(const LLInventoryObject* obj);
    // A day's local midnight from "YYYY-MM-DD", "Nd" or "today"; 0 when it is none of those.
    static time_t parseDay(const std::string& value);

    // A creator: name not in the name cache yet, which may resolve later.
    bool waitsForCreator() const;

private:
    std::vector<SKSearchTerm> mTerms;
};

// Walks a folder and everything below it a slice at a time, skipping the Trash unless the walk starts inside it.
// Folders not yet loaded start a recursive background fetch and are counted until they have loaded.
class SKInventoryWalk
{
public:
    typedef std::function<void(const LLInventoryObject* obj)> visit_t;

    void start(const LLUUID& root_id);
    void stop();
    // Walks a changed folder and what is below it again, if it lies under the root.
    void add(const LLUUID& folder_id);
    // Visits objects for up to the budget, resuming inside a folder where the last slice stopped; true once done.
    bool step(F64 seconds, const visit_t& visit);
    bool isDone() const { return mPending.empty() && mCurrent.isNull(); }
    bool isStarted() const { return mRoot.notNull(); }
    const LLUUID& getRoot() const { return mRoot; }
    // Under the root, and not in a Trash the walk skips.
    bool covers(const LLUUID& id) const;
    S32 getLoadingCount() const { return (S32)mLoading.size(); }

private:
    LLUUID mRoot;
    LLUUID mTrash;
    std::vector<LLUUID> mPending;
    LLUUID mCurrent;            // the folder a slice stopped inside
    size_t mCatIndex = 0;
    size_t mItemIndex = 0;
    std::set<LLUUID> mLoading;
};

// A query over a folder. Hits grow as the walk runs; inventory changes check the changed hits again and walk the
// changed folders, so the results follow the inventory without starting over.
class SKInventorySearch
{
public:
    typedef std::pair<LLUUID, S32> hit_t;

    void start(const LLUUID& folder_id, const std::string& text);
    void stop();
    bool isActive() const { return mActive; }
    void noteChanges(const std::set<LLUUID>& changed_ids);

    // Walks for a slice.
    void step();
    bool isDone() const { return mWalk.isDone(); }
    S32 getLoadingCount() const { return mWalk.getLoadingCount(); }
    const std::vector<hit_t>& hits() const { return mHits; }
    // Moves when hits leave or change rank, so the shown list must be rebuilt rather than added to.
    U32 getRemovals() const { return mRemovals; }
    // Moves when the status the search shows would change.
    U32 getStateGeneration() const { return mStateGeneration; }
    const SKInventorySearchQuery& query() const { return mQuery; }
    const std::string& text() const { return mText; }
    const LLUUID& folder() const { return mFolder; }

private:
    void startWalk();
    void removeHit(const LLUUID& id);
    void touchState();

    SKInventoryWalk mWalk;
    SKInventorySearchQuery mQuery;
    std::string mText;
    LLUUID mFolder;
    bool mActive = false;
    std::vector<hit_t> mHits;
    std::unordered_set<LLUUID> mHitIDs;
    U32 mRemovals = 0;
    U32 mStateGeneration = 0;
    bool mLastDone = true;
    S32 mLastLoading = 0;
    LLFrameTimer mCreatorRetry;
};

#endif // SK_SKINVENTORYEXPLORERSEARCH_H

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

#include "lluuid.h"

#include <ctime>
#include <functional>
#include <string>
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

private:
    std::vector<SKSearchTerm> mTerms;
};

// Walks a folder and everything below it a slice at a time, skipping the Trash unless the walk starts inside it.
// Folders not yet loaded start a recursive background fetch and are counted.
class SKInventoryWalk
{
public:
    typedef std::function<void(const LLInventoryObject* obj)> visit_t;

    void start(const LLUUID& root_id);
    void stop() { mPending.clear(); mRoot.setNull(); }
    // Visits objects for up to the budget; true once nothing is left.
    bool step(F64 seconds, const visit_t& visit);
    bool isDone() const { return mPending.empty(); }
    bool isStarted() const { return mRoot.notNull(); }
    const LLUUID& getRoot() const { return mRoot; }
    S32 getLoadingCount() const { return mLoading; }

private:
    LLUUID mRoot;
    LLUUID mTrash;
    std::vector<LLUUID> mPending;
    S32 mLoading = 0;
};

// A query over a folder, its hits gathered across frames.
class SKInventorySearch
{
public:
    typedef std::pair<LLUUID, S32> hit_t;

    void start(const LLUUID& folder_id, const std::string& text);
    void stop();
    // Walks again from the start, after the inventory changed.
    void restart();
    bool isActive() const { return mWalk.isStarted(); }

    // Walks for a slice; true when hits were added or the walk ended in it.
    bool step();
    bool isDone() const { return mWalk.isDone(); }
    S32 getLoadingCount() const { return mWalk.getLoadingCount(); }
    const std::vector<hit_t>& hits() const { return mHits; }
    const SKInventorySearchQuery& query() const { return mQuery; }
    const std::string& text() const { return mText; }
    const LLUUID& folder() const { return mFolder; }

private:
    SKInventoryWalk mWalk;
    SKInventorySearchQuery mQuery;
    std::string mText;
    LLUUID mFolder;
    std::vector<hit_t> mHits;
    std::vector<hit_t> mWalking;   // hits of a restarted walk, shown once it ends
    bool mRestarted = false;
};

#endif // SK_SKINVENTORYEXPLORERSEARCH_H

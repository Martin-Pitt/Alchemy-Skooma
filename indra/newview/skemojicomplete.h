/**
 * @file skemojicomplete.h
 * @brief An inline list completing a typed :shortcode: to its emoji, for chat inputs.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Alchemy Viewer Source Code
 * $/LicenseInfo$
 */

#pragma once

#include "llview.h"
#include "llemojidictionary.h"

#include <string>
#include <vector>

class LLTextEditor;

// Lies above its editor in the editor's parent; the host calls update() each frame.
class SKEmojiComplete : public LLView
{
public:
    // Adds the list to the editor's parent and hooks the editor's keys.
    static SKEmojiComplete* attach(LLTextEditor* editor);

    void update();
    bool isOpen() const { return getVisible(); }
    void dismiss();

    void draw() override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, LLScrollDelta delta) override;

private:
    explicit SKEmojiComplete(LLTextEditor* editor);

    bool handleEditorKey(KEY key, MASK mask);
    void commit(S32 index);
    void close();
    void select(S32 index);
    void layout();
    S32 rowAt(S32 y) const;
    S32 visibleRows() const;

    LLTextEditor* mEditor;       // a sibling under the same parent, so it lives as long as the list
    std::vector<LLEmojiSearchResult> mResults;
    std::string mQuery;          // the typed ":partial", colon included
    S32 mCodePos = -1;           // where the colon is in the editor's text
    S32 mDismissedPos = -1;      // a code Esc closed stays closed until its colon moves
    S32 mSelected = 0;
    S32 mFirst = 0;              // the first visible row
    std::string mLastText;
    S32 mLastCursor = -1;
};

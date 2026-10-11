/**
 * @file skfloateroutlinecapture.cpp
 * @brief Debug view of the outline capture: its atlas and per-capture stats.
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

#include "llviewerprecompiledheaders.h"

#include "skfloateroutlinecapture.h"

#include "llagent.h"
#include "llbutton.h"
#include "lldir.h"
#include "llrender2dutils.h"
#include "llselectmgr.h"
#include "lltextbox.h"
#include "llviewerobject.h"
#include "llvoavatarself.h"
#include "skoutlinecapture.h"

#include <sstream>

SKFloaterOutlineCapture::SKFloaterOutlineCapture(const LLSD& key)
    : LLFloater(key)
{
}

bool SKFloaterOutlineCapture::postBuild()
{
    mAtlasView = getChild<LLView>("atlas_view");
    mStats = getChild<LLTextBox>("stats");
    getChild<LLButton>("capture_selection")->setCommitCallback([this](LLUICtrl*, const LLSD&) { captureSelection(); });
    getChild<LLButton>("capture_seat")->setCommitCallback([this](LLUICtrl*, const LLSD&) { captureSeat(); });
    getChild<LLButton>("forget_all")->setCommitCallback([](LLUICtrl*, const LLSD&) { SKOutlineCapture::instance().forgetAll(); });
    getChild<LLButton>("dump_atlas")->setCommitCallback([this](LLUICtrl*, const LLSD&) { dumpAtlas(); });
    return true;
}

void SKFloaterOutlineCapture::captureSelection()
{
    if (LLViewerObject* obj = LLSelectMgr::getInstance()->getSelection()->getFirstRootObject(true))
    {
        LLViewerObject* root = obj->getRootEdit();
        if (root && !root->isAvatar() && !root->isAttachment())
        {
            SKOutlineCapture::instance().request(root->getID(), true);
        }
    }
}

void SKFloaterOutlineCapture::captureSeat()
{
    if (isAgentAvatarValid() && gAgentAvatarp->isSitting())
    {
        if (LLViewerObject* seat = dynamic_cast<LLViewerObject*>(gAgentAvatarp->getParent()))
        {
            SKOutlineCapture::instance().request(seat->getRootEdit()->getID(), true);
        }
    }
}

void SKFloaterOutlineCapture::dumpAtlas()
{
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, "sk_outline_atlas.png");
    const bool ok = SKOutlineCapture::instance().dumpAtlas(path);
    LL_INFOS("SKOutline") << (ok ? "wrote " : "could not write ") << path << LL_ENDL;
}

void SKFloaterOutlineCapture::refreshStats()
{
    const SKOutlineCapture& capture = SKOutlineCapture::instance();
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(3);
    out << "slots " << capture.slotsUsed() << "/" << capture.slotsTotal() << "   last frame " << capture.lastFrameRenders()
        << " renders, " << capture.lastFrameMs() << " ms\n";
    capture.forEachCapture(
        [&out](const SKOutlineCapture::Stats& s)
        {
            out << s.root.asString().substr(0, 8) << (s.pinned ? "*" : " ") << " " << s.parts << " parts, " << s.cols << "x"
                << s.rows << " @" << s.cell << " m, " << s.renders << " renders (" << s.last_reason << ") "
                << s.draws << " draws, cpu " << s.cpu_ms << " ms";
            if (s.gpu_ms >= 0.0)
            {
                out << ", gpu " << s.gpu_ms << " ms";
            }
            out.precision(1);
            out << ", " << s.last_render << " s ago, " << s.distance << " m" << (s.settled ? ", settled" : "")
                << (s.waiting ? ", loading" : "") << "\n";
            out.precision(3);
        });
    mStats->setText(out.str());
}

void SKFloaterOutlineCapture::draw()
{
    LLFloater::draw();
    if (isMinimized() || !SKOutlineCapture::instanceExists())
    {
        return;
    }
    if (mStatsTimer.getElapsedTimeF32() > 0.25f)
    {
        mStatsTimer.reset();
        refreshStats();
    }

    const LLRect r = mAtlasView->calcScreenRect();
    const LLRect own = calcScreenRect();
    const S32 side = llmin(r.getWidth(), r.getHeight());
    const S32 left = r.mLeft - own.mLeft;
    const S32 bottom = r.mBottom - own.mBottom;
    gl_rect_2d(left, bottom + side, left + side, bottom, LLColor4(0.06f, 0.06f, 0.08f, 1.f));
    if (const U32 tex = SKOutlineCapture::instance().atlasTexture())
    {
        gGL.getTextureSlot(0)->bindManual(ALTextureSlot::TT_TEXTURE, tex);
        gGL.color4f(1.f, 1.f, 1.f, 1.f);
        gGL.pushUIMatrix();
        gGL.translateUI((F32)left, (F32)bottom, 0.f);
        gl_rect_2d_simple_tex(side, side);
        gGL.popUIMatrix();
        gGL.getTextureSlot(0)->unbind();
    }
}

/**
 * @file skroadkithost.cpp
 * @brief Spike: the road model's kit questions (skroadmodel.h host::kitOf, kitLibraryPath) answered from the live
 * objects: a linkset's geometry signature as skroadkitsnaps.cpp reads it, without the root name.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "skroadkits.h"
#include "skroadmodel.h"

#include "llgltfmaterial.h"
#include "llprimitive.h"
#include "lltextureentry.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llvolume.h"

namespace
{
    bool kitPrim(LLViewerObject* o, bool root, skroad::KitPrim& out)
    {
        if (const LLSculptParams* sculpt = o->getSculptParams())
        {
            out.shape = skroad::kitSculptShape(sculpt->getSculptTexture().asString(), (S32)sculpt->getSculptType());
        }
        else if (const LLVolume* volume = o->getVolume())
        {
            const LLProfileParams& pr = volume->getParams().getProfileParams();
            const LLPathParams& pa = volume->getParams().getPathParams();
            const F64 vp[13] = { (F64)pr.getCurveType(), pr.getBegin(), pr.getEnd(), pr.getHollow(), (F64)pa.getCurveType(),
                                 pa.getBegin(), pa.getEnd(), pa.getScaleX(), pa.getScaleY(), pa.getShearX(), pa.getShearY(),
                                 pa.getTwistBegin(), pa.getTwist() };
            out.shape = skroad::kitVolumeShape(vp);
        }
        else
        {
            return false;       // its shape has not come yet
        }
        out.scale = o->getScale();
        out.pos = root ? LLVector3::zero : o->getPosition();
        out.rot = root ? LLQuaternion::DEFAULT : o->getRotation();
        out.root = root;
        return true;
    }
}

namespace skroad::host
{
    bool kitOf(const LLUUID& root, U64& hash, LLVector3d& pos, LLQuaternion& rot)
    {
        LLViewerObject* ro = gObjectList.findObject(root);
        if (!ro || !ro->isRoot())
        {
            return false;
        }
        std::vector<KitPrim> prims(1);
        if (!kitPrim(ro, true, prims[0]))
        {
            return false;
        }
        for (LLViewerObject* child : ro->getChildren())
        {
            if (child && !child->isAvatar())
            {
                prims.emplace_back();
                if (!kitPrim(child, false, prims.back()))
                {
                    return false;
                }
            }
        }
        hash = kitHash(kitGeoSignature(prims));
        pos = ro->getPositionGlobal();
        rot = ro->getRotationRegion();
        return true;
    }

    std::string kitLibraryPath()
    {
        static LLCachedControl<std::string> path(gSavedSettings, "SKRoadKitLibrary", "");
        return path;
    }

    std::string signLibraryPath()
    {
        static LLCachedControl<std::string> path(gSavedSettings, "SKRoadSignLibrary", "");
        return path;
    }

    bool signTextures(const LLUUID& root, std::vector<LLUUID>& out)
    {
        LLViewerObject* ro = gObjectList.findObject(root);
        if (!ro || ro->isDead())
        {
            return false;
        }
        auto faces = [&out](LLViewerObject* o)
        {
            for (U8 i = 0; i < o->getNumTEs(); ++i)
            {
                if (const LLTextureEntry* te = o->getTE(i))
                {
                    out.push_back(te->getID());
                    if (const LLGLTFMaterial* m = te->getGLTFRenderMaterial())
                    {
                        out.push_back(m->mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR]);
                    }
                }
            }
        };
        faces(ro);
        for (LLViewerObject* child : ro->getChildren())
        {
            if (child && !child->isAvatar())
            {
                faces(child);
            }
        }
        return true;
    }
}

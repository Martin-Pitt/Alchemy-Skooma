/**
 * @file skcaptureexport.cpp
 * @brief Writes a linkset, an avatar's attachments or an avatar as triangles with per-face visibility, for outline work offline.
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

#include "llviewerprecompiledheaders.h"

#include "skcaptureexport.h"

#include "alobjectproperties.h"
#include "llavatarnamecache.h"
#include "llcallingcard.h"
#include "lldate.h"
#include "lldir.h"
#include "llfile.h"
#include "llgltfmaterial.h"
#include "llinventorymodel.h"
#include "llmaterial.h"
#include "llselectmgr.h"
#include "lltextureentry.h"
#include "llviewerjointattachment.h"
#include "llviewertexture.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "llvovolume.h"

#include <iomanip>
#include <vector>

namespace skcapture
{
    // A PBR material's own mode wins over a legacy material's, and with
    // neither the face is blended only if its tint or texture carries alpha.
    const char* faceAlphaMode(const LLTextureEntry* te)
    {
        if (const LLGLTFMaterial* gltf = te->getGLTFRenderMaterial())
        {
            switch (gltf->mAlphaMode)
            {
                case LLGLTFMaterial::ALPHA_MODE_BLEND: return "pbr-blend";
                case LLGLTFMaterial::ALPHA_MODE_MASK: return "pbr-mask";
                default: return "pbr-opaque";
            }
        }
        const LLMaterialPtr material = te->getMaterialParams();
        if (material.notNull())
        {
            switch (material->getDiffuseAlphaMode())
            {
                case LLMaterial::DIFFUSE_ALPHA_MODE_NONE: return "none";
                case LLMaterial::DIFFUSE_ALPHA_MODE_BLEND: return "blend";
                case LLMaterial::DIFFUSE_ALPHA_MODE_MASK: return "mask";
                case LLMaterial::DIFFUSE_ALPHA_MODE_EMISSIVE: return "emissive";
                default: break;
            }
        }
        return "default";
    }

    F32 facePbrAlpha(const LLTextureEntry* te)
    {
        const LLGLTFMaterial* gltf = te->getGLTFRenderMaterial();
        return gltf ? gltf->mBaseColor.mV[VW] : 1.f;
    }

    S32 linkNumber(const LLViewerObject* object)
    {
        const LLViewerObject* parent = static_cast<const LLViewerObject*>(object->getParent());
        if (!parent)
        {
            return 1;
        }
        S32 link = 2;
        for (const LLPointer<LLViewerObject>& child : parent->getChildren())
        {
            if (child.get() == object)
            {
                break;
            }
            if (!child->isAvatar())
            {
                ++link;
            }
        }
        return link;
    }

    // Whether one face can be seen through: set to blend whatever its tint,
    // faint under blend, or the stock transparent texture.
    bool faceSeeThrough(const LLViewerObject* object, U8 f)
    {
        const LLTextureEntry* te = object->getTE(f);
        if (!te)
        {
            return false;
        }
        F32 alpha = te->getColor().mV[VALPHA];
        bool blended = true;
        bool blend_mode = false;
        if (const LLGLTFMaterial* gltf = te->getGLTFRenderMaterial())
        {
            alpha *= gltf->mBaseColor.mV[VW];
            blended = gltf->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_BLEND;
            blend_mode = blended;
        }
        else if (const LLMaterialPtr material = te->getMaterialParams(); material.notNull())
        {
            // A legacy material defaults to blend, so an opaque door with a
            // normal map reads as blended; only a texture with alpha is glass.
            const LLViewerTexture* image = object->getTEImage(f);
            blend_mode = material->getDiffuseAlphaMode() == LLMaterial::DIFFUSE_ALPHA_MODE_BLEND && image &&
                         image->getComponents() == 4;
        }
        return te->getID() == IMG_TRANSPARENT || blend_mode || (blended && alpha < 0.95f);
    }

    F32 seeThroughShare(const LLViewerObject* object)
    {
        const U8 faces = object->getNumTEs();
        if (faces == 0)
        {
            return 0.f;
        }
        U32 clear = 0;
        for (U8 f = 0; f < faces; ++f)
        {
            clear += faceSeeThrough(object, f) ? 1 : 0;
        }
        return static_cast<F32>(clear) / faces;
    }
}

namespace
{
    using namespace skcapture;

    const char* kindName(SKCaptureKind kind)
    {
        switch (kind)
        {
            case SKCaptureKind::VEHICLE: return "vehicle";
            case SKCaptureKind::ATTACHMENTS: return "attachments";
            default: return "avatar";
        }
    }

    LLViewerObject* rootOf(LLViewerObject* object)
    {
        while (object && !object->isRoot() && object->getParent())
        {
            object = static_cast<LLViewerObject*>(object->getParent());
        }
        return object;
    }

    // The selection first, so a target can be picked in the edit tools; else
    // whatever the screen centre is on, as the mark casts.
    LLViewerObject* pickTarget()
    {
        LLViewerObject* target = LLSelectMgr::getInstance()->getSelection()->getFirstRootObject(true);
        if (!target && gViewerWindow)
        {
            const LLRect& view = gViewerWindow->getWorldViewRectScaled();
            target = gViewerWindow->cursorIntersect(view.getCenterX(), view.getCenterY(), 1024.f, nullptr, -1, true,
                                                    false, true, true);
        }
        return target;
    }

    // A frame to write coordinates in: a point and a rotation, both in agent space.
    struct Frame
    {
        LLVector3 mOrigin;
        LLQuaternion mRotation;
        LLQuaternion mInverse;

        Frame(const LLVector3& origin, const LLQuaternion& rotation)
            : mOrigin(origin), mRotation(rotation), mInverse(~rotation)
        {
        }
        LLVector3 point(const LLVector3& agent) const { return (agent - mOrigin) * mInverse; }
        LLQuaternion rotation(const LLQuaternion& agent) const { return agent * mInverse; }
    };

    class CaptureWriter
    {
    public:
        explicit CaptureWriter(llofstream& file) : mFile(file) {}

        U32 mParts = 0;
        U32 mTriangles = 0;
        U32 mSources = 0;

        void vec3(const LLVector3& v) { mFile << '[' << v.mV[VX] << ',' << v.mV[VY] << ',' << v.mV[VZ] << ']'; }

        void quat(const LLQuaternion& q)
        {
            mFile << '[' << q.mQ[VX] << ',' << q.mQ[VY] << ',' << q.mQ[VZ] << ',' << q.mQ[VW] << ']';
        }

        void text(const std::string& s)
        {
            mFile << '"';
            for (char c : s)
            {
                if (c == '"' || c == '\\')
                {
                    mFile << '\\' << c;
                }
                else if (c == '\n')
                {
                    mFile << "\\n";
                }
                else if (static_cast<unsigned char>(c) >= 0x20)
                {
                    mFile << c;
                }
            }
            mFile << '"';
        }

        void frame(const char* name, const Frame& f)
        {
            mFile << "{\"name\":\"" << name << "\",\"origin\":";
            vec3(f.mOrigin);
            mFile << ",\"rotation\":";
            quat(f.mRotation);
            mFile << '}';
        }

        // One prim: its load state and, per face, how it blends and its
        // triangles in the given frame. A rigged mesh follows a skeleton, so
        // its rest positions would be wrong; it is listed without triangles.
        void part(LLViewerObject* object, S32 link, const Frame& f)
        {
            ++mParts;
            mFile << "{\"id\":\"" << object->getID() << "\",\"link\":" << link;
            LLVOVolume* volume = dynamic_cast<LLVOVolume*>(object);
            const LLVolume* v = volume ? volume->getVolume() : nullptr;
            const bool rigged = object->isRiggedMesh();
            mFile << ",\"rigged\":" << (rigged ? "true" : "false") << ",\"lod\":" << object->getLOD()
                  << ",\"mesh\":" << (volume && volume->isMesh() ? "true" : "false")
                  << ",\"loaded\":" << (v && v->getNumVolumeFaces() > 0 ? "true" : "false") << ",\"scale\":";
            vec3(object->getScale());
            mFile << ",\"faces\":[";
            const S32 faces = v ? v->getNumVolumeFaces() : 0;
            const LLVector3 scale = object->getScale();
            const LLVector3 position = object->getRenderPosition();
            const LLQuaternion object_rotation = object->getRenderRotation();
            for (S32 i = 0; i < faces; ++i)
            {
                if (i)
                {
                    mFile << ',';
                }
                const LLTextureEntry* te = object->getTE(static_cast<U8>(i));
                mFile << "{\"te\":" << i;
                if (te)
                {
                    mFile << ",\"alpha\":" << te->getColor().mV[VALPHA] << ",\"pbrAlpha\":" << facePbrAlpha(te)
                          << ",\"mode\":\"" << faceAlphaMode(te) << "\",\"texture\":\"" << te->getID()
                          << "\",\"fullbright\":" << (te->getFullbright() ? "true" : "false")
                          << ",\"seeThrough\":" << (faceSeeThrough(object, static_cast<U8>(i)) ? "true" : "false");
                }
                mFile << ",\"tris\":[";
                if (!rigged)
                {
                    const LLVolumeFace& face = v->getVolumeFace(i);
                    for (S32 n = 0; n < face.mNumIndices; ++n)
                    {
                        const LLVector4a& p = face.mPositions[face.mIndices[n]];
                        LLVector3 world(p.getF32ptr());
                        world.scaleVec(scale);
                        const LLVector3 local = f.point(world * object_rotation + position);
                        if (n)
                        {
                            mFile << ',';
                        }
                        mFile << local.mV[VX] << ',' << local.mV[VY] << ',' << local.mV[VZ];
                    }
                    mTriangles += face.mNumIndices / 3;
                }
                mFile << "]}";
            }
            mFile << "]}";
        }

        // One linkset as a source: its root's pose in the pose frame, the
        // share of its faces that show, and its prims in link order in the
        // triangle frame. The pose frame and the triangle frame differ only
        // for worn linksets captured each in their own frame.
        void source(LLViewerObject* root, const char* role, const Frame& pose, const Frame& tris,
                    const std::string& extra)
        {
            if (mSources++)
            {
                mFile << ',';
            }
            std::vector<LLViewerObject*> prims { root };
            for (const LLPointer<LLViewerObject>& child : root->getChildren())
            {
                if (!child->isAvatar())
                {
                    prims.push_back(child.get());
                }
            }
            U32 faces = 0, clear = 0;
            bool rigged = false;
            for (LLViewerObject* prim : prims)
            {
                const U8 n = prim->getNumTEs();
                faces += n;
                for (U8 f = 0; f < n; ++f)
                {
                    clear += faceSeeThrough(prim, f) ? 1 : 0;
                }
                rigged |= prim->isRiggedMesh();
            }

            mFile << "{\"role\":\"" << role << "\",\"id\":\"" << root->getID() << "\",\"name\":";
            text(sourceName(root));
            if (root->isAttachment())
            {
                mFile << ",\"attachPoint\":" << ATTACHMENT_ID_FROM_STATE(root->getAttachmentState()) << ",\"item\":\""
                      << root->getAttachmentItemID() << '"';
            }
            mFile << extra << ",\"rootPosition\":";
            vec3(pose.point(root->getRenderPosition()));
            mFile << ",\"rootRotation\":";
            quat(pose.rotation(root->getRenderRotation()));
            mFile << ",\"visibleShare\":" << (faces ? 1.f - static_cast<F32>(clear) / faces : 0.f)
                  << ",\"rigged\":" << (rigged ? "true" : "false") << ",\"parts\":[";
            for (size_t i = 0; i < prims.size(); ++i)
            {
                if (i)
                {
                    mFile << ',';
                }
                part(prims[i], i ? linkNumber(prims[i]) : 1, tris);
            }
            mFile << "]}";
        }

    private:
        // Your own attachments are named by the inventory item; anyone
        // else's only by what the property cache already holds.
        static std::string sourceName(LLViewerObject* root)
        {
            if (root->isAttachment() && root->getAvatar() && root->getAvatar()->isSelf())
            {
                if (const LLViewerInventoryItem* item = gInventory.getItem(root->getAttachmentItemID()))
                {
                    return item->getName();
                }
            }
            if (const ALObjectPropertiesCache::ServerProps* props = ALObjectPropertiesCache::instance().get(root->getID()))
            {
                return props->mName;
            }
            return std::string();
        }

        llofstream& mFile;
    };

    // Every linkset an avatar wears, in attachment point order.
    std::vector<LLViewerObject*> wornLinksets(LLVOAvatar* avatar)
    {
        std::vector<LLViewerObject*> worn;
        for (const auto& point : avatar->mAttachmentPoints)
        {
            // A HUD is drawn on the screen, not the avatar; its position is in HUD space.
            if (point.second->getIsHUDAttachment())
            {
                continue;
            }
            for (const LLPointer<LLViewerObject>& attached : point.second->mAttachedObjects)
            {
                if (attached.notNull())
                {
                    worn.push_back(attached.get());
                }
            }
        }
        return worn;
    }

    // The avatar under the mark or selected, else yourself.
    LLVOAvatar* targetAvatar()
    {
        LLViewerObject* target = pickTarget();
        if (target && target->isAttachment())
        {
            return target->getAvatar();
        }
        if (target && target->isAvatar())
        {
            return static_cast<LLVOAvatar*>(target);
        }
        return gAgentAvatarp.get();
    }

    // The linkset to treat as a vehicle: pointing at a pilot or what they
    // wear means the thing they sit on.
    LLViewerObject* targetVehicle()
    {
        LLViewerObject* target = pickTarget();
        if (target && target->isAttachment())
        {
            target = target->getAvatar();
        }
        target = rootOf(target);
        if (target && target->isAvatar() && target->getParent())
        {
            target = rootOf(static_cast<LLViewerObject*>(target->getParent()));
        }
        return target && !target->isAvatar() ? target : nullptr;
    }
}

std::string skCaptureExport(SKCaptureKind kind)
{
    LLViewerObject* root = nullptr;
    LLVOAvatar* avatar = nullptr;
    if (kind == SKCaptureKind::VEHICLE)
    {
        root = targetVehicle();
        if (!root)
        {
            return "select a vehicle, or point the camera at one; an avatar must be sitting on something";
        }
    }
    else
    {
        avatar = targetAvatar();
        if (!avatar || avatar->isDead())
        {
            return "no avatar to capture";
        }
    }

    const LLUUID& id = root ? root->getID() : avatar->getID();
    const std::string name = llformat("sknav-capture-%s-%s.json", kindName(kind), id.asString().c_str());
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, name);
    llofstream file(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        return llformat("could not write %s", path.c_str());
    }
    file << std::setprecision(6);
    CaptureWriter out(file);

    file << "{\"format\":\"sknav-capture\",\"version\":1,\"kind\":\"" << kindName(kind) << "\",\"exported\":\""
         << LLDate::now().asString() << "\",\"target\":\"" << id << '"';

    if (root)
    {
        // Everything in the root's own frame, so a vehicle caught banked,
        // pitched or upside down still reads as its plan view down -z.
        const Frame f(root->getRenderPosition(), root->getRenderRotation());
        file << ",\"frame\":";
        out.frame("root", f);

        // The heading is the root's forward axis flattened onto the ground;
        // the map turns the outline by that alone.
        const LLVector3 forward = LLVector3::x_axis * f.mRotation;
        file << ",\"vehicle\":{\"hover\":";
        out.text(root->mHudText);
        if (const ALObjectPropertiesCache::ServerProps* props = ALObjectPropertiesCache::instance().get(root->getID()))
        {
            file << ",\"description\":";
            out.text(props->mDescription);
        }
        const LLVector3d global = root->getPositionGlobal();
        file << ",\"position\":[" << std::setprecision(10) << global.mdV[VX] << ',' << global.mdV[VY] << ','
             << global.mdV[VZ] << ']' << std::setprecision(6) << ",\"up\":";
        out.vec3(LLVector3::z_axis * f.mRotation);
        file << ",\"heading\":" << std::atan2(forward.mV[VY], forward.mV[VX]) * RAD_TO_DEG << ",\"velocity\":";
        out.vec3(root->getVelocity());
        file << ",\"physical\":" << (root->flagUsePhysics() ? "true" : "false")
             << ",\"scripted\":" << (root->flagScripted() ? "true" : "false")
             << ",\"phantom\":" << (root->flagPhantom() ? "true" : "false") << '}';

        // Everyone seated anywhere on the linkset: a turret or the whole
        // visible aircraft can be an attachment.
        std::vector<std::pair<LLVOAvatar*, S32>> crew;
        std::vector<LLViewerObject*> prims { root };
        for (const LLPointer<LLViewerObject>& child : root->getChildren())
        {
            prims.push_back(child.get());
        }
        for (LLViewerObject* prim : prims)
        {
            for (const LLPointer<LLViewerObject>& child : prim->getChildren())
            {
                if (child->isAvatar())
                {
                    crew.emplace_back(static_cast<LLVOAvatar*>(child.get()), linkNumber(prim));
                }
            }
        }
        file << ",\"crew\":[";
        for (size_t i = 0; i < crew.size(); ++i)
        {
            LLVOAvatar* member = crew[i].first;
            LLAvatarName av_name;
            const bool named = LLAvatarNameCache::get(member->getID(), &av_name);
            file << (i ? "," : "") << "{\"id\":\"" << member->getID() << "\",\"name\":";
            out.text(named ? av_name.getCompleteName() : std::string());
            file << ",\"self\":" << (member->isSelf() ? "true" : "false")
                 << ",\"friend\":" << (LLAvatarTracker::instance().isBuddy(member->getID()) ? "true" : "false");
            LLNameValue* title = member->getNVPair("Title");
            file << ",\"title\":";
            out.text(title && title->getString() ? title->getString() : "");
            file << ",\"seatLink\":" << crew[i].second << ",\"position\":";
            out.vec3(f.point(member->getRenderPosition()));
            file << '}';
        }
        file << "],\"sources\":[";
        out.source(root, "vehicle", f, f, std::string());
        for (const auto& member : crew)
        {
            const std::string wearer = llformat(",\"avatar\":\"%s\"", member.first->getID().asString().c_str());
            for (LLViewerObject* worn : wornLinksets(member.first))
            {
                out.source(worn, "crew-attachment", f, f, wearer);
            }
        }
        file << ']';
    }
    else
    {
        // The body frame: the pelvis and the avatar's rotation, which is how
        // the avatar faces whatever the animation does to its limbs.
        const LLVector3 pelvis = avatar->mPelvisp ? avatar->mPelvisp->getWorldPosition() : avatar->getRenderPosition();
        const Frame body(pelvis, avatar->getRenderRotation());
        LLAvatarName av_name;
        const bool named = LLAvatarNameCache::get(avatar->getID(), &av_name);
        file << ",\"avatar\":{\"id\":\"" << avatar->getID() << "\",\"name\":";
        out.text(named ? av_name.getCompleteName() : std::string());
        file << ",\"self\":" << (avatar->isSelf() ? "true" : "false") << '}';

        // Attachments each in their own root frame, so a weapon's outline
        // does not depend on how it is held; the pose is still given in the
        // body frame so a holster can be told from a hand.
        file << ",\"frame\":";
        out.frame(kind == SKCaptureKind::ATTACHMENTS ? "attachment-root" : "body", body);
        file << ",\"sources\":[";
        for (LLViewerObject* worn : wornLinksets(avatar))
        {
            const Frame own(worn->getRenderPosition(), worn->getRenderRotation());
            out.source(worn, "attachment", body, kind == SKCaptureKind::ATTACHMENTS ? own : body, std::string());
        }
        file << ']';
    }
    file << "}\n";

    const std::streamoff bytes = file.tellp();
    file.close();

    LL_INFOS("SKNavExport") << "wrote " << bytes << " bytes to " << path << LL_ENDL;
    return llformat("%u sources, %u parts, %u triangles (%.1f MB) -> %s", out.mSources, out.mParts, out.mTriangles,
                    static_cast<F64>(bytes) / (1024.0 * 1024.0), path.c_str());
}

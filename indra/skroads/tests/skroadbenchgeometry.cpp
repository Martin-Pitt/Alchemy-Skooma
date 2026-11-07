/**
 * @file skroadbenchgeometry.cpp
 * @brief Spike: a road part's geometry rebuilt from what the road survey recorded of it.
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

#include "linden_common.h"

#include "skroadbenchgeometry.h"

#include "llquaternion.h"
#include "llsdserialize.h"
#include "llvolume.h"
#include "llvolumemgr.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

namespace skroadbench
{
    namespace
    {
        // llmeshrepository.cpp's cache layout: a preamble (version, header size, which blocks are present), then the
        // asset as fetched, the header and its blocks at their offsets past it.
        constexpr S32 PREAMBLE = 12;
        constexpr S32 NUM_LODS = 4;
        constexpr U32 FLAG_PHYSCONVEX = 1 << (NUM_LODS + 1);
        constexpr U32 FLAG_PHYSMESH = 1 << (NUM_LODS + 2);
        const char* const LOD_NAMES[NUM_LODS] = { "lowest_lod", "low_lod", "medium_lod", "high_lod" };

        // skphysicsshape.cpp's physicsDetail: the census's prim tessellation, from the size alone.
        S32 physicsDetail(const LLVector3& scale)
        {
            const F32 avg = (scale[0] + scale[1] + scale[2]) / 3.f;
            return 1 + (avg > 5.f ? 1 : 0) + (avg > 25.f ? 1 : 0);
        }

        struct Frame
        {
            LLVector3 pos, scale;
            LLQuaternion rot;
            LLVector3 place(const LLVector3& v) const
            {
                return LLVector3(v.mV[0] * scale.mV[0], v.mV[1] * scale.mV[1], v.mV[2] * scale.mV[2]) * rot + pos;
            }
        };

        LLVector3 vec3(const LLSD& a)
        {
            return LLVector3(F32(a[0].asReal()), F32(a[1].asReal()), F32(a[2].asReal()));
        }

        void takeFaces(const LLVolume& volume, const Frame& f, std::vector<RebuiltFace>& out)
        {
            out.resize(volume.getNumVolumeFaces());
            for (S32 i = 0; i < volume.getNumVolumeFaces(); ++i)
            {
                const LLVolumeFace& face = volume.getVolumeFace(i);
                RebuiltFace& r = out[i];
                if (!face.mPositions || !face.mIndices)
                {
                    continue;
                }
                r.positions.reserve(face.mNumVertices);
                for (S32 v = 0; v < face.mNumVertices; ++v)
                {
                    r.positions.push_back(f.place(LLVector3(face.mPositions[v].getF32ptr())));
                    r.uvs.push_back(face.mTexCoords ? face.mTexCoords[v] : LLVector2());
                }
                r.indices.assign(face.mIndices, face.mIndices + face.mNumIndices);
            }
        }

        void takeTriangles(const LLVolume& volume, const Frame& f, std::vector<LLVector3>& out)
        {
            for (S32 i = 0; i < volume.getNumVolumeFaces(); ++i)
            {
                const LLVolumeFace& face = volume.getVolumeFace(i);
                if (!face.mPositions || !face.mIndices)
                {
                    continue;
                }
                for (S32 k = 0; k + 2 < face.mNumIndices; ++k)
                {
                    out.push_back(f.place(LLVector3(face.mPositions[face.mIndices[k]].getF32ptr())));
                }
                out.resize(out.size() / 3 * 3);
            }
        }

        // The volume params from the survey's "vp" array (skroadsurvey.cpp writeReference).
        LLVolumeParams primParams(const LLSD& vp)
        {
            LLSD sd;
            sd["profile"]["curve"] = vp[0];
            sd["profile"]["begin"] = vp[1];
            sd["profile"]["end"] = vp[2];
            sd["profile"]["hollow"] = vp[3];
            LLSD& path = sd["path"];
            path["curve"] = vp[4];
            path["begin"] = vp[5];
            path["end"] = vp[6];
            path["scale_x"] = vp[7];
            path["scale_y"] = vp[8];
            path["shear_x"] = vp[9];
            path["shear_y"] = vp[10];
            path["twist_begin"] = vp[11];
            path["twist"] = vp[12];
            path["radius_offset"] = vp[13];
            path["taper_x"] = vp[14];
            path["taper_y"] = vp[15];
            path["revolutions"] = vp[16];
            path["skew"] = vp[17];
            sd["sculpt"]["type"] = 0;
            LLVolumeParams params;
            params.fromLLSD(sd);
            return params;
        }

        struct MeshFile
        {
            std::vector<U8> bytes;
            LLSD header;
            U32 flags = 0;
            S32 header_size = 0;

            bool read(const std::string& path)
            {
                std::ifstream in(path, std::ios::binary);
                if (!in)
                {
                    return false;
                }
                bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                if (bytes.size() < size_t(PREAMBLE))
                {
                    return false;
                }
                U32 pre[3];
                std::memcpy(pre, bytes.data(), sizeof(pre));
                header_size = S32(pre[1]);
                flags = pre[2];
                if (header_size <= 0 || size_t(PREAMBLE + header_size) > bytes.size())
                {
                    return false;
                }
                const char* data = reinterpret_cast<const char*>(bytes.data()) + PREAMBLE;
                S32 size = header_size;
                // an old asset's "<? LLSD/Binary ?>" line before the header (llmeshrepository's strip_deprecated_header)
                if (size > 2 && data[0] == '<' && data[1] == '?')
                {
                    const char* nl = static_cast<const char*>(std::memchr(data, '\n', size));
                    if (!nl)
                    {
                        return false;
                    }
                    size -= S32(nl + 1 - data);
                    data = nl + 1;
                }
                std::istringstream stream(std::string(data, size));
                return LLSDSerialize::fromBinary(header, stream, size) > 0 && header.isMap();
            }

            // A block's bytes, or null where the header has none or the cache does not hold it.
            const U8* block(const char* name, U32 flag, S32& size) const
            {
                const LLSD& b = header[name];
                if (!b.has("offset") || !b.has("size") || !(flags & flag))
                {
                    return nullptr;
                }
                const S64 at = S64(PREAMBLE) + header_size + b["offset"].asInteger();
                size = b["size"].asInteger();
                if (size <= 0 || at < 0 || at + size > S64(bytes.size()))
                {
                    return nullptr;
                }
                return bytes.data() + at;
            }
        };

        std::vector<LLVector3> unpackPoints(const LLSD& decomp, const LLSD::Binary& blob)
        {
            LLVector3 min(-0.5f, -0.5f, -0.5f), max(0.5f, 0.5f, 0.5f);
            if (decomp.has("Min"))
            {
                min.setValue(decomp["Min"]);
                max.setValue(decomp["Max"]);
            }
            const LLVector3 range = max - min;
            std::vector<LLVector3> out;
            for (size_t i = 0; i + 6 <= blob.size(); i += 6)
            {
                U16 p[3];
                std::memcpy(p, blob.data() + i, 6);
                out.emplace_back(p[0] / 65535.f * range.mV[0] + min.mV[0], p[1] / 65535.f * range.mV[1] + min.mV[1],
                                 p[2] / 65535.f * range.mV[2] + min.mV[2]);
            }
            return out;
        }

        bool rebuildMesh(const LLSD& r, const Frame& f, const std::string& cache_dir, Rebuilt& out)
        {
            const LLUUID id = r["mesh"].asUUID();
            const std::string s = id.asString();
            const std::string path = cache_dir + "/" + s[0] + "/" + s[1] + "/sl_cache_" + s + "_0.asset";
            MeshFile file;
            if (!file.read(path))
            {
                out.why = "mesh not in the cache: " + path;
                return false;
            }
            LLVolumeParams params;
            params.setType(LL_PCODE_PROFILE_SQUARE, LL_PCODE_PATH_LINE);
            params.setSculptID(id, LL_SCULPT_TYPE_MESH);

            // what is drawn: the top LOD the cache holds
            for (S32 lod = NUM_LODS - 1; lod >= 0; --lod)
            {
                S32 size = 0;
                const U8* data = file.block(LOD_NAMES[lod], 1u << lod, size);
                if (!data)
                {
                    continue;
                }
                LLPointer<LLVolume> volume = new LLVolume(params, LLVolumeLODGroup::getVolumeScaleFromDetail(lod));
                if (volume->unpackVolumeFaces(const_cast<U8*>(data), size))
                {
                    takeFaces(*volume, f, out.faces);
                    if (lod != NUM_LODS - 1)
                    {
                        out.why = std::string("drawn at ") + LOD_NAMES[lod] + ", the top LOD is not cached";
                    }
                    break;
                }
            }

            // what the census collides with (skphysicsshape.cpp's USER_MESH and USER_CONVEX cases)
            const S32 phys_type = r["phys_type"].asInteger();
            const bool too_small = f.scale.mV[0] < 0.5f || f.scale.mV[1] < 0.5f || f.scale.mV[2] < 0.5f;
            if (phys_type == 0 && !too_small && file.header["physics_mesh"]["size"].asInteger() > 0)
            {
                S32 size = 0;
                if (const U8* data = file.block("physics_mesh", FLAG_PHYSMESH, size))
                {
                    LLPointer<LLVolume> volume = new LLVolume(params, 0.f);
                    if (volume->unpackVolumeFaces(const_cast<U8*>(data), size))
                    {
                        takeTriangles(*volume, f, out.physics);
                        out.provenance = skworld::Provenance::EXACT;
                        out.solid = false;
                    }
                }
                if (out.physics.empty())
                {
                    out.why += out.why.empty() ? "" : "; ";
                    out.why += "physics mesh not cached";
                }
            }
            else if (phys_type == 0 || phys_type == 2)
            {
                S32 size = 0;
                LLSD decomp;
                if (const U8* data = file.block("physics_convex", FLAG_PHYSCONVEX, size);
                    data && LLUZipHelper::unzip_llsd(decomp, data, size) == LLUZipHelper::ZR_OK)
                {
                    if (phys_type == 0 && decomp.has("HullList") && decomp.has("Positions"))
                    {
                        const LLSD::Binary& hulls = decomp["HullList"].asBinary();
                        const std::vector<LLVector3> points = unpackPoints(decomp, decomp["Positions"].asBinary());
                        size_t at = 0;
                        for (U8 n : hulls)
                        {
                            const size_t count = n ? n : 256;
                            if (at + count > points.size())
                            {
                                break;
                            }
                            std::vector<LLVector3> hull(points.begin() + at, points.begin() + at + count), tris;
                            at += count;
                            convexHull(hull, tris);
                            for (LLVector3& v : tris)
                            {
                                out.physics.push_back(f.place(v));
                            }
                            out.convex_tris.push_back(S32(tris.size() / 3));
                        }
                        out.provenance = skworld::Provenance::HULL;
                    }
                    else if (decomp.has("BoundingVerts"))
                    {
                        std::vector<LLVector3> tris;
                        convexHull(unpackPoints(decomp, decomp["BoundingVerts"].asBinary()), tris);
                        for (LLVector3& v : tris)
                        {
                            out.physics.push_back(f.place(v));
                        }
                        out.provenance = phys_type == 2 ? skworld::Provenance::HULL : skworld::Provenance::BASE_HULL;
                    }
                }
                if (out.physics.empty())
                {
                    out.why += out.why.empty() ? "" : "; ";
                    out.why += "physics hulls not cached";
                }
            }
            return !out.faces.empty() || !out.physics.empty();
        }

        bool rebuildPrim(const LLSD& r, const Frame& f, Rebuilt& out)
        {
            const LLVolumeParams params = primParams(r["vp"]);
            const S32 top = LLVolumeLODGroup::NUM_LODS - 1;
            LLPointer<LLVolume> drawn = new LLVolume(params, LLVolumeLODGroup::getVolumeScaleFromDetail(top));
            takeFaces(*drawn, f, out.faces);
            // the census takes a prim's faces at its physics detail (PRIM_MESH and PRIM_CONVEX alike)
            LLPointer<LLVolume> phys = new LLVolume(params, LLVolumeLODGroup::getVolumeScaleFromDetail(physicsDetail(f.scale)));
            takeTriangles(*phys, f, out.physics);
            out.provenance = skworld::Provenance::EXACT;
            out.solid = true;
            return !out.faces.empty() || !out.physics.empty();
        }
    }

    LLVolumeParams volumeParams(const LLSD& vp)
    {
        return primParams(vp);
    }

    std::string defaultCacheDir()
    {
        if (const char* dir = std::getenv("SK_ROADBENCH_CACHE"))
        {
            return dir;
        }
        const char* local = std::getenv("LOCALAPPDATA");
        return std::string(local ? local : ".") + "\\AlchemyNext\\cache";
    }

    bool rebuildPart(const LLSD& r, const std::string& cache_dir, Rebuilt& out)
    {
        out = Rebuilt();
        if (!r.has("pos") || !r.has("rot") || !r.has("scale"))
        {
            out.why = "no frame";
            return false;
        }
        Frame f;
        f.pos = vec3(r["pos"]);
        f.scale = vec3(r["scale"]);
        const LLSD& q = r["rot"];
        f.rot = LLQuaternion(F32(q[0].asReal()), F32(q[1].asReal()), F32(q[2].asReal()), F32(q[3].asReal()));
        if (r.has("mesh"))
        {
            return rebuildMesh(r, f, cache_dir, out);
        }
        if (r.has("vp") && r["vp"].size() >= 18)
        {
            return rebuildPrim(r, f, out);
        }
        out.why = "neither a mesh nor volume params";
        return false;
    }

    void convexHull(const std::vector<LLVector3>& p, std::vector<LLVector3>& out)
    {
        out.clear();
        const size_t n = p.size();
        if (n < 4)
        {
            return;
        }
        LLVector3 lo = p[0], hi = p[0];
        for (const LLVector3& v : p)
        {
            lo.set(llmin(lo.mV[0], v.mV[0]), llmin(lo.mV[1], v.mV[1]), llmin(lo.mV[2], v.mV[2]));
            hi.set(llmax(hi.mV[0], v.mV[0]), llmax(hi.mV[1], v.mV[1]), llmax(hi.mV[2], v.mV[2]));
        }
        const F32 eps = 1e-5f * llmax((hi - lo).magVec(), 1e-3f);

        // a first tetrahedron: two far apart points, the farthest from their line, the farthest from their plane
        size_t a = 0, b = 0;
        for (size_t i = 1; i < n; ++i)
        {
            if (p[i].mV[0] < p[a].mV[0]) a = i;
            if (p[i].mV[0] > p[b].mV[0]) b = i;
        }
        if ((p[b] - p[a]).magVec() < eps)
        {
            for (size_t i = 0; i < n; ++i)
            {
                if ((p[i] - p[a]).magVec() > (p[b] - p[a]).magVec()) b = i;
            }
        }
        const LLVector3 ab = p[b] - p[a];
        size_t c = a;
        F32 best = 0.f;
        for (size_t i = 0; i < n; ++i)
        {
            const F32 d = (ab % (p[i] - p[a])).magVec();
            if (d > best) { best = d; c = i; }
        }
        if (best < eps * ab.magVec())
        {
            return;
        }
        LLVector3 nrm = ab % (p[c] - p[a]);
        nrm.normVec();
        size_t d = a;
        best = 0.f;
        for (size_t i = 0; i < n; ++i)
        {
            const F32 h = std::fabs(nrm * (p[i] - p[a]));
            if (h > best) { best = h; d = i; }
        }
        if (best < eps)
        {
            return;     // flat: no body
        }

        struct Face
        {
            size_t v[3];
            LLVector3 n;
            F32 off = 0.f;
            bool alive = true;
        };
        std::vector<Face> faces;
        const LLVector3 inside = (p[a] + p[b] + p[c] + p[d]) * 0.25f;
        auto add = [&](size_t i, size_t j, size_t k)
        {
            Face f;
            f.v[0] = i; f.v[1] = j; f.v[2] = k;
            f.n = (p[j] - p[i]) % (p[k] - p[i]);
            f.n.normVec();
            f.off = f.n * p[i];
            if (f.n * inside - f.off > 0.f)     // wound inward: turn it
            {
                std::swap(f.v[1], f.v[2]);
                f.n = -f.n;
                f.off = -f.off;
            }
            faces.push_back(f);
        };
        add(a, b, c);
        add(a, b, d);
        add(a, c, d);
        add(b, c, d);

        std::map<std::pair<size_t, size_t>, int> edges;
        for (size_t q = 0; q < n; ++q)
        {
            if (q == a || q == b || q == c || q == d)
            {
                continue;
            }
            edges.clear();
            bool seen = false;
            for (Face& f : faces)
            {
                if (f.alive && f.n * p[q] - f.off > eps)
                {
                    f.alive = false;
                    seen = true;
                    for (S32 e = 0; e < 3; ++e)
                    {
                        edges[{ f.v[e], f.v[(e + 1) % 3] }]++;
                    }
                }
            }
            if (!seen)
            {
                continue;
            }
            for (const auto& [e, count] : edges)
            {
                if (!edges.count({ e.second, e.first }))    // a horizon edge: its other face still stands
                {
                    Face f;
                    f.v[0] = e.first; f.v[1] = e.second; f.v[2] = q;
                    f.n = (p[e.second] - p[e.first]) % (p[q] - p[e.first]);
                    if (f.n.normVec() < 1e-12f)
                    {
                        continue;
                    }
                    f.off = f.n * p[e.first];
                    faces.push_back(f);
                }
            }
        }
        for (const Face& f : faces)
        {
            if (f.alive)
            {
                out.push_back(p[f.v[0]]);
                out.push_back(p[f.v[1]]);
                out.push_back(p[f.v[2]]);
            }
        }
    }
}

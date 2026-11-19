// [android] PC model converter: see vcs_modconv.h. Format notes (measured on the stock entries, cross-checked with
// librwgta's storiesconv reader, which goes the other way): docs/ANDROID.md "Mods: models".
#include "game/vcs_modconv.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <queue>
#include <set>
#include <unordered_map>

namespace vcs::modconv
{
    namespace
    {
        // ---- little helpers ---------------------------------------------------------------------------------------

        struct Reader
        {
            const uint8_t *p;
            size_t n;
            bool ok(size_t at, size_t len) const { return at <= n && len <= n - at; }
            uint32_t u32(size_t at) const
            {
                uint32_t v = 0;
                if (ok(at, 4))
                    std::memcpy(&v, p + at, 4);
                return v;
            }
            uint16_t u16(size_t at) const
            {
                uint16_t v = 0;
                if (ok(at, 2))
                    std::memcpy(&v, p + at, 2);
                return v;
            }
            float f32(size_t at) const
            {
                float v = 0;
                if (ok(at, 4))
                    std::memcpy(&v, p + at, 4);
                return v;
            }
        };

        struct Writer
        {
            std::vector<uint8_t> b;
            std::vector<uint32_t> relocs; // offsets holding chunk-relative pointers
            size_t size() const { return b.size(); }
            void align(size_t a)
            {
                while (b.size() % a)
                    b.push_back(0);
            }
            size_t reserve(size_t n)
            {
                const size_t at = b.size();
                b.resize(at + n, 0);
                return at;
            }
            void put32(size_t at, uint32_t v) { std::memcpy(b.data() + at, &v, 4); }
            void put16(size_t at, uint16_t v) { std::memcpy(b.data() + at, &v, 2); }
            void putf(size_t at, float v) { std::memcpy(b.data() + at, &v, 4); }
            void ptr(size_t at, uint32_t target)
            {
                put32(at, target);
                relocs.push_back(static_cast<uint32_t>(at));
            }
            void bytes(size_t at, const void *src, size_t n) { std::memcpy(b.data() + at, src, n); }
        };

        struct V3
        {
            float x = 0, y = 0, z = 0;
        };
        V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
        V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
        V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
        float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
        float len(V3 a) { return std::sqrt(dot(a, a)); }
        V3 norm(V3 a)
        {
            const float l = len(a);
            return l > 1e-12f ? a * (1.0f / l) : V3{0, 0, 1};
        }

        // ---- the PC model ------------------------------------------------------------------------------------------

        struct Vertex
        {
            V3 p, n;
            float u = 0, v = 0;
        };
        struct Tri
        {
            uint32_t v[3];
            int mat;
        };
        struct Material
        {
            std::string texture; // lower case, "" = untextured
            uint8_t rgba[4] = {255, 255, 255, 255};
        };
        struct Mesh
        {
            std::vector<Vertex> verts;
            std::vector<Tri> tris;
            std::vector<Material> mats;
        };

        struct Image
        {
            int w = 0, h = 0;
            std::vector<uint8_t> rgba;
        };

        // RenderWare binary stream chunk ids
        constexpr uint32_t rwStruct = 0x1, rwString = 0x2, rwExtension = 0x3, rwTexture = 0x6, rwMaterial = 0x7,
                           rwMatList = 0x8, rwFrameList = 0xE, rwGeometry = 0xF, rwClump = 0x10, rwAtomic = 0x14,
                           rwTexNative = 0x15, rwTexDict = 0x16, rwGeomList = 0x1A;

        struct Chunk
        {
            uint32_t type = 0, size = 0, version = 0;
            size_t body = 0;
        };

        bool chunkAt(const Reader &r, size_t at, size_t end, Chunk &c)
        {
            if (at + 12 > end)
                return false;
            c.type = r.u32(at);
            c.size = r.u32(at + 4);
            c.version = r.u32(at + 8);
            c.body = at + 12;
            if (c.size > end - c.body) // a chunk running past its parent / the file (truncated mods exist): what is there
                c.size = static_cast<uint32_t>(end - c.body);
            return true;
        }

        // the library version in a chunk's version stamp (0x31000..0x36003)
        uint32_t libVersion(uint32_t stamp)
        {
            if (stamp & 0xFFFF0000u)
                return ((stamp >> 14 & 0x3FF00u) + 0x30000u) | (stamp >> 16 & 0x3Fu);
            return stamp << 8;
        }

        bool findChild(const Reader &r, size_t from, size_t end, uint32_t type, Chunk &out)
        {
            for (size_t at = from; at + 12 <= end;)
            {
                Chunk c;
                if (!chunkAt(r, at, end, c))
                    return false;
                if (c.type == type)
                {
                    out = c;
                    return true;
                }
                at = c.body + c.size;
            }
            return false;
        }

        std::string lower(std::string s)
        {
            for (char &c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        std::string cstr(const Reader &r, size_t at, size_t max)
        {
            std::string s;
            for (size_t i = 0; i < max && r.ok(at + i, 1) && r.p[at + i]; ++i)
                s.push_back(static_cast<char>(r.p[at + i]));
            return s;
        }

        struct Frame
        {
            float m[12]; // right, up, at, pos
            int parent;
            std::string name;
        };

        V3 xform(const std::vector<Frame> &frames, int f, V3 p, bool direction)
        {
            for (int guard = 0; f >= 0 && f < static_cast<int>(frames.size()) && guard < 64; ++guard)
            {
                const float *m = frames[f].m;
                V3 q{m[0] * p.x + m[3] * p.y + m[6] * p.z, m[1] * p.x + m[4] * p.y + m[7] * p.z,
                     m[2] * p.x + m[5] * p.y + m[8] * p.z};
                if (!direction)
                    q = q + V3{m[9], m[10], m[11]};
                p = q;
                f = frames[f].parent;
            }
            return p;
        }

        // One geometry chunk into the mesh (vertices transformed by `frame`'s matrix chain)
        bool readGeometry(const Reader &r, const Chunk &g, const std::vector<Frame> &frames, int frame, Mesh &mesh,
                          std::string &error)
        {
            Chunk st;
            if (!findChild(r, g.body, g.body + g.size, rwStruct, st))
                return error = "geometry without data", false;
            const uint32_t ver = libVersion(st.version);
            size_t at = st.body;
            const uint32_t flags = r.u16(at);
            uint32_t numUV = r.p[at + 2];
            const bool native = r.p[at + 3] != 0;
            const uint32_t numTris = r.u32(at + 4), numVerts = r.u32(at + 8), numMorph = r.u32(at + 12);
            if (native)
                return error = "the model holds platform-native geometry (not a PC model)", false;
            if (numVerts > 200000 || numTris > 400000 || numMorph == 0)
                return error = "implausible geometry sizes", false;
            // the count byte decides; only when it is 0 do the flags say it (textured2 0x80 = 2 sets, textured 0x4 = 1).
            // Mods exported with both textured flags set and a count of 1 exist (a GTA III taxi: flags 0xF7, one UV
            // set): taking 2 there read every triangle from the wrong place.
            if (numUV == 0)
                numUV = (flags & 0x80) ? 2u : (flags & 0x4) ? 1u : 0u;
            at += 16;
            if (ver < 0x34000)
                at += 12; // ambient, specular, diffuse
            if (flags & 0x8)
                at += 4u * numVerts; // prelight colours (not used)
            const size_t uvAt = at;
            at += 8u * numVerts * numUV;
            const size_t triAt = at;
            at += 8u * numTris;
            // the first morph target: bounding sphere, hasVertices, hasNormals
            at += 16;
            const bool hasPos = r.u32(at) != 0, hasNormals = r.u32(at + 4) != 0;
            at += 8;
            if (!hasPos || !r.ok(at, 12u * numVerts))
                return error = "geometry without positions", false;
            const size_t base = mesh.verts.size();
            for (uint32_t i = 0; i < numVerts; ++i)
            {
                Vertex v;
                v.p = xform(frames, frame, {r.f32(at + i * 12), r.f32(at + i * 12 + 4), r.f32(at + i * 12 + 8)}, false);
                if (hasNormals)
                    v.n = norm(xform(frames, frame,
                                     {r.f32(at + 12u * numVerts + i * 12), r.f32(at + 12u * numVerts + i * 12 + 4),
                                      r.f32(at + 12u * numVerts + i * 12 + 8)},
                                     true));
                if (numUV)
                {
                    v.u = r.f32(uvAt + i * 8);
                    v.v = r.f32(uvAt + i * 8 + 4);
                }
                mesh.verts.push_back(v);
            }
            // materials
            Chunk ml, mlst;
            const size_t matBase = mesh.mats.size();
            int numMats = 0;
            if (findChild(r, st.body + st.size, g.body + g.size, rwMatList, ml) &&
                findChild(r, ml.body, ml.body + ml.size, rwStruct, mlst))
            {
                numMats = static_cast<int>(r.u32(mlst.body));
                std::vector<int32_t> refs;
                for (int i = 0; i < numMats; ++i)
                    refs.push_back(static_cast<int32_t>(r.u32(mlst.body + 4 + i * 4)));
                std::vector<Material> own;
                for (size_t at2 = mlst.body + mlst.size; at2 + 12 <= ml.body + ml.size;)
                {
                    Chunk c;
                    if (!chunkAt(r, at2, ml.body + ml.size, c))
                        break;
                    if (c.type == rwMaterial)
                    {
                        Material m;
                        Chunk ms, tx;
                        if (findChild(r, c.body, c.body + c.size, rwStruct, ms))
                        {
                            std::memcpy(m.rgba, r.p + ms.body + 4, 4);
                            if (r.u32(ms.body + 12) && findChild(r, ms.body + ms.size, c.body + c.size, rwTexture, tx))
                            {
                                Chunk ts, name;
                                if (findChild(r, tx.body, tx.body + tx.size, rwStruct, ts) &&
                                    findChild(r, ts.body + ts.size, tx.body + tx.size, rwString, name))
                                    m.texture = lower(cstr(r, name.body, name.size));
                            }
                        }
                        own.push_back(m);
                    }
                    at2 = c.body + c.size;
                }
                for (int i = 0; i < numMats; ++i)
                {
                    const int32_t ref = refs[static_cast<size_t>(i)];
                    if (ref < 0 && !own.empty())
                    {
                        mesh.mats.push_back(own.front());
                        own.erase(own.begin());
                    }
                    else if (ref >= 0 && static_cast<size_t>(ref) < mesh.mats.size() - matBase)
                        mesh.mats.push_back(mesh.mats[matBase + static_cast<size_t>(ref)]);
                    else
                        mesh.mats.push_back(Material{});
                }
            }
            if (numMats == 0)
            {
                mesh.mats.push_back(Material{});
                numMats = 1;
            }
            for (uint32_t i = 0; i < numTris; ++i)
            {
                // RW: v1, v0, material, v2 (u16 each)
                const size_t t = triAt + i * 8;
                const uint32_t a = r.u16(t + 2), b = r.u16(t), m = r.u16(t + 4), c = r.u16(t + 6);
                if (a >= numVerts || b >= numVerts || c >= numVerts || a == b || b == c || a == c)
                    continue;
                mesh.tris.push_back({{static_cast<uint32_t>(base + a), static_cast<uint32_t>(base + b),
                                      static_cast<uint32_t>(base + c)},
                                     static_cast<int>(matBase + std::min<uint32_t>(m, static_cast<uint32_t>(numMats - 1)))});
            }
            if (!hasNormals) // flat-ish normals from the faces
            {
                for (size_t i = base; i < mesh.verts.size(); ++i)
                    mesh.verts[i].n = {0, 0, 0};
                for (const Tri &t : mesh.tris)
                    if (t.v[0] >= base)
                    {
                        const V3 n = cross(mesh.verts[t.v[1]].p - mesh.verts[t.v[0]].p, mesh.verts[t.v[2]].p - mesh.verts[t.v[0]].p);
                        for (uint32_t k : t.v)
                            mesh.verts[k].n = mesh.verts[k].n + n;
                    }
                for (size_t i = base; i < mesh.verts.size(); ++i)
                    mesh.verts[i].n = norm(mesh.verts[i].n);
            }
            return true;
        }

        // One atomic of a clump, its vertices in model space (its frame's whole matrix chain applied)
        struct Part
        {
            std::string name; // its frame's name, lower case ("" without the node name plugin)
            int frame = -1;
            Mesh mesh;
        };

        // Every atomic of the clump, one part each; frames with their names and matrices
        // part: only the atomics at or below the frame of that name (one weapon of a clump holding several), in that
        // frame's own space (its placement in the clump left out); "" = every atomic
        bool readDffParts(const std::vector<uint8_t> &bytes, std::vector<Frame> &frames, std::vector<Part> &parts, std::string &error,
                          const std::string &part = std::string())
        {
            Reader r{bytes.data(), bytes.size()};
            Chunk clump;
            if (!findChild(r, 0, bytes.size(), rwClump, clump))
                return error = "not a RenderWare model (.dff): no clump", false;
            const size_t end = clump.body + clump.size;
            Chunk fl, fls;
            if (findChild(r, clump.body, end, rwFrameList, fl) && findChild(r, fl.body, fl.body + fl.size, rwStruct, fls))
            {
                const uint32_t n = std::min<uint32_t>(r.u32(fls.body), 4096);
                for (uint32_t i = 0; i < n; ++i)
                {
                    Frame f;
                    for (int k = 0; k < 12; ++k)
                        f.m[k] = r.f32(fls.body + 4 + i * 56 + k * 4);
                    f.parent = static_cast<int32_t>(r.u32(fls.body + 4 + i * 56 + 48));
                    frames.push_back(f);
                }
                // frame names: the extensions after the struct, one per frame (node name plugin 0x253F2FE)
                size_t at = fls.body + fls.size;
                for (uint32_t i = 0; i < n && at + 12 <= fl.body + fl.size; ++i)
                {
                    Chunk ext;
                    if (!chunkAt(r, at, fl.body + fl.size, ext) || ext.type != rwExtension)
                        break;
                    Chunk nm;
                    if (findChild(r, ext.body, ext.body + ext.size, 0x253F2FEu, nm))
                        frames[i].name = lower(cstr(r, nm.body, nm.size));
                    at = ext.body + ext.size;
                }
            }
            int partFrame = -1;
            if (!part.empty())
            {
                for (size_t i = 0; i < frames.size() && partFrame < 0; ++i)
                    if (frames[i].name == part)
                        partFrame = static_cast<int>(i);
                if (partFrame < 0)
                    return error = "the model has no part named " + part, false;
                // the part's frame becomes the root: identity, no parent (xform stops there)
                const float identity[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
                std::memcpy(frames[static_cast<size_t>(partFrame)].m, identity, sizeof identity);
            }
            auto under = [&](int f) {
                for (int guard = 0; f >= 0 && f < static_cast<int>(frames.size()) && guard < 64; ++guard, f = frames[static_cast<size_t>(f)].parent)
                    if (f == partFrame)
                        return true;
                return false;
            };
            if (partFrame >= 0)
                frames[static_cast<size_t>(partFrame)].parent = -1;
            std::vector<Chunk> geoms;
            Chunk gl;
            if (findChild(r, clump.body, end, rwGeomList, gl))
                for (size_t at = gl.body; at + 12 <= gl.body + gl.size;)
                {
                    Chunk c;
                    if (!chunkAt(r, at, gl.body + gl.size, c))
                        break;
                    if (c.type == rwGeometry)
                        geoms.push_back(c);
                    at = c.body + c.size;
                }
            for (size_t at = clump.body; at + 12 <= end;)
            {
                Chunk c;
                if (!chunkAt(r, at, end, c))
                    break;
                if (c.type == rwAtomic)
                {
                    Chunk as;
                    if (findChild(r, c.body, c.body + c.size, rwStruct, as))
                    {
                        const int frame = static_cast<int>(r.u32(as.body)), geom = static_cast<int>(r.u32(as.body + 4));
                        if (geom >= 0 && geom < static_cast<int>(geoms.size()) && (partFrame < 0 || under(frame)))
                        {
                            Part part;
                            part.frame = frame;
                            part.name = frame >= 0 && frame < static_cast<int>(frames.size()) ? frames[frame].name : "";
                            if (!readGeometry(r, geoms[static_cast<size_t>(geom)], frames, frame, part.mesh, error))
                                return false;
                            parts.push_back(std::move(part));
                        }
                    }
                }
                at = c.body + c.size;
            }
            return true;
        }

        // `src` added to `dst`, moved by `offset`
        void appendMesh(Mesh &dst, const Mesh &src, V3 offset)
        {
            const uint32_t vbase = static_cast<uint32_t>(dst.verts.size());
            const int mbase = static_cast<int>(dst.mats.size());
            for (Vertex v : src.verts)
            {
                v.p = v.p + offset;
                dst.verts.push_back(v);
            }
            dst.mats.insert(dst.mats.end(), src.mats.begin(), src.mats.end());
            for (Tri t : src.tris)
            {
                for (uint32_t &k : t.v)
                    k += vbase;
                t.mat += mbase;
                dst.tris.push_back(t);
            }
        }

        bool lowDetail(const std::string &name)
        {
            return name.find("_dam") != std::string::npos || name.find("_vlo") != std::string::npos ||
                   name.find("_lo") != std::string::npos;
        }

        // A clump as one mesh: every atomic whose frame isn't a damaged / low-detail part
        bool readDff(const std::vector<uint8_t> &bytes, Mesh &mesh, std::string &report, std::string &error,
                     const std::string &part = std::string())
        {
            std::vector<Frame> frames;
            std::vector<Part> parts;
            if (!readDffParts(bytes, frames, parts, error, part))
                return false;
            int used = 0, skipped = 0;
            for (const Part &part : parts)
                if (lowDetail(part.name))
                    ++skipped;
                else
                    appendMesh(mesh, part.mesh, {0, 0, 0}), ++used;
            if (mesh.tris.empty())
                return error = "the model has no triangles", false;
            char line[160];
            std::snprintf(line, sizeof(line), "model: %d part(s) (%d damaged / low-detail skipped), %zu vertices, %zu triangles, %zu materials",
                          used, skipped, mesh.verts.size(), mesh.tris.size(), mesh.mats.size());
            report += line;
            report += "\n";
            return true;
        }

        // ---- textures (.txd, D3D8 / D3D9) --------------------------------------------------------------------------

        void decodeDxt(const uint8_t *src, int w, int h, int type, std::vector<uint8_t> &out)
        {
            out.assign(static_cast<size_t>(w) * h * 4, 255);
            const int bw = (w + 3) / 4, bh = (h + 3) / 4, blockBytes = type == 1 ? 8 : 16;
            for (int by = 0; by < bh; ++by)
                for (int bx = 0; bx < bw; ++bx)
                {
                    const uint8_t *b = src + (static_cast<size_t>(by) * bw + bx) * blockBytes;
                    const uint8_t *cb = type == 1 ? b : b + 8;
                    uint16_t c0, c1;
                    std::memcpy(&c0, cb, 2);
                    std::memcpy(&c1, cb + 2, 2);
                    uint8_t col[4][4];
                    auto expand = [](uint16_t c, uint8_t *o) {
                        o[0] = static_cast<uint8_t>((c >> 11 & 31) * 255 / 31);
                        o[1] = static_cast<uint8_t>((c >> 5 & 63) * 255 / 63);
                        o[2] = static_cast<uint8_t>((c & 31) * 255 / 31);
                        o[3] = 255;
                    };
                    expand(c0, col[0]);
                    expand(c1, col[1]);
                    for (int k = 0; k < 3; ++k)
                    {
                        if (c0 > c1 || type != 1)
                        {
                            col[2][k] = static_cast<uint8_t>((2 * col[0][k] + col[1][k]) / 3);
                            col[3][k] = static_cast<uint8_t>((col[0][k] + 2 * col[1][k]) / 3);
                        }
                        else
                        {
                            col[2][k] = static_cast<uint8_t>((col[0][k] + col[1][k]) / 2);
                            col[3][k] = 0;
                        }
                    }
                    col[2][3] = 255;
                    col[3][3] = (c0 > c1 || type != 1) ? 255 : 0;
                    uint32_t bits;
                    std::memcpy(&bits, cb + 4, 4);
                    uint8_t alpha[16];
                    for (int i = 0; i < 16; ++i)
                        alpha[i] = 255;
                    if (type == 3)
                        for (int i = 0; i < 16; ++i)
                            alpha[i] = static_cast<uint8_t>(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
                    else if (type == 5)
                    {
                        uint8_t a[8] = {b[0], b[1]};
                        for (int i = 2; i < 8; ++i)
                            a[i] = a[0] > a[1] ? static_cast<uint8_t>(((8 - i) * a[0] + (i - 1) * a[1]) / 7)
                                               : i < 6 ? static_cast<uint8_t>(((6 - i) * a[0] + (i - 1) * a[1]) / 5)
                                                       : (i == 6 ? 0 : 255);
                        uint64_t ab = 0;
                        std::memcpy(&ab, b + 2, 6);
                        for (int i = 0; i < 16; ++i)
                            alpha[i] = a[(ab >> (3 * i)) & 7];
                    }
                    for (int i = 0; i < 16; ++i)
                    {
                        const int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                        if (x >= w || y >= h)
                            continue;
                        uint8_t *o = &out[(static_cast<size_t>(y) * w + x) * 4];
                        const uint8_t *c = col[(bits >> (2 * i)) & 3];
                        o[0] = c[0], o[1] = c[1], o[2] = c[2];
                        o[3] = static_cast<uint8_t>(c[3] * alpha[i] / 255);
                    }
                }
        }

        bool readTexNative(const Reader &r, const Chunk &tn, std::string &name, Image &img)
        {
            Chunk st;
            if (!findChild(r, tn.body, tn.body + tn.size, rwStruct, st))
                return false;
            const size_t s = st.body;
            const uint32_t platform = r.u32(s);
            if (platform != 8 && platform != 9)
                return false;
            name = lower(cstr(r, s + 8, 32));
            const uint32_t format = r.u32(s + 72), fourcc = r.u32(s + 76);
            const int w = r.u16(s + 80), h = r.u16(s + 82), depth = r.p[s + 84];
            const uint8_t compression = r.p[s + 87];
            if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
                return false;
            size_t at = s + 88;
            img.w = w;
            img.h = h;
            img.rgba.assign(static_cast<size_t>(w) * h * 4, 255);
            int dxt = 0;
            if (platform == 8 && compression)
                dxt = compression;
            if (platform == 9 && (r.p[s + 87] & 8))
                dxt = fourcc == 0x31545844u ? 1 : fourcc == 0x33545844u ? 3 : fourcc == 0x35545844u ? 5 : 0;
            if (platform == 9 && !dxt && (fourcc == 0x31545844u || fourcc == 0x33545844u || fourcc == 0x35545844u))
                dxt = fourcc == 0x31545844u ? 1 : fourcc == 0x33545844u ? 3 : 5;
            const bool pal8 = format & 0x2000, pal4 = format & 0x4000;
            std::vector<uint8_t> palette;
            if (pal8 || pal4)
            {
                const size_t n = pal8 ? 256 : 32; // D3D8 PAL4 stores 32 entries
                if (!r.ok(at, n * 4))
                    return false;
                palette.assign(r.p + at, r.p + at + n * 4);
                at += n * 4;
            }
            const uint32_t size = r.u32(at);
            at += 4;
            if (!r.ok(at, size))
                return false;
            const uint8_t *px = r.p + at;
            const size_t count = static_cast<size_t>(w) * h;
            if (dxt)
            {
                if (size < static_cast<size_t>((w + 3) / 4) * ((h + 3) / 4) * (dxt == 1 ? 8 : 16))
                    return false;
                decodeDxt(px, w, h, dxt, img.rgba);
                return true;
            }
            const uint32_t kind = format & 0x0F00;
            for (size_t i = 0; i < count; ++i)
            {
                uint8_t *o = &img.rgba[i * 4];
                if (pal8 || pal4)
                {
                    const uint32_t idx = pal8 ? px[i] : (px[i / 2] >> ((i & 1) * 4)) & 15;
                    if ((pal8 && i >= size) || (pal4 && i / 2 >= size))
                        return false;
                    std::memcpy(o, &palette[idx * 4], 4);
                    if (kind == 0x0600) // 888: no alpha
                        o[3] = 255;
                }
                else if (depth == 32 || kind == 0x0500 || kind == 0x0600)
                {
                    if (i * 4 + 4 > size)
                        return false;
                    o[0] = px[i * 4 + 2], o[1] = px[i * 4 + 1], o[2] = px[i * 4 + 0];
                    o[3] = kind == 0x0600 ? 255 : px[i * 4 + 3];
                }
                else if (depth == 16)
                {
                    if (i * 2 + 2 > size)
                        return false;
                    uint16_t c;
                    std::memcpy(&c, px + i * 2, 2);
                    if (kind == 0x0200) // 565
                        o[0] = static_cast<uint8_t>((c >> 11 & 31) * 255 / 31), o[1] = static_cast<uint8_t>((c >> 5 & 63) * 255 / 63),
                        o[2] = static_cast<uint8_t>((c & 31) * 255 / 31), o[3] = 255;
                    else if (kind == 0x0300) // 4444
                        o[0] = static_cast<uint8_t>((c >> 8 & 15) * 17), o[1] = static_cast<uint8_t>((c >> 4 & 15) * 17),
                        o[2] = static_cast<uint8_t>((c & 15) * 17), o[3] = static_cast<uint8_t>((c >> 12 & 15) * 17);
                    else // 1555
                        o[0] = static_cast<uint8_t>((c >> 10 & 31) * 255 / 31), o[1] = static_cast<uint8_t>((c >> 5 & 31) * 255 / 31),
                        o[2] = static_cast<uint8_t>((c & 31) * 255 / 31), o[3] = (c & 0x8000) ? 255 : 0;
                }
                else
                    return false;
            }
            return true;
        }

        void readTxd(const std::vector<uint8_t> &bytes, std::map<std::string, Image> &out)
        {
            Reader r{bytes.data(), bytes.size()};
            Chunk td;
            if (!findChild(r, 0, bytes.size(), rwTexDict, td))
                return;
            for (size_t at = td.body; at + 12 <= td.body + td.size;)
            {
                Chunk c;
                if (!chunkAt(r, at, td.body + td.size, c))
                    break;
                if (c.type == rwTexNative)
                {
                    std::string name;
                    Image img;
                    if (readTexNative(r, c, name, img) && !name.empty())
                        out[name] = std::move(img);
                }
                at = c.body + c.size;
            }
        }

        int pow2Below(int v, int cap)
        {
            int p = 8;
            while (p * 2 <= v && p * 2 <= cap)
                p *= 2;
            return p;
        }

        // box filter to (w, h), powers of two
        Image resize(const Image &src, int w, int h)
        {
            Image d;
            d.w = w;
            d.h = h;
            d.rgba.resize(static_cast<size_t>(w) * h * 4);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    const int x0 = x * src.w / w, x1 = std::max(x0 + 1, (x + 1) * src.w / w);
                    const int y0 = y * src.h / h, y1 = std::max(y0 + 1, (y + 1) * src.h / h);
                    uint32_t acc[4] = {}, n = 0;
                    for (int yy = y0; yy < y1; ++yy)
                        for (int xx = x0; xx < x1; ++xx, ++n)
                            for (int k = 0; k < 4; ++k)
                                acc[k] += src.rgba[(static_cast<size_t>(yy) * src.w + xx) * 4 + k];
                    for (int k = 0; k < 4; ++k)
                        d.rgba[(static_cast<size_t>(y) * w + x) * 4 + k] = static_cast<uint8_t>(acc[k] / n);
                }
            return d;
        }

        // median-cut to at most 256 colours: indices + palette (RGBA)
        void palettise(const Image &img, std::vector<uint8_t> &indices, std::vector<std::array<uint8_t, 4>> &palette)
        {
            struct Box
            {
                std::vector<uint32_t> px;
            };
            const size_t n = static_cast<size_t>(img.w) * img.h;
            auto col = [&](uint32_t i, int k) { return img.rgba[i * 4 + static_cast<size_t>(k)]; };
            std::vector<Box> boxes(1);
            for (uint32_t i = 0; i < n; ++i)
                boxes[0].px.push_back(i);
            while (boxes.size() < 256)
            {
                int best = -1, bestK = 0, bestRange = 0;
                for (size_t b = 0; b < boxes.size(); ++b)
                {
                    if (boxes[b].px.size() < 2)
                        continue;
                    for (int k = 0; k < 4; ++k)
                    {
                        int lo = 255, hi = 0;
                        for (uint32_t i : boxes[b].px)
                            lo = std::min(lo, static_cast<int>(col(i, k))), hi = std::max(hi, static_cast<int>(col(i, k)));
                        if (hi - lo > bestRange)
                            bestRange = hi - lo, best = static_cast<int>(b), bestK = k;
                    }
                }
                if (best < 0 || bestRange == 0)
                    break;
                auto &px = boxes[static_cast<size_t>(best)].px;
                std::sort(px.begin(), px.end(), [&](uint32_t a, uint32_t b) { return col(a, bestK) < col(b, bestK); });
                Box other;
                other.px.assign(px.begin() + static_cast<long>(px.size() / 2), px.end());
                px.resize(px.size() / 2);
                boxes.push_back(std::move(other));
            }
            indices.assign(n, 0);
            palette.clear();
            for (size_t b = 0; b < boxes.size(); ++b)
            {
                uint64_t acc[4] = {};
                for (uint32_t i : boxes[b].px)
                    for (int k = 0; k < 4; ++k)
                        acc[k] += col(i, k);
                const size_t m = std::max<size_t>(1, boxes[b].px.size());
                palette.push_back({static_cast<uint8_t>(acc[0] / m), static_cast<uint8_t>(acc[1] / m),
                                   static_cast<uint8_t>(acc[2] / m), static_cast<uint8_t>(acc[3] / m)});
                for (uint32_t i : boxes[b].px)
                    indices[i] = static_cast<uint8_t>(b);
            }
            while (palette.size() < 256)
                palette.push_back({0, 0, 0, 0});
        }

        // ---- decimation (quadric edge collapse onto an endpoint; borders and seams stay) ---------------------------

        struct Quadric
        {
            double q[10] = {};
            void addPlane(double a, double b, double c, double d, double w)
            {
                const double p[4] = {a, b, c, d};
                int k = 0;
                for (int i = 0; i < 4; ++i)
                    for (int j = i; j < 4; ++j)
                        q[k++] += w * p[i] * p[j];
            }
            double eval(V3 v) const
            {
                const double x = v.x, y = v.y, z = v.z;
                return q[0] * x * x + 2 * q[1] * x * y + 2 * q[2] * x * z + 2 * q[3] * x + q[4] * y * y + 2 * q[5] * y * z +
                       2 * q[6] * y + q[7] * z * z + 2 * q[8] * z + q[9];
            }
            void add(const Quadric &o)
            {
                for (int i = 0; i < 10; ++i)
                    q[i] += o.q[i];
            }
        };

        int decimateSplit(Mesh &m, int target)
        {
            const size_t nv = m.verts.size();
            std::vector<Quadric> Q(nv);
            std::vector<std::vector<uint32_t>> vf(nv); // faces per vertex
            std::vector<bool> alive(m.tris.size(), true);
            for (uint32_t f = 0; f < m.tris.size(); ++f)
            {
                const Tri &t = m.tris[f];
                const V3 a = m.verts[t.v[0]].p, b = m.verts[t.v[1]].p, c = m.verts[t.v[2]].p;
                V3 n = cross(b - a, c - a);
                const float area = len(n);
                if (area < 1e-12f)
                    continue;
                n = n * (1.0f / area);
                for (uint32_t k : t.v)
                {
                    Q[k].addPlane(n.x, n.y, n.z, -dot(n, a), area);
                    vf[k].push_back(f);
                }
            }
            // border edges (one face): their vertices are not moved
            std::map<std::pair<uint32_t, uint32_t>, int> edgeCount;
            for (const Tri &t : m.tris)
                for (int e = 0; e < 3; ++e)
                {
                    uint32_t a = t.v[e], b = t.v[(e + 1) % 3];
                    if (a > b)
                        std::swap(a, b);
                    ++edgeCount[{a, b}];
                }
            std::vector<bool> locked(nv, false);
            for (const auto &[e, c] : edgeCount)
                if (c != 2)
                    locked[e.first] = locked[e.second] = true;
            std::vector<uint32_t> remap(nv);
            for (uint32_t i = 0; i < nv; ++i)
                remap[i] = i;
            auto find = [&](uint32_t v) {
                while (remap[v] != v)
                    v = remap[v] = remap[remap[v]];
                return v;
            };
            struct Cand
            {
                double cost;
                uint32_t from, to;
                bool operator<(const Cand &o) const { return cost > o.cost; }
            };
            std::priority_queue<Cand> heap;
            auto push = [&](uint32_t a, uint32_t b) {
                if (!locked[a])
                {
                    Quadric q = Q[a];
                    q.add(Q[b]);
                    heap.push({q.eval(m.verts[b].p), a, b});
                }
                if (!locked[b])
                {
                    Quadric q = Q[a];
                    q.add(Q[b]);
                    heap.push({q.eval(m.verts[a].p), b, a});
                }
            };
            for (const auto &[e, c] : edgeCount)
                push(e.first, e.second);
            int faces = static_cast<int>(m.tris.size());
            std::vector<uint32_t> version(nv, 0);
            while (faces > target && !heap.empty())
            {
                const Cand c = heap.top();
                heap.pop();
                const uint32_t from = find(c.from), to = find(c.to);
                if (from != c.from || to != c.to || from == to || locked[from])
                    continue;
                // would a face around `from` flip, or the faces around `from` already be gone?
                bool flips = false;
                std::vector<uint32_t> removed;
                for (uint32_t f : vf[from])
                {
                    if (!alive[f])
                        continue;
                    Tri &t = m.tris[f];
                    uint32_t v[3] = {find(t.v[0]), find(t.v[1]), find(t.v[2])};
                    if (v[0] == to || v[1] == to || v[2] == to)
                    {
                        removed.push_back(f);
                        continue;
                    }
                    const V3 before = cross(m.verts[v[1]].p - m.verts[v[0]].p, m.verts[v[2]].p - m.verts[v[0]].p);
                    for (uint32_t &x : v)
                        if (x == from)
                            x = to;
                    const V3 after = cross(m.verts[v[1]].p - m.verts[v[0]].p, m.verts[v[2]].p - m.verts[v[0]].p);
                    // a face that would flip, or turn by more than ~70 degrees (the spikes of a hard collapse)
                    if (dot(before, after) <= 0.35f * len(before) * len(after))
                    {
                        flips = true;
                        break;
                    }
                }
                if (flips || removed.empty())
                    continue;
                remap[from] = to;
                for (uint32_t f : removed)
                    alive[f] = false, --faces;
                Q[to].add(Q[from]);
                for (uint32_t f : vf[from])
                    if (alive[f])
                        vf[to].push_back(f);
                // new candidates around `to`
                std::set<uint32_t> ring;
                for (uint32_t f : vf[to])
                    if (alive[f])
                        for (uint32_t k : m.tris[f].v)
                            if (find(k) != to)
                                ring.insert(find(k));
                for (uint32_t k : ring)
                    push(k, to);
            }
            std::vector<Tri> out;
            for (uint32_t f = 0; f < m.tris.size(); ++f)
                if (alive[f])
                {
                    Tri t = m.tris[f];
                    for (uint32_t &k : t.v)
                        k = find(k);
                    if (t.v[0] != t.v[1] && t.v[1] != t.v[2] && t.v[0] != t.v[2])
                        out.push_back(t);
                }
            m.tris.swap(out);
            return static_cast<int>(m.tris.size());
        }

        // The second pass, for meshes the first can't bring down: PC mods split nearly every vertex at UV seams and
        // normal creases (a 12.8k-triangle SA rifle: 15.8k vertices, the split pass stopped at 10.8k triangles, every
        // edge a "border"). Here vertices at one position are one point ("group"): an edge collapse moves a whole group
        // onto a neighbouring group, and each of its vertices onto the vertex of that group with the nearest UV (so a
        // texture island stays continuous; across a seam the nearest island's UV is taken). Open borders of the welded
        // surface stay locked, as in the first pass.
        int decimateWelded(Mesh &m, int target)
        {
            const size_t nv = m.verts.size();
            std::vector<uint32_t> group(nv);
            std::vector<std::vector<uint32_t>> members;
            {
                std::map<std::array<int64_t, 3>, uint32_t> at;
                for (uint32_t i = 0; i < nv; ++i)
                {
                    const V3 p = m.verts[i].p;
                    const std::array<int64_t, 3> k = {std::llround(p.x * 1e4), std::llround(p.y * 1e4), std::llround(p.z * 1e4)};
                    auto it = at.find(k);
                    if (it == at.end())
                        it = at.emplace(k, static_cast<uint32_t>(members.size())).first, members.emplace_back();
                    group[i] = it->second;
                    members[it->second].push_back(i);
                }
            }
            const size_t ng = members.size();
            std::vector<V3> gp(ng);
            for (size_t g = 0; g < ng; ++g)
                gp[g] = m.verts[members[g][0]].p;
            std::vector<Quadric> Q(ng);
            std::vector<std::vector<uint32_t>> gf(ng); // faces per group
            std::vector<bool> alive(m.tris.size(), true);
            std::map<std::pair<uint32_t, uint32_t>, int> edgeCount;
            for (uint32_t f = 0; f < m.tris.size(); ++f)
            {
                const Tri &t = m.tris[f];
                const uint32_t g[3] = {group[t.v[0]], group[t.v[1]], group[t.v[2]]};
                if (g[0] == g[1] || g[1] == g[2] || g[0] == g[2])
                {
                    alive[f] = false; // degenerate once welded
                    continue;
                }
                V3 n = cross(gp[g[1]] - gp[g[0]], gp[g[2]] - gp[g[0]]);
                const float area = len(n);
                if (area >= 1e-12f)
                {
                    n = n * (1.0f / area);
                    for (uint32_t k : g)
                        Q[k].addPlane(n.x, n.y, n.z, -dot(n, gp[g[0]]), area);
                }
                for (int e = 0; e < 3; ++e)
                {
                    gf[g[e]].push_back(f);
                    uint32_t a = g[e], b = g[(e + 1) % 3];
                    if (a > b)
                        std::swap(a, b);
                    ++edgeCount[{a, b}];
                }
            }
            // open borders (one face: car bodies are many open shells) may slide along themselves: each border edge
            // adds a heavy plane through it, upright on its face, so moving a border point off its line costs much;
            // non-manifold edges (three faces or more) stay locked
            std::vector<bool> locked(ng, false);
            for (const auto &[e, c] : edgeCount)
                if (c > 2)
                    locked[e.first] = locked[e.second] = true;
            for (uint32_t f = 0; f < m.tris.size(); ++f)
            {
                if (!alive[f])
                    continue;
                const Tri &t = m.tris[f];
                const uint32_t g[3] = {group[t.v[0]], group[t.v[1]], group[t.v[2]]};
                const V3 fn = norm(cross(gp[g[1]] - gp[g[0]], gp[g[2]] - gp[g[0]]));
                for (int k = 0; k < 3; ++k)
                {
                    uint32_t a = g[k], b = g[(k + 1) % 3];
                    if (edgeCount[{std::min(a, b), std::max(a, b)}] != 1)
                        continue;
                    const V3 dir = gp[b] - gp[a];
                    const V3 pn = norm(cross(dir, fn));
                    const double w = 1000.0 * dot(dir, dir);
                    Q[a].addPlane(pn.x, pn.y, pn.z, -dot(pn, gp[a]), w);
                    Q[b].addPlane(pn.x, pn.y, pn.z, -dot(pn, gp[a]), w);
                }
            }
            std::vector<uint32_t> vmap(nv); // vertex -> the vertex it became
            for (uint32_t i = 0; i < nv; ++i)
                vmap[i] = i;
            auto vfind = [&](uint32_t v) {
                while (vmap[v] != v)
                    v = vmap[v] = vmap[vmap[v]];
                return v;
            };
            std::vector<uint32_t> gmap(ng);
            for (uint32_t i = 0; i < ng; ++i)
                gmap[i] = i;
            auto gfind = [&](uint32_t g) {
                while (gmap[g] != g)
                    g = gmap[g] = gmap[gmap[g]];
                return g;
            };
            struct Cand
            {
                double cost;
                uint32_t from, to;
                bool operator<(const Cand &o) const { return cost > o.cost; }
            };
            std::priority_queue<Cand> heap;
            auto push = [&](uint32_t a, uint32_t b) {
                Quadric q = Q[a];
                q.add(Q[b]);
                if (!locked[a])
                    heap.push({q.eval(gp[b]), a, b});
                if (!locked[b])
                    heap.push({q.eval(gp[a]), b, a});
            };
            for (const auto &[e, c] : edgeCount)
                push(e.first, e.second);
            int faces = 0;
            for (bool a : alive)
                faces += a;
            auto corner = [&](uint32_t v) { return gfind(group[vfind(v)]); };
            while (faces > target && !heap.empty())
            {
                const Cand c = heap.top();
                heap.pop();
                const uint32_t from = gfind(c.from), to = gfind(c.to);
                if (from != c.from || to != c.to || from == to || locked[from])
                    continue;
                bool flips = false;
                std::vector<uint32_t> removed;
                for (uint32_t f : gf[from])
                {
                    if (!alive[f])
                        continue;
                    const Tri &t = m.tris[f];
                    uint32_t g[3] = {corner(t.v[0]), corner(t.v[1]), corner(t.v[2])};
                    if (g[0] == to || g[1] == to || g[2] == to)
                    {
                        removed.push_back(f);
                        continue;
                    }
                    const V3 before = cross(gp[g[1]] - gp[g[0]], gp[g[2]] - gp[g[0]]);
                    for (uint32_t &x : g)
                        if (x == from)
                            x = to;
                    const V3 after = cross(gp[g[1]] - gp[g[0]], gp[g[2]] - gp[g[0]]);
                    // a face that would flip, or turn by more than ~70 degrees (the spikes of a hard collapse)
                    if (dot(before, after) <= 0.35f * len(before) * len(after))
                    {
                        flips = true;
                        break;
                    }
                }
                if (flips || removed.empty())
                    continue;
                // each vertex of `from` onto the vertex of `to` with the nearest UV
                std::vector<uint32_t> targets;
                for (uint32_t v : members[to])
                    if (vfind(v) == v)
                        targets.push_back(v);
                if (targets.empty())
                    continue;
                for (uint32_t v : members[from])
                {
                    if (vfind(v) != v)
                        continue;
                    uint32_t best = targets[0];
                    float bestD = 1e30f;
                    for (uint32_t u : targets)
                    {
                        const float du = m.verts[u].u - m.verts[v].u, dv = m.verts[u].v - m.verts[v].v;
                        if (du * du + dv * dv < bestD)
                            bestD = du * du + dv * dv, best = u;
                    }
                    vmap[v] = best;
                }
                gmap[from] = to;
                for (uint32_t f : removed)
                    alive[f] = false, --faces;
                Q[to].add(Q[from]);
                for (uint32_t f : gf[from])
                    if (alive[f])
                        gf[to].push_back(f);
                std::set<uint32_t> ring;
                for (uint32_t f : gf[to])
                    if (alive[f])
                        for (uint32_t k : m.tris[f].v)
                            if (corner(k) != to)
                                ring.insert(corner(k));
                for (uint32_t k : ring)
                    push(k, to);
            }
            std::vector<Tri> out;
            for (uint32_t f = 0; f < m.tris.size(); ++f)
                if (alive[f])
                {
                    Tri t = m.tris[f];
                    for (uint32_t &k : t.v)
                        k = vfind(k);
                    if (t.v[0] != t.v[1] && t.v[1] != t.v[2] && t.v[0] != t.v[2] && corner(t.v[0]) != corner(t.v[1]) &&
                        corner(t.v[1]) != corner(t.v[2]) && corner(t.v[0]) != corner(t.v[2]))
                        out.push_back(t);
                }
            m.tris.swap(out);
            return static_cast<int>(m.tris.size());
        }

        // Decimation to `target` triangles: the split pass (seams kept exactly), then the welded one if that was not
        // enough (more than 15% over)
        int decimate(Mesh &m, int target)
        {
            int n = decimateSplit(m, target);
            if (n > target + target * 15 / 100)
                n = decimateWelded(m, target);
            return n;
        }

        // ---- strips -----------------------------------------------------------------------------------------------

        // Greedy strips for one material's triangles, joined by degenerate triangles; each strip starts at an even
        // position so every triangle keeps its winding (triangle i of a strip is (s[i], s[i+1], s[i+2]), flipped for odd i).
        std::vector<uint32_t> stripify(const std::vector<Tri> &tris)
        {
            std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> byEdge; // directed edge -> triangles
            for (uint32_t f = 0; f < tris.size(); ++f)
                for (int e = 0; e < 3; ++e)
                    byEdge[{tris[f].v[e], tris[f].v[(e + 1) % 3]}].push_back(f);
            std::vector<bool> used(tris.size(), false);
            std::vector<uint32_t> out;
            auto third = [&](uint32_t f, uint32_t a, uint32_t b) {
                for (uint32_t k : tris[f].v)
                    if (k != a && k != b)
                        return k;
                return tris[f].v[0];
            };
            // each strip: the start triangle tried in its three rotations, the longest kept
            std::vector<uint32_t> stamp(tris.size(), 0);
            uint32_t attempt = 0;
            auto grow = [&](uint32_t start, int rot, std::vector<uint32_t> &s, std::vector<uint32_t> &faces) {
                ++attempt;
                s = {tris[start].v[rot], tris[start].v[(rot + 1) % 3], tris[start].v[(rot + 2) % 3]};
                faces = {start};
                stamp[start] = attempt;
                for (;;)
                {
                    // the triangle at index i = s.size() - 2: even i is (s[i], s[i+1], x), so its face holds the
                    // directed edge s[i] -> s[i+1]; odd i is flipped
                    const size_t i = s.size() - 2;
                    const uint32_t a = s[i], b = s[i + 1];
                    const std::pair<uint32_t, uint32_t> want = (i % 2 == 0) ? std::make_pair(a, b) : std::make_pair(b, a);
                    uint32_t next = UINT32_MAX;
                    auto it = byEdge.find(want);
                    if (it != byEdge.end())
                        for (uint32_t f : it->second)
                            if (!used[f] && stamp[f] != attempt)
                            {
                                next = f;
                                break;
                            }
                    if (next == UINT32_MAX)
                        break;
                    stamp[next] = attempt;
                    faces.push_back(next);
                    s.push_back(third(next, a, b));
                }
            };
            for (uint32_t start = 0; start < tris.size(); ++start)
            {
                if (used[start])
                    continue;
                std::vector<uint32_t> s, faces, bestS, bestFaces;
                for (int rot = 0; rot < 3; ++rot)
                {
                    grow(start, rot, s, faces);
                    if (faces.size() > bestFaces.size())
                        bestS = s, bestFaces = faces;
                }
                for (uint32_t f : bestFaces)
                    used[f] = true;
                s = std::move(bestS);
                if (!out.empty())
                {
                    out.push_back(out.back());
                    out.push_back(s.front());
                    if (out.size() % 2)
                        out.push_back(s.front());
                }
                out.insert(out.end(), s.begin(), s.end());
            }
            return out;
        }

        // ---- the donor's structures --------------------------------------------------------------------------------

        struct Donor
        {
            std::vector<uint8_t> element, node; // 0x30 / 0xB0 bytes, pointers cleared
            uint32_t meshUnknown = 0;
            uint8_t matColour[4] = {0x96, 0x96, 0x96, 0xFF};
            uint32_t geomFlags = 7;
        };

        bool readDonorMdl(const std::vector<uint8_t> &b, Donor &d, std::string &error)
        {
            Reader r{b.data(), b.size()};
            if (r.u32(0) != 0x006D646Cu)
                return error = "the stock model is not an MDL chunk", false;
            if ((r.u32(0x1C) >> 16) != 1)
                return error = "the stock model is not a single-element model (vehicles are not supported yet)", false;
            const uint32_t e = r.u32(0x20);
            if (!r.ok(e, 0x30) || r.p[e] != 1)
                return error = "the stock model's element was not found", false;
            const uint32_t node = r.u32(e + 4), geom = r.u32(e + 20);
            if (!r.ok(node, 0xB0) || !r.ok(geom, 0x20 + 0x40 + 0x30) || r.p[geom] != 8)
                return error = "the stock model's node / geometry was not found", false;
            d.element.assign(b.begin() + e, b.begin() + e + 0x30);
            d.node.assign(b.begin() + node, b.begin() + node + 0xB0);
            const uint32_t matList = r.u32(geom + 12);
            if (r.u32(geom + 16) >= 1 && r.ok(matList, 4) && r.ok(r.u32(matList), 16))
                std::memcpy(d.matColour, b.data() + r.u32(matList) + 4, 4);
            d.geomFlags = r.u32(geom + 0x20 + 20);
            d.meshUnknown = r.u32(geom + 0x60 + 24);
            return true;
        }

        struct DonorTexture
        {
            std::string name;
            uint32_t unk2, flags;
            std::vector<uint8_t> data; // texels + palette
        };

        bool readDonorXet(const std::vector<uint8_t> &b, std::vector<DonorTexture> &out, std::vector<uint8_t> &gap,
                          std::string &error, bool lcs = false)
        {
            Reader r{b.data(), b.size()};
            if (r.u32(0) != 0x00746578u)
                return error = "the stock texture list is not an XTX chunk", false;
            if (lcs)
            {
                // [lcs] rasters {texels, flags}. A texture's bytes run to the next texture's (mipmap levels follow the
                // palette: the stock KURUMA 128 x 128 texture's flags 0xF5081C7 and 0x1540 bytes of levels after it), or
                // to the first texture / raster struct: copied whole, so the stock flags stay true.
                std::vector<uint32_t> starts, structs;
                for (uint32_t link = r.u32(0x28), guard = 0; link != 0x28 && guard < 64 && r.ok(link - 8, 80); ++guard)
                {
                    const uint32_t t = link - 8, ras = r.u32(t);
                    structs.push_back(t);
                    structs.push_back(ras);
                    starts.push_back(r.u32(ras));
                    link = r.u32(link);
                }
                for (uint32_t link = r.u32(0x28), guard = 0; link != 0x28 && guard < 64; ++guard)
                {
                    const uint32_t t = link - 8;
                    if (!r.ok(t, 80) || !r.ok(r.u32(t), 8))
                        return error = "the stock texture list is damaged", false;
                    const uint32_t ras = r.u32(t), data = r.u32(ras);
                    uint32_t end = r.u32(0xC); // the data size
                    for (uint32_t s : starts)
                        if (s > data && s < end)
                            end = s;
                    for (uint32_t s : structs)
                        if (s > data && s < end)
                            end = s;
                    if (!r.ok(data, end - data) || end <= data)
                        return error = "the stock texture list is damaged", false;
                    DonorTexture d;
                    d.name = cstr(r, t + 16, 32);
                    d.unk2 = 0;
                    d.flags = r.u32(ras + 4);
                    d.data.assign(b.begin() + data, b.begin() + end);
                    out.push_back(std::move(d));
                    link = r.u32(link);
                }
                gap.assign(b.begin() + 0x38, b.begin() + 0x40); // [lcs] the texels start at +0x40 (VCS +0x50)
                return true;
            }
            uint32_t link = r.u32(0x28);
            for (int guard = 0; link != 0x28 && guard < 64; ++guard)
            {
                const uint32_t t = link - 8;
                if (!r.ok(t, 80))
                    return error = "the stock texture list is damaged", false;
                DonorTexture d;
                d.name = cstr(r, t + 16, 32);
                const uint32_t ras = r.u32(t);
                if (!r.ok(ras, 16))
                    return error = "the stock texture list is damaged", false;
                d.unk2 = r.u32(ras + 4);
                d.flags = r.u32(ras + 12);
                const uint32_t w = 1u << (d.flags & 63), h = 1u << (d.flags >> 6 & 63), depth = d.flags >> 12 & 63;
                const size_t bytes = static_cast<size_t>(w) * h * depth / 8 + (depth == 4 ? 64 : depth == 8 ? 1024 : 0);
                const uint32_t data = r.u32(ras + 8);
                if (!r.ok(data, bytes))
                    return error = "the stock texture list is damaged", false;
                d.data.assign(b.begin() + data, b.begin() + data + static_cast<long>(bytes));
                out.push_back(std::move(d));
                link = r.u32(link);
            }
            gap.assign(b.begin() + 0x38, b.begin() + 0x50);
            return true;
        }

        // The global table: numFuncs pointers, each to a function-pointer field the game's loader fills in (an
        // element's render callback: a weapon has one, at element +0x24; a car one per element). Stock chunks put it
        // right after the data, then the relocation table, then the strings.
        void finishChunk(Writer &w, uint32_t ident, const std::vector<uint32_t> &globals, uint16_t numFuncs,
                         const std::vector<std::pair<uint32_t, std::string>> &strings)
        {
            // [data][global word][relocation table][strings], as the stock chunks
            w.align(16);
            const uint32_t dataSize = static_cast<uint32_t>(w.size());
            uint32_t globalTab = 0;
            if (!globals.empty())
            {
                globalTab = static_cast<uint32_t>(w.reserve(4 * globals.size()));
                for (size_t i = 0; i < globals.size(); ++i)
                    w.ptr(globalTab + i * 4, globals[i]);
            }
            // strings go after the table: their pointers are part of it, so count first
            const size_t relocAt = w.size();
            const size_t numRelocs = w.relocs.size() + strings.size();
            w.reserve(numRelocs * 4);
            std::vector<uint32_t> stringAt;
            for (const auto &[ptrAt, s] : strings)
            {
                stringAt.push_back(static_cast<uint32_t>(w.size()));
                const size_t at = w.reserve(s.size() + 1);
                w.bytes(at, s.c_str(), s.size() + 1);
                w.put32(ptrAt, stringAt.back());
            }
            for (const auto &[ptrAt, s] : strings)
                (void)s, w.relocs.push_back(ptrAt);
            for (size_t i = 0; i < w.relocs.size(); ++i)
                w.put32(relocAt + i * 4, w.relocs[i]);
            w.put32(0x00, ident);
            w.put32(0x04, 0);
            w.put32(0x08, static_cast<uint32_t>(w.size()));
            w.put32(0x0C, dataSize);
            w.put32(0x10, static_cast<uint32_t>(relocAt));
            w.put32(0x14, static_cast<uint32_t>(w.relocs.size()));
            w.put32(0x18, globalTab);
            w.put32(0x1C, static_cast<uint32_t>(numFuncs) << 16);
        }
        std::vector<uint8_t> buildXet(const Mesh &mesh, const std::vector<uint8_t> &txd, int maxTexture,
                                      const std::vector<DonorTexture> &donorTex, const std::vector<uint8_t> &gap,
                                      std::string &report, std::vector<std::string> &matTexName, bool lcs,
                                      bool flattenGlass = false, int maxPixels = 0)
        {
            char line[200];
            // textures: the model's, resized and palettised; names kept (lower case, 31 characters at most)
            std::map<std::string, Image> pcTex;
            readTxd(txd, pcTex);
            {
                std::string names;
                for (const auto &[n, img] : pcTex)
                    names += " " + n + "(" + std::to_string(img.w) + "x" + std::to_string(img.h) + ")";
                report += "textures read from the .txd:" + (names.empty() ? std::string(" none") : names) + "\n";
            }
            struct NewTex
            {
                std::string name;
                int w, h;
                std::vector<uint8_t> indices;
                std::vector<std::array<uint8_t, 4>> palette;
                Image src;
            };
            std::vector<NewTex> newTex;
            matTexName.assign(mesh.mats.size(), std::string());
            for (size_t m = 0; m < mesh.mats.size(); ++m)
            {
                const std::string &want = mesh.mats[m].texture;
                if (want.empty())
                    continue;
                auto it = pcTex.find(want);
                if (it == pcTex.end())
                {
                    report += "texture '" + want + "' is not in the .txd: drawn untextured\n";
                    continue;
                }
                std::string name = want.substr(0, 31);
                matTexName[m] = name;
                bool have = false;
                for (const NewTex &t : newTex)
                    have |= t.name == name;
                if (have)
                    continue;
                Image src = it->second;
                // [lcs] a vehicle's glass (a see-through material: alpha < 250, as SA's 242 windows): the car pipeline
                // draws its textures opaque (a stock car texture's alpha is not transparency: 3..250 in the Sentinel's),
                // so a glass texture's dirt spots showed as dark squares on clear glass. Laid over a dark tint instead,
                // the way LCS's own car windows look.
                bool glass = false;
                for (const Material &mm : mesh.mats)
                    glass |= flattenGlass && mm.texture == want && mm.rgba[3] < 250;
                if (glass)
                {
                    for (size_t i = 0; i + 3 < src.rgba.size(); i += 4)
                    {
                        const float a = src.rgba[i + 3] / 255.0f * 0.2f; // (0.6 left the dirt spots as dark patches)
                        const uint8_t tint[3] = {22, 27, 34};
                        for (int k = 0; k < 3; ++k)
                            src.rgba[i + k] = static_cast<uint8_t>(src.rgba[i + k] * a + tint[k] * (1.0f - a));
                        src.rgba[i + 3] = 255;
                    }
                    report += "texture " + name + ": glass, laid over a dark tint (drawn opaque)\n";
                }
                NewTex t;
                t.name = name;
                t.w = pow2Below(src.w, maxTexture);
                t.h = pow2Below(src.h, maxTexture);
                t.src = std::move(src);
                newTex.push_back(std::move(t));
            }
            // a budget for all of them (vehicles: as much as ten 128 x 128 textures): the largest halved until it fits
            for (int guard = 0; maxPixels > 0 && guard < 256; ++guard)
            {
                size_t total = 0, big = 0;
                for (size_t i = 0; i < newTex.size(); ++i)
                {
                    total += static_cast<size_t>(newTex[i].w) * newTex[i].h;
                    if (newTex[i].w * newTex[i].h > newTex[big].w * newTex[big].h)
                        big = i;
                }
                if (newTex.empty() || total <= static_cast<size_t>(maxPixels) || newTex[big].w * newTex[big].h <= 32 * 32)
                    break;
                NewTex &b = newTex[big];
                (b.w >= b.h ? b.w : b.h) /= 2;
            }
            for (NewTex &t : newTex)
            {
                const Image small = resize(t.src, t.w, t.h);
                palettise(small, t.indices, t.palette);
                std::snprintf(line, sizeof(line), "texture %s: %dx%d -> %dx%d, 8-bit palette\n", t.name.c_str(), t.src.w, t.src.h, t.w, t.h);
                report += line;
                t.src = Image{};
            }

            // ---- the texture list (xet) ----
            {
                Writer w;
                w.reserve(0x20);
                const size_t list = w.reserve(24);
                w.b[list] = 6;
                w.b[list + 1] = lcs ? 0 : 0x86; // [lcs] LCS's stock lists: 0
                w.put32(list + 16, 1); // lInInstance, as stock
                // [lcs] LCS: the texels start at +0x40, as in every stock list (a texture 16 bytes off drew as noise:
                // the pistol's HUD icon in a run)
                const size_t gapSize = lcs ? 0x08 : 0x18;
                const size_t gapAt = w.reserve(gapSize);
                w.bytes(gapAt, gap.data(), std::min<size_t>(gap.size(), gapSize));
                struct Placed
                {
                    std::string name;
                    uint32_t unk2, flags, data;
                };
                std::vector<Placed> placed;
                // donor textures first (HUD icons etc.), skipping names the model brings itself
                for (const DonorTexture &d : donorTex)
                {
                    bool replaced = false;
                    for (const NewTex &t : newTex)
                        replaced |= lower(d.name) == t.name;
                    if (replaced)
                        continue;
                    w.align(16);
                    const size_t at = w.reserve(d.data.size());
                    w.bytes(at, d.data.data(), d.data.size());
                    placed.push_back({d.name, d.unk2, d.flags, static_cast<uint32_t>(at)});
                }
                for (const NewTex &t : newTex)
                {
                    w.align(16);
                    const size_t at = w.reserve(static_cast<size_t>(t.w) * t.h + 1024);
                    // texels: the CLUT is CSM1-ordered, so index bits 3 and 4 are swapped (as the game's own 8-bit textures)
                    for (size_t i = 0; i < t.indices.size(); ++i)
                    {
                        const uint8_t x = t.indices[i];
                        w.b[at + i] = static_cast<uint8_t>((x & ~0x18) | ((x & 0x08) << 1) | ((x & 0x10) >> 1));
                    }
                    const size_t pal = at + static_cast<size_t>(t.w) * t.h;
                    for (int i = 0; i < 256; ++i)
                    {
                        const auto &c = t.palette[static_cast<size_t>(i)];
                        w.b[pal + i * 4 + 0] = c[0];
                        w.b[pal + i * 4 + 1] = c[1];
                        w.b[pal + i * 4 + 2] = c[2];
                        w.b[pal + i * 4 + 3] = static_cast<uint8_t>((c[3] * 128 + 127) / 255);
                    }
                    int lw = 0, lh = 0;
                    while ((1 << lw) < t.w)
                        ++lw;
                    while ((1 << lh) < t.h)
                        ++lh;
                    // [lcs] bits 20-23: the number of levels (1); bits 24 + k: level k is stored PSMT8-swizzled (the
                    // stock pistol's two 64 x 64 textures: 0x1108186 / 0x1104186, swizzled). The texels written here
                    // are linear: no swizzle bit ([crossover] with 0x11 the game read them swizzled: a VCS PCJ-600's
                    // wheels and engine drew as noise, hidden run t1)
                    const uint32_t flags = static_cast<uint32_t>(lw) | static_cast<uint32_t>(lh) << 6 | 8u << 12 | 1u << 20;
                    placed.push_back({t.name, 0x250000u | static_cast<uint32_t>(t.w), flags, static_cast<uint32_t>(at)});
                }
                // textures (80 bytes) then rasters (16 bytes)
                w.align(16);
                std::vector<size_t> texAt, rasAt;
                // [lcs] LCS's texture is 0x54 bytes, its +0x50 = 1 in every stock one (the pistol's two, the KURUMA's);
                // without it the pistol's HUD icon drew as noise in a run. [crossover] VCS's is 0x60 bytes with +0x50 = 1
                // (Vic's txd, agent hero): with 0x50 VCS drew only the list's first texture and the others showed its
                // texels - the "mottled" VCS mod cars
                for (size_t i = 0; i < placed.size(); ++i)
                {
                    texAt.push_back(w.reserve(lcs ? 0x54 : 0x60));
                    w.put32(texAt.back() + 0x50, 1);
                }
                for (size_t i = 0; i < placed.size(); ++i)
                    rasAt.push_back(w.reserve(lcs ? 8 : 16));
                for (size_t i = 0; i < placed.size(); ++i)
                {
                    const size_t t = texAt[i], ras = rasAt[i];
                    w.ptr(t, static_cast<uint32_t>(ras));
                    w.ptr(t + 4, static_cast<uint32_t>(list));
                    // the list: head.next -> first texture's link, each link -> the next, the last -> the head
                    w.ptr(t + 8, static_cast<uint32_t>(i + 1 < placed.size() ? texAt[i + 1] + 8 : list + 8));
                    w.ptr(t + 12, static_cast<uint32_t>(i > 0 ? texAt[i - 1] + 8 : list + 8));
                    std::memcpy(w.b.data() + t + 16, placed[i].name.c_str(), std::min<size_t>(placed[i].name.size(), 31));
                    if (lcs) // [lcs] {texels, flags}
                    {
                        w.ptr(ras, placed[i].data);
                        w.put32(ras + 4, placed[i].flags);
                        continue;
                    }
                    w.put32(ras + 4, placed[i].unk2);
                    w.ptr(ras + 8, placed[i].data);
                    w.put32(ras + 12, placed[i].flags);
                }
                if (!placed.empty())
                {
                    w.ptr(list + 8, static_cast<uint32_t>(texAt.front() + 8));
                    w.ptr(list + 12, static_cast<uint32_t>(texAt.back() + 8));
                }
                else
                {
                    w.ptr(list + 8, static_cast<uint32_t>(list + 8));
                    w.ptr(list + 12, static_cast<uint32_t>(list + 8));
                }
                finishChunk(w, 0x00746578u, {}, 0, {});
                return std::move(w.b);
            }

        }

        struct GeomOut
        {
            size_t geom = 0;
            std::vector<size_t> matAt; // each material's offset
            std::vector<int> mats;     // the mesh material each one is
            uint32_t totalVerts = 0;
            size_t strips = 0;
        };

        // One RslGeometry (header, sPs2Geometry, meshes, one DMA packet per material) and its materials, appended
        // to `w`. `style` fills a material's colour / effects (texture names go to `strings`, written after the
        // relocation table as stock). The vertices are in the space of the node the geometry hangs from.
        template <typename Style>
        bool writeGeometry(Writer &w, const Mesh &mesh, const std::vector<std::string> &matTexName, uint32_t meshUnknown,
                           std::vector<std::pair<uint32_t, std::string>> &strings, Style style, GeomOut &g, std::string &error)
        {
            // group the triangles by material, strip each group
            std::vector<int> usedMats;
            for (const Tri &t : mesh.tris)
                if (std::find(usedMats.begin(), usedMats.end(), t.mat) == usedMats.end())
                    usedMats.push_back(t.mat);
            std::vector<std::vector<uint32_t>> strips;
            for (int m : usedMats)
            {
                std::vector<Tri> group;
                for (const Tri &t : mesh.tris)
                    if (t.mat == m)
                        group.push_back(t);
                strips.push_back(stripify(group));
            }
            // quantisation: the geometry's centre and power-of-two half extents (as stock)
            V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
            for (const auto &s : strips)
                for (uint32_t k : s)
                {
                    const V3 p = mesh.verts[k].p;
                    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
                }
            const V3 centre = (lo + hi) * 0.5f;
            float scale[3];
            const float halfExt[3] = {(hi.x - lo.x) * 0.5f, (hi.y - lo.y) * 0.5f, (hi.z - lo.z) * 0.5f};
            for (int k = 0; k < 3; ++k)
            {
                scale[k] = 1.0f / 1024.0f;
                while (scale[k] < halfExt[k])
                    scale[k] *= 2.0f;
            }
            auto quant = [&](float v, float c, float s) {
                return static_cast<int16_t>(std::clamp(std::lround((v - c) / s * 32767.5f), -32767l, 32767l));
            };
            float radius = 0;
            for (const auto &s : strips)
                for (uint32_t k : s)
                    radius = std::max(radius, len(mesh.verts[k].p - centre));

            const size_t matArray = w.reserve(4 * usedMats.size());
            for (size_t i = 0; i < usedMats.size(); ++i)
            {
                const size_t at = w.reserve(16);
                w.ptr(matArray + i * 4, static_cast<uint32_t>(at));
                const std::string &tex = matTexName[static_cast<size_t>(usedMats[i])];
                if (!tex.empty())
                    strings.push_back({static_cast<uint32_t>(at), tex});
                w.put32(at + 8, 2); // reference count, 2 as stock (the chunk owns the material)
                style(w, at, usedMats[i]);
                g.matAt.push_back(at);
                g.mats.push_back(usedMats[i]);
            }
            w.align(16);
            const size_t geom = w.reserve(0x20);
            // geometry: type 8, refcount 1, material list
            w.b[geom] = 8;
            w.put16(geom + 8, 1);
            w.ptr(geom + 12, static_cast<uint32_t>(matArray));
            w.put32(geom + 16, static_cast<uint32_t>(usedMats.size()));
            w.put32(geom + 20, static_cast<uint32_t>(usedMats.size()));
            w.put32(geom + 28, 0xAAAAAAAAu);
            const size_t ps2 = w.reserve(0x40);
            const size_t meshes = w.reserve(0x30 * strips.size());
            const size_t dma = w.size();
            uint32_t totalVerts = 0;
            int16_t gmin[3] = {32767, 32767, 32767}, gmax[3] = {-32767, -32767, -32767};
            for (size_t s = 0; s < strips.size(); ++s)
            {
                const std::vector<uint32_t> &st = strips[s];
                const size_t m = meshes + s * 0x30;
                const size_t tag = w.reserve(16);
                int16_t mn[3] = {32767, 32767, 32767}, mx[3] = {-32767, -32767, -32767};
                // UVs: shifted by whole tiles to start at 0 (the texture repeats), scaled into 0..255
                float umin = 1e30f, vmin = 1e30f;
                for (uint32_t k : st)
                    umin = std::min(umin, mesh.verts[k].u), vmin = std::min(vmin, mesh.verts[k].v);
                umin = std::floor(umin);
                vmin = std::floor(vmin);
                float umax = 0.0f, vmax = 0.0f;
                for (uint32_t k : st)
                    umax = std::max(umax, mesh.verts[k].u - umin), vmax = std::max(vmax, mesh.verts[k].v - vmin);
                const float uvScale[2] = {std::max(0.5f, umax / 2.0f), std::max(0.5f, vmax / 2.0f)};
                for (size_t first = 0; first + 2 < st.size() || first == 0; first += 68)
                {
                    const size_t n = std::min<size_t>(70, st.size() - first);
                    if (n < 3)
                        break;
                    const size_t at = w.reserve(20);
                    w.put32(at, 0x6C018000u);
                    w.put32(at + 12, static_cast<uint32_t>(n));
                    w.put32(at + 16, 0x8000u | static_cast<uint32_t>(n));
                    auto mask = [&](uint32_t m2, uint32_t row2) {
                        const size_t a = w.reserve(28);
                        w.put32(a, 0x20000000u);
                        w.put32(a + 4, m2);
                        w.put32(a + 8, 0x30000000u);
                        w.put32(a + 20, row2);
                    };
                    mask(0x40404040u, 0);
                    size_t u = w.reserve(4 + ((n * 6 + 3) & ~size_t(3)));
                    w.put32(u, 0x79008001u | static_cast<uint32_t>(n) << 16);
                    for (size_t i = 0; i < n; ++i)
                    {
                        const V3 p = mesh.verts[st[first + i]].p;
                        const int16_t q[3] = {quant(p.x, centre.x, scale[0]), quant(p.y, centre.y, scale[1]), quant(p.z, centre.z, scale[2])};
                        for (int k = 0; k < 3; ++k)
                        {
                            w.put16(u + 4 + i * 6 + k * 2, static_cast<uint16_t>(q[k]));
                            mn[k] = std::min(mn[k], q[k]), mx[k] = std::max(mx[k], q[k]);
                        }
                    }
                    mask(0x50505050u, 0xFF);
                    u = w.reserve(4 + ((n * 2 + 3) & ~size_t(3)));
                    w.put32(u, 0x7600C08Du | static_cast<uint32_t>(n) << 16);
                    for (size_t i = 0; i < n; ++i)
                    {
                        const Vertex &v = mesh.verts[st[first + i]];
                        w.b[u + 4 + i * 2] = static_cast<uint8_t>(std::clamp(std::lround((v.u - umin) / uvScale[0] * 127.5f), 0l, 255l));
                        w.b[u + 5 + i * 2] = static_cast<uint8_t>(std::clamp(std::lround((v.v - vmin) / uvScale[1] * 127.5f), 0l, 255l));
                    }
                    u = w.reserve(4 + ((n * 3 + 3) & ~size_t(3)));
                    w.put32(u, 0x6A008047u | static_cast<uint32_t>(n) << 16);
                    for (size_t i = 0; i < n; ++i)
                    {
                        const V3 nn = mesh.verts[st[first + i]].n;
                        const float c[3] = {nn.x, nn.y, nn.z};
                        for (int k = 0; k < 3; ++k)
                            w.b[u + 4 + i * 3 + k] = static_cast<uint8_t>(static_cast<int8_t>(std::clamp(std::lround(c[k] * 127.0f), -127l, 127l)));
                    }
                    const size_t call = w.reserve(4);
                    w.put32(call, 0x14000006u);
                    w.align(16);
                    if (first + n >= st.size())
                        break;
                }
                const uint32_t qwc = static_cast<uint32_t>((w.size() - tag) / 16 - 1);
                w.put32(tag, 0x60000000u | qwc);
                // the mesh header
                V3 c{0, 0, 0};
                for (uint32_t k : st)
                    c = c + mesh.verts[k].p;
                c = c * (1.0f / static_cast<float>(st.size()));
                float r = 0;
                for (uint32_t k : st)
                    r = std::max(r, len(mesh.verts[k].p - c));
                w.putf(m, c.x), w.putf(m + 4, c.y), w.putf(m + 8, c.z), w.putf(m + 12, r);
                w.putf(m + 16, uvScale[0]), w.putf(m + 20, uvScale[1]);
                w.put32(m + 24, meshUnknown);
                w.put32(m + 28, static_cast<uint32_t>(tag - dma));
                w.put16(m + 32, static_cast<uint16_t>(st.size() - 2));
                w.put16(m + 34, static_cast<uint16_t>(s));
                for (int k = 0; k < 3; ++k)
                {
                    w.put16(m + 36 + k * 2, static_cast<uint16_t>(mn[k]));
                    w.put16(m + 42 + k * 2, static_cast<uint16_t>(mx[k]));
                    gmin[k] = std::min(gmin[k], mn[k]), gmax[k] = std::max(gmax[k], mx[k]);
                }
                totalVerts += static_cast<uint32_t>(st.size());
            }
            // sPs2Geometry
            w.putf(ps2, centre.x), w.putf(ps2 + 4, centre.y), w.putf(ps2 + 8, centre.z), w.putf(ps2 + 12, radius);
            w.put32(ps2 + 16, static_cast<uint32_t>(strips.size()) << 20 | static_cast<uint32_t>(w.size() - ps2));
            w.put32(ps2 + 20, 7); // positions, normals, texture coordinates
            w.put16(ps2 + 24, static_cast<uint16_t>(std::min<uint32_t>(totalVerts, 0xFFFF)));
            w.put16(ps2 + 26, static_cast<uint16_t>(dma - ps2));
            for (int k = 0; k < 3; ++k)
            {
                w.put16(ps2 + 28 + k * 2, static_cast<uint16_t>(gmin[k]));
                w.put16(ps2 + 34 + k * 2, static_cast<uint16_t>(gmax[k]));
            }
            w.putf(ps2 + 40, scale[0]), w.putf(ps2 + 44, scale[1]), w.putf(ps2 + 48, scale[2]);
            w.putf(ps2 + 52, centre.x), w.putf(ps2 + 56, centre.y), w.putf(ps2 + 60, centre.z);
            if (w.size() - ps2 >= (1u << 20))
                return error = "a converted geometry is over 1 MB", false;
            g.geom = geom;
            g.totalVerts = totalVerts;
            g.strips = strips.size();
            return true;
        }

    }

    bool convertSimple(const std::vector<uint8_t> &donorMdl, const std::vector<uint8_t> &donorXet,
                       const std::vector<uint8_t> &dff, const std::vector<uint8_t> &txd, int modelId,
                       const Options &opt, Result &out, std::string &error)
    {
        Donor donor;
        std::vector<DonorTexture> donorTex;
        std::vector<uint8_t> gap;
        if (!readDonorMdl(donorMdl, donor, error) || !readDonorXet(donorXet, donorTex, gap, error, opt.lcs))
            return false;
        Mesh mesh;
        if (!readDff(dff, mesh, out.report, error, lower(opt.part)))
            return false;
        if (opt.scale != 1.0f)
            for (Vertex &v : mesh.verts)
                v.p = v.p * opt.scale;
        char line[200];
        if (static_cast<int>(mesh.tris.size()) > opt.maxTriangles)
        {
            const size_t before = mesh.tris.size();
            decimate(mesh, opt.maxTriangles);
            std::snprintf(line, sizeof(line), "decimated %zu -> %zu triangles (budget %d)\n", before, mesh.tris.size(), opt.maxTriangles);
            out.report += line;
        }
        std::vector<std::string> matTexName;
        out.xet = buildXet(mesh, txd, opt.maxTexture, donorTex, gap, out.report, matTexName, opt.lcs);

        Writer w;
        w.reserve(0x20);
        const size_t elemArray = w.reserve(4);
        w.align(8);
        const size_t elem = w.reserve(0x30);
        w.bytes(elem, donor.element.data(), 0x30);
        w.align(16);
        const size_t node = w.reserve(0xB0);
        w.bytes(node, donor.node.data(), 0xB0);
        std::vector<std::pair<uint32_t, std::string>> strings;
        GeomOut g;
        // the stock weapons' lighting colour, tinted by the PC material's colour
        auto style = [&](Writer &ww, size_t at, int mat) {
            const uint8_t *pc = mesh.mats[static_cast<size_t>(mat)].rgba;
            for (int k = 0; k < 3; ++k)
                ww.b[at + 4 + k] = static_cast<uint8_t>(donor.matColour[k] * pc[k] / 255);
            ww.b[at + 7] = 0xFF;
        };
        if (!writeGeometry(w, mesh, matTexName, donor.meshUnknown, strings, style, g, error))
            return false;
        w.ptr(elemArray, static_cast<uint32_t>(elem));
        // element: node (+4), its link in the node's object list (+8 / +12), geometry (+20), model id (+40)
        w.ptr(elem + 4, static_cast<uint32_t>(node));
        w.ptr(elem + 8, static_cast<uint32_t>(node + 8));
        w.ptr(elem + 12, static_cast<uint32_t>(node + 8));
        w.put32(elem + 16, 0);
        w.ptr(elem + 20, static_cast<uint32_t>(g.geom));
        w.put32(elem + 24, 0);
        w.put16(elem + 40, static_cast<uint16_t>(modelId));
        // node: object list -> the element's link (+8 / +12), root = itself (+0x98), no child / sibling
        w.put32(node + 4, 0);
        w.ptr(node + 8, static_cast<uint32_t>(elem + 8));
        w.ptr(node + 12, static_cast<uint32_t>(elem + 8));
        w.put32(node + 0x90, 0);
        w.put32(node + 0x94, 0);
        w.ptr(node + 0x98, static_cast<uint32_t>(node));
        finishChunk(w, 0x006D646Cu, {static_cast<uint32_t>(elem + 0x24)}, 1, strings);
        out.mdl = std::move(w.b);
        std::snprintf(line, sizeof(line), "written: %zu triangles in %zu strip(s), %u strip vertices, model %zu bytes, textures %zu bytes\n",
                      mesh.tris.size(), g.strips, g.totalVerts, out.mdl.size(), out.xet.size());
        out.report += line;
        return true;
    }


    // ---- vehicles -------------------------------------------------------------------------------------------------------
    //
    // A stock vehicle chunk (measured on STINGER, index 254): the data starts with the vehicle data {element group,
    // number of extras, extras, 30 primary-colour materials, 25 secondary-colour materials}; the element group's node
    // tree carries VC's names (chassis_dummy > chassis_hi / chassis_vlo / door_lf_dummy / bump_front_dummy...,
    // wheel_*_dummy) and each part node holds two elements: flag 4 = the intact part, 0 = the damaged one. Wheels are
    // not in the car (no element under wheel_*_dummy). The converted car keeps the whole stock chunk - nodes, elements,
    // dummies, wheels, collision and the model info's lights and seats stay the slot's - and gets new geometries for
    // its elements, from the PC parts of the same names, appended after it.

    bool convertVehicle(const std::vector<uint8_t> &donorMdl, const std::vector<uint8_t> &donorXet,
                        const std::vector<uint8_t> &dff, const std::vector<uint8_t> &txd, int modelId,
                        const Options &opt, Result &out, std::string &error)
    {
        (void)modelId;
        Reader d{donorMdl.data(), donorMdl.size()};
        if (d.u32(0) != 0x006D646Cu)
            return error = "the stock model is not an MDL chunk", false;
        const uint32_t fileSize = d.u32(8), relocTab = d.u32(0x10), numRelocs = d.u32(0x14), globalTab = d.u32(0x18);
        const uint16_t numFuncs = d.u16(0x1E);
        if (fileSize > donorMdl.size() || !d.ok(relocTab, numRelocs * 4u) || !d.ok(globalTab, 4u * numFuncs))
            return error = "the stock model's header is damaged", false;
        const uint32_t group = d.u32(0x20);
        if (!d.ok(group, 16) || d.p[group] != 2)
            return error = "the stock model is not a vehicle (no element group)", false;
        std::vector<DonorTexture> donorTex;
        std::vector<uint8_t> gap;
        if (!readDonorXet(donorXet, donorTex, gap, error, opt.lcs))
            return false;

        // the donor's nodes and elements
        struct DonorElement
        {
            uint32_t at, node;
            bool intact;
            std::string nodeName;
            V3 nodePos; // in the vehicle's space (the root node's own placement left out)
            int nodeIndex = -1;
        };
        std::vector<DonorElement> elements;
        // every node (frame) of the stock tree: +0x40 its position in its parent's space (the matrix's last row, as
        // RenderWare's RwFrame: right +0x10, up +0x20, at +0x30, pos +0x40), +0x90 first child, +0x94 next sibling
        // A node's rotation (its right / up / at rows) composed down the tree: the stock cars' nodes are all unrotated,
        // a bike's are not (LCS's Faggio: forks_front / handlebars tilted 0.1 rad, wheel_front tilted back, chassis_vlo
        // turned 90 degrees), so a part's vertices go into its node's space by the whole matrix, not the position alone.
        struct Rot
        {
            V3 r{1, 0, 0}, u{0, 1, 0}, a{0, 0, 1}; // the node's axes in the vehicle's space
            V3 apply(V3 p) const { return r * p.x + u * p.y + a * p.z; }     // node space -> vehicle space
            V3 inverse(V3 p) const { return {dot(p, r), dot(p, u), dot(p, a)}; } // vehicle space -> node space
        };
        struct DonorNode
        {
            uint32_t at;
            int parent;
            std::string name;
            V3 world;
            Rot rot;
        };
        std::vector<DonorNode> nodes;
        std::vector<std::vector<size_t>> nodeElements; // per node: indices into elements
        uint32_t meshUnknown = 0;
        std::function<void(uint32_t, V3, Rot, int, bool, int)> walk = [&](uint32_t n, V3 parent, Rot parentRot, int depth,
                                                                         bool root, int up) {
            for (int guard = 0; n && d.ok(n, 0xB0) && depth < 32 && guard < 256; ++guard)
            {
                const V3 pos = root ? V3{0, 0, 0} : parent + parentRot.apply(V3{d.f32(n + 0x40), d.f32(n + 0x44), d.f32(n + 0x48)});
                Rot rot;
                if (!root)
                {
                    auto row = [&](uint32_t off) { return parentRot.apply(V3{d.f32(n + off), d.f32(n + off + 4), d.f32(n + off + 8)}); };
                    rot.r = row(0x10), rot.u = row(0x20), rot.a = row(0x30);
                    if (std::fabs(len(rot.r) - 1) > 0.1f || std::fabs(len(rot.u) - 1) > 0.1f || std::fabs(len(rot.a) - 1) > 0.1f)
                        rot = parentRot; // not a rotation (never seen): treated as none
                }
                const uint32_t nameAt = d.u32(n + (opt.lcs ? 0xA4u : 0xA8u)); // [lcs] LCS +0xA4 (its KURUMA: chassis_dummy, chassis_hi...)
                const std::string name = nameAt && d.ok(nameAt, 1) ? lower(cstr(d, nameAt, 32)) : "";
                const int self = static_cast<int>(nodes.size());
                nodes.push_back({n, up, name, pos, rot});
                nodeElements.emplace_back();
                uint32_t link = d.u32(n + 8);
                for (int k = 0; link && link != n + 8 && d.ok(link, 8) && k < 16; ++k)
                {
                    const uint32_t e = link - 8;
                    if (d.ok(e, 0x30) && d.p[e] == 1)
                    {
                        nodeElements[static_cast<size_t>(self)].push_back(elements.size());
                        elements.push_back({e, n, (d.p[e + 2] & 4) != 0, name, pos, self});
                        const uint32_t g = d.u32(e + 20);
                        if (!meshUnknown && d.ok(g + 0x60, 0x30))
                            meshUnknown = d.u32(g + 0x60 + 24);
                    }
                    link = d.u32(link);
                }
                walk(d.u32(n + 0x90), pos, rot, depth + 1, false, self);
                n = d.u32(n + 0x94);
                root = false;
                if (depth == 0)
                    break; // the root has no siblings
            }
        };
        walk(d.u32(group + 4), {0, 0, 0}, Rot{}, 0, true, -1);
        if (elements.empty())
            return error = "the stock vehicle has no parts", false;
        {
            std::string names;
            for (const DonorNode &n : nodes)
                names += (names.empty() ? "" : " ") + (n.name.empty() ? std::string("?") : n.name);
            out.report += "the slot's nodes: " + names + "\n";
        }
        // the paint's environment map: the first stock primary-colour material's effect, reused for the new paint
        uint32_t paintFx = 0;
        if (const uint32_t m = d.u32(0x2C); m && d.ok(m, 16))
            paintFx = d.u32(m + 12);

        // the PC parts, by name
        std::vector<Frame> frames;
        std::vector<Part> parts;
        if (!readDffParts(dff, frames, parts, error))
            return false;
        Mesh chassis, vlo;
        std::map<std::string, Mesh> intact, damaged;
        int pcTris = 0, skipped = 0;
        auto ends = [](const std::string &s, const char *suffix) {
            const size_t n = std::strlen(suffix);
            return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
        };
        // Part names (gtamods.com wiki, vehicle models): GTA III / VC <part>_hi_ok / <part>_hi_dam under <part>_dummy,
        // SA <part>_ok / <part>_dam; the wheel geometry under wheel_rf_dummy (or a frame named "wheel*"); extras
        // extra1..6; chassis_hi (SA: chassis) the body, chassis_vlo the far model.
        // A part named as one of the slot's own part nodes (a bike's forks_front, handlebars, mudguard, wheel_front,
        // forks_rear, wheel_rear: each its own element there, steering / spinning with its node) goes onto that node.
        std::set<std::string> ownNodes;
        for (const DonorElement &e : elements)
            if (!ends(e.nodeName, "_dummy") && e.nodeName != "chassis_hi" && e.nodeName != "chassis_vlo" && !e.nodeName.empty() &&
                e.nodeName != "boat_hi" && e.nodeName != "boat_vlo") // (a boat's body: from the mod's body, below)
                ownNodes.insert(e.nodeName);
        std::map<std::string, Mesh> named;
        std::vector<std::string> skippedNames;
        Mesh wheel;
        for (const Part &p : parts)
        {
            const std::string &n = p.name;
            if (ownNodes.count(n))
                appendMesh(named[n], p.mesh, {0, 0, 0}), pcTris += static_cast<int>(p.mesh.tris.size());
            else if (n.find("wheel") != std::string::npos)
                appendMesh(wheel, p.mesh, {0, 0, 0}), ++skipped;
            else if (n.rfind("extra", 0) == 0)
                skippedNames.push_back(n), ++skipped;
            else if (ends(n, "_hi_ok"))
                appendMesh(intact[n.substr(0, n.size() - 6)], p.mesh, {0, 0, 0}), pcTris += static_cast<int>(p.mesh.tris.size());
            else if (ends(n, "_hi_dam"))
                appendMesh(damaged[n.substr(0, n.size() - 7)], p.mesh, {0, 0, 0});
            else if (ends(n, "_ok") && n.find("_lo") == std::string::npos && n.find("_vlo") == std::string::npos) // SA
                appendMesh(intact[n.substr(0, n.size() - 3)], p.mesh, {0, 0, 0}), pcTris += static_cast<int>(p.mesh.tris.size());
            else if (ends(n, "_dam") && n.find("_lo") == std::string::npos && n.find("_vlo") == std::string::npos) // SA
                appendMesh(damaged[n.substr(0, n.size() - 4)], p.mesh, {0, 0, 0});
            else if (n == "chassis_vlo" || n == "boat_vlo")
                appendMesh(vlo, p.mesh, {0, 0, 0});
            else if (lowDetail(n))
                ++skipped;
            else // chassis_hi, and parts named otherwise (body panels of some mods): the body
                appendMesh(chassis, p.mesh, {0, 0, 0}), pcTris += static_cast<int>(p.mesh.tris.size());
        }
        if (chassis.tris.empty())
            return error = "the model has no body (no chassis_hi or other undamaged part)", false;
        // a part the slot has no element for (LCS's Cabbie has wing_lf_dummy / wing_rf_dummy nodes but no wing elements:
        // its wings are in its body) goes into the body, intact, so the car has no hole there (it then dents with the body)
        {
            std::set<std::string> slotParts;
            for (const DonorElement &e : elements)
                slotParts.insert(ends(e.nodeName, "_dummy") ? e.nodeName.substr(0, e.nodeName.size() - 6) : e.nodeName);
            std::string merged;
            for (auto it = intact.begin(); it != intact.end();)
                if (!slotParts.count(it->first))
                {
                    appendMesh(chassis, it->second, {0, 0, 0});
                    merged += " " + it->first;
                    damaged.erase(it->first);
                    it = intact.erase(it);
                }
                else
                    ++it;
            if (!merged.empty())
                out.report += "parts the slot has no element for, drawn with the body:" + merged + "\n";
        }
        // the mod's own wheel, in its frame's space: the wheel part under wheel_rf_dummy (else the first wheel part),
        // centred on its axle and scaled to 1 m high (the stock wheel models' size; the wheel scale gives its size)
        Mesh ownWheel;
        {
            std::string pick;
            for (const Part &p : parts)
            {
                if (p.name.find("wheel") == std::string::npos || ends(p.name, "_dummy") || ownNodes.count(p.name) ||
                    p.mesh.tris.empty() || lowDetail(p.name))
                    continue;
                bool rf = false;
                for (int f = p.frame, guard = 0; f >= 0 && f < static_cast<int>(frames.size()) && guard < 64;
                     f = frames[static_cast<size_t>(f)].parent, ++guard)
                    rf |= frames[static_cast<size_t>(f)].name == "wheel_rf_dummy";
                if (pick.empty() || rf)
                    pick = p.name;
                if (rf)
                    break;
            }
            std::vector<Frame> f2;
            std::vector<Part> p2;
            std::string e2;
            // [lcs] LCS only: VCS's shared-wheel field in the model info is not located, so its car would draw both
            if (opt.lcs && !pick.empty() && readDffParts(dff, f2, p2, e2, pick))
            {
                for (const Part &p : p2)
                    if (!lowDetail(p.name))
                        appendMesh(ownWheel, p.mesh, {0, 0, 0});
                V3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
                for (const Vertex &v : ownWheel.verts)
                    lo = {std::min(lo.x, v.p.x), std::min(lo.y, v.p.y), std::min(lo.z, v.p.z)},
                    hi = {std::max(hi.x, v.p.x), std::max(hi.y, v.p.y), std::max(hi.z, v.p.z)};
                const float h = hi.z - lo.z;
                if (ownWheel.tris.empty() || h < 0.2f || h > 3.0f)
                    ownWheel = Mesh{};
                else
                {
                    const V3 c{0, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
                    for (Vertex &v : ownWheel.verts)
                        v.p = (v.p - c) * (1.0f / h);
                    if (static_cast<int>(ownWheel.tris.size()) > 400)
                        decimate(ownWheel, 400);
                }
            }
        }
        char line[240];

        // The dummies: a stock node whose name the mod has as a frame (door_lf_dummy, wheel_rf_dummy, bonnet_dummy,
        // boot_dummy, bump_front_dummy, windscreen_dummy ...) is moved to the mod's position, so doors swing on the
        // mod's hinges and wheels sit where the mod puts them (the wheel's physics follows its node, as on the PC).
        std::map<std::string, V3> modFrame;
        for (size_t i = 0; i < frames.size(); ++i)
            if (!frames[i].name.empty() && !modFrame.count(frames[i].name))
                modFrame[frames[i].name] = xform(frames, static_cast<int>(i), {0, 0, 0}, false);
        int moved = 0;
        std::vector<V3> newWorld(nodes.size());
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            newWorld[i] = nodes[i].world;
            const DonorNode &n = nodes[i];
            if (n.parent >= 0 && (ends(n.name, "_dummy") || ownNodes.count(n.name)) && n.name != "chassis_dummy" &&
                modFrame.count(n.name))
            {
                newWorld[i] = modFrame[n.name];
                const V3 dlt = newWorld[i] - n.world;
                if (std::fabs(dlt.x) + std::fabs(dlt.y) + std::fabs(dlt.z) > 0.001f)
                    ++moved;
            }
        }
        for (size_t i = 0; i < nodes.size(); ++i)
            for (size_t k : nodeElements[i])
                elements[k].nodePos = newWorld[i];
        std::string positions; // "set ..." lines: read by vcs_mods.cpp (model info values), cached with the report
        int dummiesFound = 0;
        {
            // the model info's positions (LCS: +0x50 + k x 0x10; vcs_mods.cpp kLcsCarPositions), from the PC dummies of
            // the same meaning; lights and seats mirrored, so their x is taken positive as the stock ones are
            const char *names[5] = {"headlights", "taillights", "ped_frontseat", "ped_backseat", "exhaust"};
            for (int k = 0; k < 5; ++k)
                if (modFrame.count(names[k]))
                {
                    V3 p = modFrame[names[k]];
                    if (k < 4)
                        p.x = std::fabs(p.x);
                    std::snprintf(line, sizeof(line), "set position %d %.4f %.4f %.4f (%s)\n", k, p.x, p.y, p.z, names[k]);
                    positions += line;
                    ++dummiesFound;
                }
            // the wheel's size: its height in the vehicle's space (= the IDE wheel scale: the stock wheel models are
            // 1 m across; the Kuruma's model info has 0.7, VC's IDE value for it)
            if (!wheel.verts.empty())
            {
                float lo = 1e9f, hi = -1e9f;
                for (const Vertex &v : wheel.verts)
                    lo = std::min(lo, v.p.z), hi = std::max(hi, v.p.z);
                if (hi - lo > 0.2f && hi - lo < 3.0f)
                {
                    std::snprintf(line, sizeof(line), "set wheelscale %.3f (the mod's wheel is %.2f m high)\n", hi - lo, hi - lo);
                    positions += line;
                }
            }
        }
        std::snprintf(line, sizeof(line), "dummies: %d stock nodes moved to the mod's frames; %d light / seat / exhaust positions\n",
                      moved, dummiesFound);
        out.report += line;
        if (!named.empty())
        {
            std::string s;
            for (const auto &[n, m] : named)
                s += " " + n;
            out.report += "the slot's own parts from the mod's parts of the same name:" + s + "\n";
        }
        if (!wheel.verts.empty() && ownWheel.tris.empty())
            out.report += "wheels: the slot's wheel model is kept (it is shared by other cars), sized to the mod's wheel\n";
        if (!skippedNames.empty())
        {
            std::string s;
            for (const std::string &n : skippedNames)
                s += " " + n;
            out.report += "not converted (the slot's extras stay):" + s + "\n";
        }
        std::snprintf(line, sizeof(line), "vehicle: %zu PC parts (%d skipped: wheels, extras, low detail), %d triangles in the body and "
                                          "intact parts; the slot has %zu elements\n",
                      parts.size(), skipped, pcTris, elements.size());
        out.report += line;
        // the triangle budget, shared in proportion
        const float share = pcTris > opt.maxTriangles ? static_cast<float>(opt.maxTriangles) / static_cast<float>(pcTris) : 1.0f;
        // floor: a small part keeps enough to keep its shape (a bike wheel's rim and spokes at a 6% share were spikes)
        auto budget = [&](Mesh &m, int cap, int floor = 60) {
            const int target = std::max(std::min(floor, static_cast<int>(m.tris.size())),
                                        std::min(cap, static_cast<int>(static_cast<float>(m.tris.size()) * share)));
            if (static_cast<int>(m.tris.size()) > target)
                decimate(m, target);
        };
        budget(chassis, opt.maxTriangles);
        for (auto &[n, m] : intact)
            budget(m, opt.maxTriangles);
        for (auto &[n, m] : damaged)
            budget(m, opt.maxTriangles);
        for (auto &[n, m] : named) // (a bike's wheel is seen big: its rim, spokes and tyre keep 1500; at 600 the spokes became spikes)
            budget(m, opt.maxTriangles, n.find("wheel") != std::string::npos ? 1500 : 200);
        if (vlo.tris.empty())
        {
            vlo = chassis;
            budget(vlo, 300);
        }
        else
            budget(vlo, 400);
        {
            size_t kept = chassis.tris.size() + vlo.tris.size();
            for (const auto *group : {&intact, &damaged, &named})
                for (const auto &[n, m] : *group)
                    kept += m.tris.size();
            std::snprintf(line, sizeof(line), "triangles after the budget (%d): body %zu, all parts %zu\n", opt.maxTriangles,
                          chassis.tris.size(), kept);
            out.report += line;
        }

        // textures: every part's
        Mesh all;
        appendMesh(all, chassis, {0, 0, 0});
        appendMesh(all, vlo, {0, 0, 0});
        for (auto &[n, m] : intact)
            appendMesh(all, m, {0, 0, 0});
        for (auto &[n, m] : damaged)
            appendMesh(all, m, {0, 0, 0});
        for (auto &[n, m] : named)
            appendMesh(all, m, {0, 0, 0});
        appendMesh(all, ownWheel, {0, 0, 0});
        std::vector<std::string> allTex;
        out.xet = buildXet(all, txd, opt.maxTexture, donorTex, gap, out.report, allTex, opt.lcs, opt.lcs, opt.lcs ? 10 * 128 * 128 : 0);
        std::set<std::string> converted(allTex.begin(), allTex.end());
        converted.erase("");

        // the chunk: the stock one whole, then the new geometries
        Writer w;
        w.b.assign(donorMdl.begin(), donorMdl.begin() + fileSize);
        for (uint32_t i = 0; i < numRelocs; ++i)
            w.relocs.push_back(d.u32(relocTab + i * 4));
        // the moved dummies: their position in the parent's space
        for (size_t i = 0; i < nodes.size(); ++i)
            if (nodes[i].parent >= 0)
            {
                const DonorNode &up = nodes[static_cast<size_t>(nodes[i].parent)];
                const V3 local = up.rot.inverse(newWorld[i] - newWorld[static_cast<size_t>(nodes[i].parent)]);
                const V3 old = up.rot.inverse(nodes[i].world - up.world);
                if (std::fabs(local.x - old.x) + std::fabs(local.y - old.y) + std::fabs(local.z - old.z) > 0.001f)
                {
                    std::memcpy(w.b.data() + nodes[i].at + 0x40, &local.x, 4);
                    std::memcpy(w.b.data() + nodes[i].at + 0x44, &local.y, 4);
                    std::memcpy(w.b.data() + nodes[i].at + 0x48, &local.z, 4);
                }
            }
        std::vector<std::pair<uint32_t, std::string>> strings;
        std::vector<uint32_t> primary, secondary;
        int lightMats = 0, extraPaint = 0;
        Mesh empty; // a part the mod lacks: one degenerate triangle, nothing drawn
        empty.verts.assign(1, Vertex{});
        empty.mats.assign(1, Material{});
        empty.tris.push_back({{0, 0, 0}, 0});
        std::map<std::pair<const Mesh *, uint32_t>, size_t> written; // (mesh, node) -> geometry
        int replaced = 0, blank = 0, total = 0;
        // [lcs] The mod's own wheel (SA and many VC mods carry one under wheel_rf_dummy): the slot's wheel model is
        // shared with other cars (model info +0x40: LCS's Kuruma, Taxi, Manana ... all 251), so instead of replacing it the
        // car gets wheels of its own, as LCS's bikes have: one new element on each wheel_*_dummy node, all drawing
        // the mod's wheel scaled to 1 m (the size of the stock wheel models; the game scales the wheel nodes by the
        // model info's wheel scale each frame), and "set wheelmodel -1" makes the game add no shared wheel (the bikes'
        // value). New elements are linked as the stock ones: into their node's object list (node +8, element +8/+0xC),
        // into the element group's list (group +8 first / +0xC last, element +0x1C next) and with a render-callback
        // field (+0x24) in the global table.
        std::vector<uint32_t> newGlobals;
        int ownWheels = 0;
        if (!ownWheel.tris.empty())
        {
            std::set<uint32_t> relocSet(w.relocs.begin(), w.relocs.end());
            auto setPtr = [&](size_t at, uint32_t target) {
                if (relocSet.insert(static_cast<uint32_t>(at)).second)
                    w.ptr(at, target);
                else
                    w.put32(at, target);
            };
            const DonorElement *tmpl = nullptr;
            for (const DonorElement &e : elements)
                if (e.intact && (e.nodeName == "chassis_hi" || !tmpl))
                    tmpl = &e;
            const size_t stockElements = elements.size();
            for (size_t i = 0; i < nodes.size() && tmpl; ++i)
            {
                const std::string &n = nodes[i].name;
                if (n.rfind("wheel_", 0) != 0 || !ends(n, "_dummy") || !nodeElements[i].empty())
                    continue;
                w.align(16);
                const uint32_t E = static_cast<uint32_t>(w.reserve(0x30));
                std::memcpy(w.b.data() + E, donorMdl.data() + tmpl->at, 0x30);
                const uint32_t node = nodes[i].at;
                setPtr(E + 4, node);
                setPtr(E + 8, node + 8);
                setPtr(E + 0xC, node + 8);
                setPtr(node + 8, E + 8);
                setPtr(node + 0xC, E + 8);
                setPtr(E + 0x14, 0); // the geometry, set below
                setPtr(E + 0x18, group);
                uint32_t last;
                std::memcpy(&last, w.b.data() + group + 0xC, 4); // (moved by the wheel added before)
                setPtr(last, E + 0x1C);
                setPtr(E + 0x1C, group + 8);
                setPtr(group + 0xC, E + 0x1C);
                w.put32(E + 0x24, 0);
                newGlobals.push_back(E + 0x24);
                elements.push_back({E, node, true, n, newWorld[i], static_cast<int>(i)});
                ++ownWheels;
            }
            if (elements.size() == stockElements)
                ownWheel.tris.clear(); // no free wheel node (not a car slot): the shared wheel stays
        }
        for (const DonorElement &e : elements)
        {
            std::string base = e.nodeName;
            if (ends(base, "_dummy"))
                base = base.substr(0, base.size() - 6);
            const Mesh *src = nullptr;
            const bool wheelElement = !ownWheel.tris.empty() && e.nodeName.rfind("wheel_", 0) == 0 && ends(e.nodeName, "_dummy");
            if (wheelElement)
                src = &ownWheel;
            else if (named.count(e.nodeName))
                src = &named[e.nodeName];
            else if (base == "chassis_hi" || base == "boat_hi") // [lcs] LCS's boats: boat_hi / boat_vlo (+ boat_moving_hi)
                src = &chassis;
            else if (base == "chassis_vlo" || base == "boat_vlo")
                src = &vlo;
            else if (e.intact && intact.count(base))
                src = &intact[base];
            else if (!e.intact && damaged.count(base))
                src = &damaged[base];
            else if (!e.intact && intact.count(base))
                src = &intact[base];
            const bool missing = !src || src->tris.empty();
            if (missing)
                src = &empty, ++blank;
            else
                ++replaced;
            auto key = std::make_pair(src, missing || wheelElement ? 0u : e.node); // (one wheel geometry for all four)
            size_t geom;
            if (written.count(key))
                geom = written[key];
            else
            {
                Mesh local = *src; // in the node's space (its position and rotation undone)
                const Rot rot = e.nodeIndex >= 0 ? nodes[static_cast<size_t>(e.nodeIndex)].rot : Rot{};
                if (!missing && !wheelElement) // (the own wheel is in its node's space already)
                    for (Vertex &v : local.verts)
                        v.p = rot.inverse(v.p - e.nodePos), v.n = rot.inverse(v.n);
                std::vector<std::string> texNames(local.mats.size());
                for (size_t m = 0; m < local.mats.size(); ++m)
                    if (converted.count(local.mats[m].texture.substr(0, 31)))
                        texNames[m] = local.mats[m].texture.substr(0, 31);
                auto style = [&](Writer &ww, size_t at, int mat) {
                    const uint8_t *c = local.mats[static_cast<size_t>(mat)].rgba;
                    auto is = [&](int r, int g, int b) { return c[0] == r && c[1] == g && c[2] == b; };
                    // the paint marker colours (GTA III / VC / SA: primary 60,255,0, secondary 255,0,175; SA's third
                    // 0,255,255 and fourth 255,0,255 have no slot here: they take the primary / secondary paint)
                    const bool third = is(0, 255, 255), fourth = is(255, 0, 255);
                    const bool prim = is(60, 255, 0) || third, sec = is(255, 0, 175) || fourth;
                    // SA's light markers (headlights 255,175,0 / 0,255,200, tail lights 185,255,0 / 255,60,0): their
                    // texture carries the look, the marker colour would tint it, so they are drawn white
                    if (is(255, 175, 0) || is(0, 255, 200) || is(185, 255, 0) || is(255, 60, 0))
                    {
                        const uint8_t white[4] = {255, 255, 255, c[3]};
                        std::memcpy(ww.b.data() + at + 4, white, 4);
                        ++lightMats;
                        return;
                    }
                    if (third || fourth)
                        ++extraPaint;
                    if (prim || sec)
                    {
                        ww.put32(at + 4, 0xFFFFFFFFu);
                        if (paintFx)
                            ww.ptr(at + 12, paintFx);
                        (prim ? primary : secondary).push_back(static_cast<uint32_t>(at));
                    }
                    else
                        std::memcpy(ww.b.data() + at + 4, c, 4);
                };
                GeomOut g;
                w.align(16);
                if (!writeGeometry(w, local, texNames, meshUnknown, strings, style, g, error))
                    return false;
                geom = g.geom;
                written[key] = geom;
            }
            w.put32(e.at + 20, static_cast<uint32_t>(geom)); // the stock pointer is in the relocation table already
            ++total;
        }
        // the paint lists: the new materials, the stock ones dropped
        // [lcs] LCS: 25 + 25 (its KURUMA: 23 primary pointers then 2 zeros up to +0x8C, 13 secondary from +0x90)
        const uint32_t numPrimary = opt.lcs ? 25u : 30u;
        const uint32_t kPrimary = 0x2C, kSecondary = 0x2C + numPrimary * 4;
        w.relocs.erase(std::remove_if(w.relocs.begin(), w.relocs.end(),
                                      [&](uint32_t r) { return r >= kPrimary && r < kSecondary + 25 * 4; }),
                       w.relocs.end());
        for (uint32_t i = 0; i < numPrimary; ++i)
            if (i < primary.size())
                w.ptr(kPrimary + i * 4, primary[i]);
            else
                w.put32(kPrimary + i * 4, 0);
        for (uint32_t i = 0; i < 25; ++i)
            if (i < secondary.size())
                w.ptr(kSecondary + i * 4, secondary[i]);
            else
                w.put32(kSecondary + i * 4, 0);
        // the stock global pointer, kept
        // the stock global table (one render-callback field per element), kept
        std::vector<uint32_t> globals;
        for (uint32_t i = 0; i < numFuncs; ++i)
            globals.push_back(d.u32(globalTab + i * 4));
        globals.insert(globals.end(), newGlobals.begin(), newGlobals.end()); // the own wheels' callbacks
        finishChunk(w, 0x006D646Cu, globals, static_cast<uint16_t>(globals.size()), strings);
        if (ownWheels)
        {
            std::snprintf(line, sizeof(line), "wheels: the mod's own wheel on %d wheel nodes of this car (the shared wheel model is not used)\n"
                                              "set wheelmodel -1\n", ownWheels);
            out.report += line;
        }
        out.mdl = std::move(w.b);
        std::snprintf(line, sizeof(line), "written: %d of %d parts from the mod, %d left empty; %zu primary / %zu secondary paint "
                                          "materials; model %zu bytes, textures %zu bytes\n",
                      replaced, total, blank, primary.size(), secondary.size(), out.mdl.size(), out.xet.size());
        out.report += line;
        if (lightMats || extraPaint)
        {
            std::snprintf(line, sizeof(line), "SA colours: %d light materials drawn white, %d third / fourth paint materials as primary / secondary\n",
                          lightMats, extraPaint);
            out.report += line;
        }
        out.report += positions;
        return true;
    }

    int countTriangles(const std::vector<uint8_t> &b, std::string *dump)
    {
        Reader r{b.data(), b.size()};
        if (r.u32(0) != 0x006D646Cu)
            return -1;
        const uint32_t e = r.u32(0x20);
        if (!r.ok(e, 0x30))
            return -1;
        const uint32_t g = r.u32(e + 20), h = g + 0x20;
        if (!r.ok(h, 0x40))
            return -1;
        const uint32_t ns = r.u32(h + 16) >> 20, dma = h + 0x40 + 0x30 * ns;
        int tris = 0;
        for (uint32_t s = 0; s < ns; ++s)
        {
            const uint32_t m = h + 0x40 + 0x30 * s, pk = dma + r.u32(m + 28);
            const uint32_t end = pk + ((r.u32(pk) & 0xFFFF) + 1) * 16;
            std::vector<std::array<int16_t, 3>> verts;
            bool first = true;
            for (uint32_t w = pk + 16; w < end && r.ok(w, 4);)
            {
                const uint32_t c = r.u32(w);
                if (c == 0)
                {
                    w += 4;
                    continue;
                }
                if (c == 0x6C018000u)
                {
                    w += 20;
                    continue;
                }
                if (c == 0x20000000u)
                {
                    w += 8;
                    continue;
                }
                if (c == 0x30000000u)
                {
                    w += 20;
                    continue;
                }
                if (c == 0x14000006u)
                {
                    w += 4;
                    first = false;
                    continue;
                }
                const uint32_t n = c >> 16 & 0xFF, cmd = c >> 24;
                if ((cmd & 0x60) != 0x60)
                    return -1;
                static const uint32_t kBits[4] = {32, 16, 8, 16};
                const uint32_t sz = (cmd & 0x6F) == 0x6F ? 2 : ((cmd >> 2 & 3) + 1) * kBits[cmd & 3] / 8;
                if ((cmd & 0x6F) == 0x69) // positions V3-16
                    for (uint32_t i = first ? 0 : 2; i < n; ++i)
                        verts.push_back({static_cast<int16_t>(r.u16(w + 4 + i * 6)), static_cast<int16_t>(r.u16(w + 6 + i * 6)),
                                         static_cast<int16_t>(r.u16(w + 8 + i * 6))});
                w += 4 + ((n * sz + 3) & ~3u);
            }
            int here = 0;
            for (size_t i = 0; i + 2 < verts.size(); ++i)
                if (verts[i] != verts[i + 1] && verts[i + 1] != verts[i + 2] && verts[i] != verts[i + 2])
                    ++here;
            if (dump)
            {
                char line[120];
                std::snprintf(line, sizeof(line), "mesh %u: %zu strip vertices, %d triangles\n", s, verts.size(), here);
                *dump += line;
            }
            tris += here;
        }
        return tris;
    }
}

// See stories_bridge.h.
#include "game/stories_bridge.h"

#include "game/game_table.h"
#include "game/lcs_trainer.h"
#include "game/vcs_trainer.h"
#include "runtime/gs/gs_bridge.h"
#include "runtime/ps2_control.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bridge
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;

        // Per game. Evidence (Ghidra on each ELF + RAM dumps of a running game):
        // LCS: CPhysical::ProcessCollision 0x2BE900 walks the entity's sector nodes (+0x100; node +8 the sector,
        //   +0x10 next) into ProcessCollisionSectorList 0x2C3B38, which goes over the sector's 9 lists and calls
        //   ProcessEntityCollision (vtable +0xB4; CAutomobile's 0x1371C8 -> CCollision::ProcessColModels 0x26E8C0 with
        //   the wheel colpoints veh+0x4E0 and spring ratios +0x5E0). CPhysical::Add 0x2BBE90: sectors *0x3D09F4,
        //   100 x 100 of 40 m (index x / 40 + 50), 0x2C each; list 0 / 1 the buildings (RAM: CBuildings, 0x60 apart).
        //   Entity: matrix +0, flags u64 +0x40 (type bits 33-35, 1 = building; bit 41 uses collision), scan code s16
        //   +0x54, model s16 +0x58, vtable +0x5C. Model infos *0x3D8720 (0x3C each, col model +0x14). Col model:
        //   bounding sphere +0, box min +0x10 / max +0x20, s16 spheres +0x30 / boxes +0x32 / tris +0x34, u8 lines
        //   +0x36, boxes* +0x44 (0x30 each: min, max, surface u8 +0x20).
        // VCS: ProcessCollision 0x348D40 -> 0x34A220 walks +0xE4 into ProcessCollisionSectorList 0x34FD60 (11 lists);
        //   CAutomobile::ProcessEntityCollision 0x14C6A8 -> ProcessColModels 0x2B8ED0 (colpoints +0x570, ratios
        //   +0x670). CPhysical::Add 0x347348: sector(x, y) 0x472698 = *0x486464 + (y * 50 + x) * 0x38, 50 x 50 of 80 m
        //   (x / 80 + 30, y / 80 + 25); list 0 / 1 buildings. Entity: flags u64 +0x48 (type (f & 0xE) >> 1; bit 9 uses
        //   collision), scan code +0x54, model s16 +0x56 (and +0x58), vtable +0x5C. Model infos *0x486518, count
        //   *0x4CD10C. Col model: u16 boxes +0x30, u16 tris +0x32, u8 spheres +0x34, u8 lines +0x35, boxes* +0x40.
        struct Game
        {
            uint32_t sectors, cellLists, sectorSize, gridX, gridY;
            float cell, originX, originY; // index = (x - origin) / cell
            uint32_t modelInfos, modelCount;
            uint32_t flagsOff, modelOff, model2Off, scanOff;
            uint64_t colFlag, typeMask, typeBuilding;
            uint32_t cmBoxCount, cmBoxPtr, cmTriCount, cmSphereCount, cmLineCount; // offsets in the col model
            bool cmCountsAre8; // VCS: spheres / lines are bytes
        };
        constexpr Game kLcs{0x003D09F4u, 9,    0x2Cu, 100, 100, 40.0f, -2000.0f, -2000.0f, 0x003D8720u, 0,
                            0x40u,       0x58u, 0,    0x54u, 1ull << 41, 7ull << 33, 1ull << 33,
                            0x32u,       0x44u, 0x34u, 0x30u, 0x36u, false};
        constexpr Game kVcs{0x00486464u, 11,   0x38u, 50,    50,    80.0f, -2400.0f, -2000.0f, 0x00486518u, 0x004CD10Cu,
                            0x48u,       0x56u, 0x58u, 0x54u, 1ull << 9, 0xEull, 0x2ull,
                            0x30u,       0x40u, 0x32u, 0x34u, 0x35u, true};

        // Guest scratch: the EE kernel's area below the game (0x00000-0x7FFFF), unused by the recompiled game
        // (DECISIONS.md, free aim at 0x60000). The bridge takes 0x68000-0x6FFFF.
        constexpr uint32_t kScratch = 0x00068000u, kScratchEnd = 0x00070000u;

        // The deck, in LCS coordinates (VCS: + the dual-world offset). Portland: from the docks onto the east pier
        // (surface z 10.8 at x 1400-1610, y -614..-746, measured with the player). The lane between the warehouses is
        // narrower than it looks (bridge2, a Banshee driven east along it at 12 m/s at each y): at y -646 / -648 it stops
        // at x 1514 (the north warehouse's plinth, part of its own model: no separate object in the sector lists), at
        // -650 it glances off it; -656 and -662 run clear to the pier; -665 stops at x 1556. So the route runs at
        // y -656, 14 m wide (-663..-649), between the two. y -680 runs into a warehouse at x 1491-1525, y -700 into
        // stacked containers at 1586-1601. Vice City: the seawall road of Escobar International (VCS x -1834..-1810,
        // z 13.86, running north - south). 13 m over the sea: boats pass under.
        struct Seg
        {
            float x0, x1, z0, z1; // top of the deck at x0 / x1 (as drawn)
            float e0, e1;         // the collision box goes on this far along its slope past x0 / x1: its end edges
                                  // buried (in the ground, in the next box) - an edge level with the road surface
                                  // throws a car (a 2 m hop at 20 m/s, 12 m/s up at 42 m/s from the pier onto the ramp)
        };
        std::vector<Seg> g_segs = {
            {1466.0f, 1500.0f, 12.30f, 13.0f, 4.0f, 6.0f}, // [bridge4] the landing in Portland (y -407): the grass slope above the beach, ground 12.5 at x 1470, 7.3 at 1500; its west end buried in the hill (was the docks' pier ramp, 1560-1625)
            // the deck: one box from the ramp's top over the border (x 1800) to Vice City's seawall (VCS x -1835) -
            // flat segments end to end made the cars hop at every joint (the boxes' end edges; 85 m segments: +2 m at
            // x 1795 and 1880, 20 m/s)
            {1500.0f, 1900.0f, 13.0f, 13.0f, -4.0f, 20.0f}, // (starts 4 m in under the ramp's end, ends under the next)
            // Vice City: Escobar's seawall rises from 13.04 at VCS x -1845 to its top, 13.86, at -1834 (probed with the
            // player, bridge2). [bridge2] The first ramp climbed 0.8 m in the last 13 m (3.5 deg): at 42 m/s it threw the
            // car ~20 m through the air into the airport fence's kerb at -1805. Now a long one, 0.85 m over 67 m (0.7 deg,
            // 0.5 m/s up at 42 m/s), above the seawall's slope all the way, ending 1 cm under the seawall's top where that
            // is flat (VCS -1833): the car rolls onto the seawall road (which runs north - south) level
            {1900.0f, 1967.0f, 13.0f, 13.85f, 6.0f, 0.0f},
        };
        float g_y = -407.0f, g_width = 14.0f, // [bridge2] was -652, 16 m (the docks post)
              g_thick = 1.5f, g_kerbW = 0.6f, g_kerbH = 0.9f;
        // [bridge4] The route is no longer one y: the Liberty City end stays at g_y (the docks lane), the Vice City end
        // lies g_vcsDy further south (STORIES_BRIDGE_VCS_DY, default -100: the first landing, at y -656, ran into
        // barricades and the airport fence). In between the road bends over g_bendX0..g_bendX1 (LCS x): a slope that
        // eases in and out (trapezoid), so a car at speed meets no kink. Drawn as a shear of the straight deck (every
        // point's y moved by shift(x)), collided as short yawed boxes along the same polyline.
        float g_vcsDy = 0.0f, g_vcsZ = 13.95f, g_bendX0 = 1630.0f, g_bendX1 = 1955.0f, g_ease = 0.12f;
        std::vector<std::pair<float, float>> g_route; // (x, dy from g_y), the polyline's nodes
        void makeRoute()
        {
            g_route.clear();
            if (g_vcsDy == 0.0f)
                return;
            const float L = g_bendX1 - g_bendX0;
            const int n = std::max(1, int(std::round(L / 20.0f)));
            // slope profile u in 0..1: 0 -> 1 over the first g_ease, flat, 1 -> 0 over the last g_ease; integrated and scaled to 1
            auto slope = [](float u) { return std::min(1.0f, std::min(u / g_ease, (1.0f - u) / g_ease)); };
            constexpr int kN = 2000;
            std::vector<float> cum(kN + 1, 0.0f);
            for (int i = 1; i <= kN; ++i)
                cum[i] = cum[i - 1] + slope((i - 0.5f) / kN);
            for (int i = 0; i <= n; ++i)
            {
                const float u = float(i) / float(n);
                g_route.push_back({g_bendX0 + L * u, g_vcsDy * cum[int(std::lround(u * kN))] / cum[kN]});
            }
        }
        // the road's centre line, as an offset from g_y (LCS x; flat beyond the bend)
        float shiftAt(float x)
        {
            if (g_route.empty())
                return 0.0f;
            if (x <= g_route.front().first)
                return g_route.front().second;
            for (size_t i = 1; i < g_route.size(); ++i)
                if (x <= g_route[i].first)
                {
                    const auto &a = g_route[i - 1], &b = g_route[i];
                    return a.second + (b.second - a.second) * (x - a.first) / (b.first - a.first);
                }
            return g_route.back().second;
        }
        // the collision pieces: the segments cut at the route's nodes, each a box along a chord of the route
        struct Chunk
        {
            float x0, x1, z0, z1, y0, y1, e0, e1;
        };
        std::vector<Chunk> g_chunks;
        void makeChunks()
        {
            g_chunks.clear();
            for (const Seg &s : g_segs)
            {
                std::vector<float> xs = {s.x0, s.x1};
                for (const auto &nd : g_route)
                    if (nd.first > s.x0 + 0.5f && nd.first < s.x1 - 0.5f)
                        xs.push_back(nd.first);
                std::sort(xs.begin(), xs.end());
                for (size_t i = 0; i + 1 < xs.size(); ++i)
                {
                    const float a = xs[i], b = xs[i + 1], m = (s.z1 - s.z0) / (s.x1 - s.x0);
                    Chunk c{a, b, s.z0 + m * (a - s.x0), s.z0 + m * (b - s.x0), g_y + shiftAt(a), g_y + shiftAt(b),
                            i == 0 ? s.e0 : 3.0f, i + 2 == xs.size() ? s.e1 : 3.0f};
                    // (a long overhang under a yawed neighbour would stick out sideways)
                    c.e0 = std::min(c.e0, 8.0f), c.e1 = std::min(c.e1, 8.0f);
                    g_chunks.push_back(c);
                }
            }
        }

        struct Built
        {
            uint32_t ent = 0, col = 0;
            std::vector<uint32_t> nodes, heads; // our list nodes, the list heads they hang from
        };
        bool g_on = false, g_built = false, g_failed = false;
        float g_off[3] = {-3800.0f, 0.0f, 0.0f};
        const Game *G = nullptr;
        std::vector<Built> g_ents;
        uint32_t g_modelIndex = 0, g_modelInfo = 0, g_template = 0;
        std::atomic<uint32_t> g_relinks{0}, g_checks{0};
        uint32_t g_frame = 0, g_readyFrames = 0;
        std::string g_why = "not built";

        uint32_t rd32(const uint8_t *r, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, r + (a & kRamMask), 4);
            return v;
        }
        uint64_t rd64(const uint8_t *r, uint32_t a)
        {
            uint64_t v;
            std::memcpy(&v, r + (a & kRamMask), 8);
            return v;
        }
        int16_t rd16(const uint8_t *r, uint32_t a)
        {
            int16_t v;
            std::memcpy(&v, r + (a & kRamMask), 2);
            return v;
        }
        void wr32(uint8_t *r, uint32_t a, uint32_t v) { std::memcpy(r + (a & kRamMask), &v, 4); }
        void wr16(uint8_t *r, uint32_t a, uint16_t v) { std::memcpy(r + (a & kRamMask), &v, 2); }
        void wrf(uint8_t *r, uint32_t a, float v) { std::memcpy(r + (a & kRamMask), &v, 4); }
        bool ptr(uint32_t p) { return p >= 0x00100000u && p < 0x02000000u; }
        // a list node: the game's (its heap) or ours (the scratch area)
        bool nodeOk(uint32_t p) { return ptr(p) || (p >= kScratch && p < kScratchEnd); }

        // the game's coordinates of an LCS point
        float gx(float lcsX) { return game::isLcs() ? lcsX : lcsX + g_off[0]; }
        float gy(float lcsY) { return game::isLcs() ? lcsY : lcsY + g_off[1]; }
        float gz(float lcsZ) { return game::isLcs() ? lcsZ : lcsZ + g_off[2]; }

        uint32_t sectorAt(const uint8_t *r, int ix, int iy) { return rd32(r, G->sectors) + (uint32_t(iy) * G->gridX + uint32_t(ix)) * G->sectorSize; }

        // a real building with collision boxes: its entity and model info are the templates
        bool findTemplate(const uint8_t *r)
        {
            const uint32_t infos = rd32(r, G->modelInfos);
            if (!ptr(rd32(r, G->sectors)) || !ptr(infos))
                return false;
            for (uint32_t iy = 0; iy < G->gridY; ++iy)
                for (uint32_t ix = 0; ix < G->gridX; ++ix)
                {
                    uint32_t n = rd32(r, sectorAt(r, int(ix), int(iy)));
                    for (int k = 0; n && nodeOk(n) && k < 64; ++k, n = rd32(r, n + 8))
                    {
                        const uint32_t e = rd32(r, n);
                        if (!ptr(e))
                            continue; // (ours)
                        const uint64_t f = rd64(r, e + G->flagsOff);
                        if ((f & G->typeMask) != G->typeBuilding || !(f & G->colFlag))
                            continue;
                        const int m = rd16(r, e + G->modelOff);
                        if (m <= 0)
                            continue;
                        const uint32_t mi = rd32(r, infos + 4u * uint32_t(m));
                        if (!ptr(mi) || !ptr(rd32(r, mi + 0x14)))
                            continue;
                        const uint32_t col = rd32(r, mi + 0x14);
                        const uint32_t boxes = G->cmCountsAre8 ? uint16_t(rd16(r, col + G->cmBoxCount)) : uint16_t(rd16(r, col + G->cmBoxCount));
                        if (boxes == 0 || !ptr(rd32(r, col + G->cmBoxPtr)))
                            continue;
                        g_template = e;
                        g_modelInfo = mi; // (template; replaced by our copy)
                        return true;
                    }
                }
            return false;
        }

        uint32_t modelCount(const uint8_t *r)
        {
            if (G->modelCount)
                return rd32(r, G->modelCount);
            // LCS: the table ends where its entries stop being pointers (RAM: 4121 entries, then 0x17171717 fill)
            const uint32_t infos = rd32(r, G->modelInfos);
            uint32_t n = 0;
            while (n < 8000 && (rd32(r, infos + 4 * n) == 0 || ptr(rd32(r, infos + 4 * n))))
                ++n;
            return n;
        }

        // our nodes into the sector lists the entity's world box covers (list 0, at its head)
        void link(uint8_t *r, Built &b, uint32_t &alloc, float x0, float x1, float y0, float y1)
        {
            const int ix0 = std::clamp(int(std::floor((x0 - G->originX) / G->cell)), 0, int(G->gridX) - 1);
            const int ix1 = std::clamp(int(std::floor((x1 - G->originX) / G->cell)), 0, int(G->gridX) - 1);
            const int iy0 = std::clamp(int(std::floor((y0 - G->originY) / G->cell)), 0, int(G->gridY) - 1);
            const int iy1 = std::clamp(int(std::floor((y1 - G->originY) / G->cell)), 0, int(G->gridY) - 1);
            for (int iy = iy0; iy <= iy1; ++iy)
                for (int ix = ix0; ix <= ix1; ++ix)
                {
                    const uint32_t head = sectorAt(r, ix, iy); // list 0 at +0
                    const uint32_t node = alloc;
                    alloc += 0x10u;
                    b.nodes.push_back(node);
                    b.heads.push_back(head);
                }
        }

        // a node hung at the head of its list (CPtrList: node {item, prev, next}; the old head's prev -> us)
        void hang(uint8_t *r, uint32_t node, uint32_t ent, uint32_t head)
        {
            const uint32_t old = rd32(r, head);
            if (old == node)
                return; // already the head
            wr32(r, node + 0, ent);
            wr32(r, node + 4, 0);
            wr32(r, node + 8, old);
            if (nodeOk(old))
                wr32(r, old + 4, node);
            wr32(r, head, node);
        }

        bool inList(const uint8_t *r, uint32_t head, uint32_t node)
        {
            uint32_t n = rd32(r, head);
            for (int k = 0; n && nodeOk(n) && k < 4096; ++k, n = rd32(r, n + 8))
                if (n == node)
                    return true;
            return false;
        }

        // our nodes out of whatever lists still hold them
        void unlinkAll(uint8_t *r)
        {
            for (const Built &b : g_ents)
                for (size_t k = 0; k < b.nodes.size(); ++k)
                {
                    const uint32_t head = b.heads[k], node = b.nodes[k];
                    uint32_t prev = 0, n = rd32(r, head);
                    for (int i = 0; n && nodeOk(n) && i < 4096; ++i)
                    {
                        if (n == node)
                        {
                            const uint32_t next = rd32(r, n + 8);
                            if (prev)
                                wr32(r, prev + 8, next);
                            else
                                wr32(r, head, next);
                            if (nodeOk(next))
                                wr32(r, next + 4, prev);
                            break;
                        }
                        prev = n;
                        n = rd32(r, n + 8);
                    }
                }
            g_ents.clear();
        }

        bool gameReady() { return game::isLcs() ? lcs::trainer::ready() : vcs::trainer::ready(); }

        void build(uint8_t *r)
        {
            if (!findTemplate(r))
            {
                g_why = "no template building yet";
                return;
            }
            const uint32_t infos = rd32(r, G->modelInfos);
            const uint32_t count = modelCount(r);
            g_modelIndex = 0;
            for (uint32_t i = count; i-- > 1000;)
                if (rd32(r, infos + 4 * i) == 0)
                {
                    g_modelIndex = i;
                    break;
                }
            if (!g_modelIndex)
            {
                g_why = "no free model slot";
                g_failed = true;
                return;
            }
            makeRoute();
            makeChunks();
            unlinkAll(r); // a rebuild: our old nodes out of the lists before their memory is reused
            uint32_t alloc = kScratch;
            std::memset(r + kScratch, 0, kScratchEnd - kScratch);
            // our model info: a copy of the template's (0x40 bytes), its col model replaced per entity? No: one model
            // index has one col model, so each segment gets its own model slot? The col model is per model, not per
            // entity: one model info per segment.
            const uint32_t tmplInfo = g_modelInfo;
            const uint32_t tmplCol = rd32(r, tmplInfo + 0x14);
            std::vector<uint32_t> slots;
            for (uint32_t i = count; i-- > 1000 && slots.size() < g_chunks.size();)
                if (rd32(r, infos + 4 * i) == 0)
                    slots.push_back(i);
            if (slots.size() < g_chunks.size())
            {
                g_why = "not enough free model slots";
                g_failed = true;
                return;
            }
            g_ents.clear();
            for (size_t s = 0; s < g_chunks.size(); ++s)
            {
                const Chunk &dr = g_chunks[s];
                const float m = (dr.z1 - dr.z0) / (dr.x1 - dr.x0), my = (dr.y1 - dr.y0) / (dr.x1 - dr.x0);
                // as collided: the ends pushed out along the chord
                const float ax0 = dr.x0 - dr.e0, ax1 = dr.x1 + dr.e1;
                const float az0 = dr.z0 - m * dr.e0, az1 = dr.z1 + m * dr.e1;
                const float ay0 = dr.y0 - my * dr.e0, ay1 = dr.y1 + my * dr.e1;
                const float Lx = ax1 - ax0, Ly = ay1 - ay0, H = az1 - az0;
                const float L = std::hypot(Lx, Ly);
                const float len = std::sqrt(L * L + H * H);
                const float ca = L / len, sa = H / len;
                const float cpsi = Lx / L, spsi = Ly / L; // the yaw
                const float hl = len * 0.5f, hw = g_width * 0.5f * cpsi; // (the drawn deck is g_width across in y)
                const float kW = g_kerbW * cpsi;
                // boxes, local: the deck (top at z 0) and two kerbs
                const uint32_t boxes = alloc;
                alloc += 3 * 0x30u;
                auto box = [&](int i, float ax, float ay, float az, float bx, float by, float bz) {
                    const uint32_t a = boxes + uint32_t(i) * 0x30u;
                    wrf(r, a + 0, ax), wrf(r, a + 4, ay), wrf(r, a + 8, az);
                    wrf(r, a + 0x10, bx), wrf(r, a + 0x14, by), wrf(r, a + 0x18, bz);
                    r[(a + 0x20) & kRamMask] = 0; // surface: default (the docks' own triangles are 0)
                    r[(a + 0x21) & kRamMask] = 0;
                };
                box(0, -hl, -hw, -g_thick, hl, hw, 0.0f);
                box(1, -hl, -hw, -0.1f, hl, -hw + kW, g_kerbH);
                box(2, -hl, hw - kW, -0.1f, hl, hw, g_kerbH);
                // the col model: the template's header, our bounds and boxes
                const uint32_t col = alloc;
                alloc += 0x60u;
                std::memcpy(r + col, r + (tmplCol & kRamMask), 0x60);
                const float rad = std::sqrt(hl * hl + hw * hw + g_thick * g_thick) + 0.5f;
                wrf(r, col + 0, 0.0f), wrf(r, col + 4, 0.0f), wrf(r, col + 8, -g_thick * 0.5f), wrf(r, col + 0xC, rad);
                wrf(r, col + 0x10, -hl), wrf(r, col + 0x14, -hw), wrf(r, col + 0x18, -g_thick), wrf(r, col + 0x1C, 0.0f);
                wrf(r, col + 0x20, hl), wrf(r, col + 0x24, hw), wrf(r, col + 0x28, g_kerbH), wrf(r, col + 0x2C, 0.0f);
                wr16(r, col + G->cmBoxCount, 3);
                wr16(r, col + G->cmTriCount, 0);
                if (G->cmCountsAre8)
                {
                    r[(col + G->cmSphereCount) & kRamMask] = 0;
                    r[(col + G->cmLineCount) & kRamMask] = 0;
                }
                else
                {
                    wr16(r, col + G->cmSphereCount, 0);
                    r[(col + G->cmLineCount) & kRamMask] = 0;
                }
                wr32(r, col + G->cmBoxPtr, boxes);
                // the model info
                const uint32_t mi = alloc;
                alloc += 0x40u;
                std::memcpy(r + mi, r + (tmplInfo & kRamMask), 0x40);
                wr32(r, mi + 0x14, col);
                wr32(r, infos + 4 * slots[s], mi);
                // the entity: a copy of the template building, our matrix and model
                Built b;
                b.ent = alloc;
                b.col = col;
                alloc += 0x60u;
                std::memcpy(r + b.ent, r + (g_template & kRamMask), 0x60);
                const float cx = gx((ax0 + ax1) * 0.5f), cy = gy((ay0 + ay1) * 0.5f), cz = gz((az0 + az1) * 0.5f);
                // right = local x along the deck (yawed, pitched), forward = the deck's left, up
                wrf(r, b.ent + 0x00, cpsi * ca), wrf(r, b.ent + 0x04, spsi * ca), wrf(r, b.ent + 0x08, sa);
                wrf(r, b.ent + 0x10, -spsi), wrf(r, b.ent + 0x14, cpsi), wrf(r, b.ent + 0x18, 0.0f);
                wrf(r, b.ent + 0x20, -sa * cpsi), wrf(r, b.ent + 0x24, -sa * spsi), wrf(r, b.ent + 0x28, ca);
                wrf(r, b.ent + 0x30, cx), wrf(r, b.ent + 0x34, cy), wrf(r, b.ent + 0x38, cz);
                wr16(r, b.ent + G->scanOff, 0);
                wr16(r, b.ent + G->modelOff, uint16_t(slots[s]));
                if (G->model2Off)
                    wr16(r, b.ent + G->model2Off, uint16_t(slots[s]));
                {
                    // the box's world footprint, a metre of margin
                    const float ex = std::fabs(hl * cpsi * ca) + std::fabs(hw * spsi) + 1.0f, ey = std::fabs(hl * spsi * ca) + std::fabs(hw * cpsi) + 1.0f;
                    link(r, b, alloc, cx - ex, cx + ex, cy - ey, cy + ey);
                }
                if (alloc > kScratchEnd)
                {
                    g_why = "scratch full";
                    g_failed = true;
                    return;
                }
                for (size_t k = 0; k < b.nodes.size(); ++k)
                    hang(r, b.nodes[k], b.ent, b.heads[k]);
                g_ents.push_back(b);
            }
            g_modelIndex = slots.front();
            g_built = true;
            char buf[160];
            std::snprintf(buf, sizeof(buf), "built: %zu segments, models %u..%u, template %08x, %u bytes", g_ents.size(),
                          slots.back(), slots.front(), g_template, alloc - kScratch);
            g_why = buf;
            std::fprintf(stderr, "[bridge] %s\n", buf);
        }


        // ---- the visible bridge: world quads, projected each game frame with the game's camera ----
        // [bridge2] Our own drawing, no game art: Gouraud-coloured quads. The road is a grid of vertices with a fixed
        // pseudo-random shade each (asphalt grain), lane markings just above it, a concrete parapet with a steel rail,
        // box girders and piers down into the sea, two cable-stayed towers (one each side of the border, so each city
        // has one close by), lamp posts whose heads light at night and throw pools of light on the road. Fogged by the
        // GS with the game's own fog colour (the frame's FOGCOL) and the camera's fog plane.
        struct Poly
        {
            float p[4][3];
            uint32_t c[4];  // 0xAABBGGRR, our 0..255 scale (shade() takes it to the GS's)
            float glow[4];  // the lamps' light on it at night (0..1)
            uint8_t kind;   // 0 lit by the hour, 1 a lamp (bright at night), 2 a warning light (red, night only)
        };
        std::vector<Poly> g_polys;
        std::mutex g_drawMutex;
        // the projections, one a game frame: the newest, and the last 16 by their frame tags (the renderer asks for its frame)
        struct Frame
        {
            uint64_t seq = 0, tag = 0; // tag: the game frame's (gsdw::setFrameTag), what the renderer says it draws
            std::vector<gsbridge::Vert> tris;
        };
        std::mutex g_frameMutex;
        std::shared_ptr<const Frame> g_newestFrame;
        std::shared_ptr<const Frame> g_tagged[16]; // by tag % 16 ([dwlag]'s frame tags)
        uint64_t g_tagHits = 0, g_tagMisses = 0;   // (g_frameMutex)
        uint64_t g_seq = 0, g_lagSum = 0, g_lagN = 0, g_lagMax = 0; // (g_frameMutex) frames the newest was ahead at draws
        bool g_sync = true; // STORIES_BRIDGE_SYNC=0 / `bridge sync 0`: the newest projection (the old way, to compare)
        // The game's depth (the frame's 0..1 units) at view depth w: R * near / w + B (near: the RW camera's near plane,
        // which the game moves: 0.1125 on foot, 0.6 in a car), linear in 1/w as the GS interpolates it. Measured in LCS
        // (calib2.py: a small square on the flat pier, z 10.80, at 8 / 15 / 30 m; the scale where it starts to pass GEQUAL
        // against the pier): near 0.1125 -> 0.006843 / w + 0.000614 (the 15 m point predicted within 0.4%), near 0.6 ->
        // 0.03649 / w + 0.000579: R 0.0608, B 0.0006. STORIES_BRIDGE_Z=R,B or `bridge z R B` overrides. Only used when
        // the renderer has no projection of the frame's own (gs_bridge.h).
        float g_zR = 0.0608f, g_zB = 0.0006f;

        uint32_t scaleRgb(uint32_t c, float k)
        {
            auto ch = [&](int sh) { return uint32_t(std::clamp(float((c >> sh) & 0xFFu) * k, 0.0f, 255.0f)) << sh; };
            return ch(0) | ch(8) | ch(16) | (c & 0xFF000000u);
        }
        uint32_t rgb(int r, int g, int b) { return 0xFF000000u | uint32_t(std::clamp(b, 0, 255)) << 16 | uint32_t(std::clamp(g, 0, 255)) << 8 | uint32_t(std::clamp(r, 0, 255)); }

        void addQuad(const float a[3], const float b[3], const float c[3], const float d[3], uint32_t rgba, uint8_t kind = 0)
        {
            Poly q;
            std::memcpy(q.p[0], a, 12), std::memcpy(q.p[1], b, 12), std::memcpy(q.p[2], c, 12), std::memcpy(q.p[3], d, 12);
            for (int i = 0; i < 4; ++i)
                q.c[i] = rgba, q.glow[i] = 0.0f;
            q.kind = kind;
            g_polys.push_back(q);
        }

        // ---- the bridge's shape, in LCS coordinates (P() takes a point to this game's) ----
        struct V3
        {
            float x, y, z;
        };
        V3 P(float x, float y, float z) { return {gx(x), gy(y + shiftAt(x)), gz(z)}; }
        // the top of the road at LCS x (the segments as drawn; flat beyond the ends)
        float deckZ(float x)
        {
            if (x <= g_segs.front().x0)
                return g_segs.front().z0;
            for (const Seg &s : g_segs)
                if (x <= s.x1)
                    return s.z0 + (s.z1 - s.z0) * (x - s.x0) / (s.x1 - s.x0);
            return g_segs.back().z1;
        }
        float deckX0() { return g_segs.front().x0; }
        float deckX1() { return g_segs.back().x1; }
        std::vector<float> g_lampX;                 // lamp posts (both sides), LCS x
        const float kTowerX[2] = {1720.0f, 1880.0f}; // the towers, LCS x (the border at 1800 between them)
        constexpr float kSeaBed = -4.0f, kTowerTop = 58.0f;

        void quadV(V3 a, V3 b, V3 c, V3 d, uint32_t col, uint8_t kind = 0)
        {
            const float A[3] = {a.x, a.y, a.z}, B[3] = {b.x, b.y, b.z}, C[3] = {c.x, c.y, c.z}, D[3] = {d.x, d.y, d.z};
            addQuad(A, B, C, D, col, kind);
        }
        // an axis-aligned box (LCS coordinates), its faces shaded by their direction (a fixed light from above)
        void box(float x0, float x1, float y0, float y1, float z0, float z1, uint32_t col, uint8_t kind = 0, bool bottom = false)
        {
            const uint32_t top = col, sy = scaleRgb(col, 0.86f), sx = scaleRgb(col, 0.74f), bot = scaleRgb(col, 0.55f);
            quadV(P(x0, y0, z1), P(x1, y0, z1), P(x1, y1, z1), P(x0, y1, z1), kind ? col : top, kind);
            quadV(P(x0, y0, z0), P(x1, y0, z0), P(x1, y0, z1), P(x0, y0, z1), kind ? col : sy, kind);
            quadV(P(x0, y1, z0), P(x1, y1, z0), P(x1, y1, z1), P(x0, y1, z1), kind ? col : sy, kind);
            quadV(P(x0, y0, z0), P(x0, y1, z0), P(x0, y1, z1), P(x0, y0, z1), kind ? col : sx, kind);
            quadV(P(x1, y0, z0), P(x1, y1, z0), P(x1, y1, z1), P(x1, y0, z1), kind ? col : sx, kind);
            if (bottom)
                quadV(P(x0, y0, z0), P(x1, y0, z0), P(x1, y1, z0), P(x0, y1, z0), kind ? col : bot, kind);
        }
        // a beam along the deck from x0 to x1 (following the road's slope), between y0..y1 and dz0..dz1 over the road
        void deckBeam(float x0, float x1, float y0, float y1, float dz0, float dz1, uint32_t col, bool bottom = true)
        {
            const uint32_t sy = scaleRgb(col, 0.86f), bot = scaleRgb(col, 0.55f);
            // pieces at the segment joints, so the beam follows the ramps
            std::vector<float> xs = {x0};
            for (const Seg &s : g_segs)
                for (float j : {s.x0, s.x1})
                    if (j > x0 + 0.01f && j < x1 - 0.01f)
                        xs.push_back(j);
            xs.push_back(x1);
            std::sort(xs.begin(), xs.end());
            for (size_t i = 0; i + 1 < xs.size(); ++i)
            {
                const float a = xs[i], b = xs[i + 1], za = deckZ(a), zb = deckZ(b);
                quadV(P(a, y0, za + dz1), P(b, y0, zb + dz1), P(b, y1, zb + dz1), P(a, y1, za + dz1), col);
                quadV(P(a, y0, za + dz0), P(b, y0, zb + dz0), P(b, y0, zb + dz1), P(a, y0, za + dz1), sy);
                quadV(P(a, y1, za + dz0), P(b, y1, zb + dz0), P(b, y1, zb + dz1), P(a, y1, za + dz1), sy);
                if (bottom)
                    quadV(P(a, y0, za + dz0), P(b, y0, zb + dz0), P(b, y1, zb + dz0), P(a, y1, za + dz0), bot);
            }
        }
        // a thin line in space (a cable): two crossed strips, so it shows from any side
        void cable(V3 a, V3 b, float w, uint32_t col)
        {
            const float d[3] = {b.x - a.x, b.y - a.y, b.z - a.z};
            const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len < 0.01f)
                return;
            // across 1: horizontal, perpendicular to the cable; across 2: perpendicular to both
            float n1[3] = {-d[1], d[0], 0.0f};
            float l1 = std::sqrt(n1[0] * n1[0] + n1[1] * n1[1]);
            if (l1 < 1e-4f)
                n1[0] = 1.0f, n1[1] = 0.0f, l1 = 1.0f;
            n1[0] *= w / l1, n1[1] *= w / l1;
            float n2[3] = {d[1] * n1[2] - d[2] * n1[1], d[2] * n1[0] - d[0] * n1[2], d[0] * n1[1] - d[1] * n1[0]};
            const float l2 = std::sqrt(n2[0] * n2[0] + n2[1] * n2[1] + n2[2] * n2[2]);
            for (float &v : n2)
                v *= w / l2;
            for (const float *n : {static_cast<const float *>(n1), static_cast<const float *>(n2)})
                quadV({a.x - n[0], a.y - n[1], a.z - n[2]}, {b.x - n[0], b.y - n[1], b.z - n[2]}, {b.x + n[0], b.y + n[1], b.z + n[2]},
                      {a.x + n[0], a.y + n[1], a.z + n[2]}, col);
        }
        // a fixed pseudo-random 0..1 per grid point (the asphalt's grain, the same every run)
        float grain(int i, int j)
        {
            uint32_t h = uint32_t(i) * 73856093u ^ uint32_t(j) * 19349663u;
            h ^= h >> 13, h *= 0x5bd1e995u, h ^= h >> 15;
            return float(h & 0xFFFFu) / 65535.0f;
        }
        // the lamps' light at a road point (LCS x / y): pools of ~10 m under each head
        float lampGlow(float x, float y)
        {
            float g = 0.0f;
            const float hw = g_width * 0.5f;
            for (float lx : g_lampX)
                for (int side = -1; side <= 1; side += 2)
                {
                    const float hx = lx, hy = g_y + side * (hw - 2.4f); // the head hangs over the road
                    const float d = std::hypot(x - hx, y - hy);
                    if (d < 11.0f)
                        g += (1.0f - d / 11.0f) * (1.0f - d / 11.0f);
                }
            return std::min(g, 1.0f);
        }

        // the bridge as quads in this game's coordinates
        void buildPolys()
        {
            g_polys.clear();
            const float hw = g_width * 0.5f, k = g_kerbW, H = g_kerbH, T = g_thick, y0 = g_y;
            const float x0 = deckX0(), x1 = deckX1();
            // lamp posts: every 36 m along the deck, not at the towers
            g_lampX.clear();
            for (float lx = x0 + 14.0f; lx < x1 - 6.0f; lx += 36.0f)
                if (std::fabs(lx - kTowerX[0]) > 10.0f && std::fabs(lx - kTowerX[1]) > 10.0f)
                    g_lampX.push_back(lx);

            // the road: a grid ~4 m along by 1.85 m across, each vertex its own shade of asphalt (and the lamps' glow)
            const float ry0 = y0 - hw + k, ry1 = y0 + hw - k;
            constexpr int kAcross = 8;
            for (const Seg &sg : g_segs)
            {
                const int n = std::max(1, int(std::round((sg.x1 - sg.x0) / 4.0f)));
                for (int t = 0; t < n; ++t)
                {
                    const float xa = sg.x0 + (sg.x1 - sg.x0) * t / n, xb = sg.x0 + (sg.x1 - sg.x0) * (t + 1) / n;
                    for (int j = 0; j < kAcross; ++j)
                    {
                        const float ya = ry0 + (ry1 - ry0) * j / kAcross, yb = ry0 + (ry1 - ry0) * (j + 1) / kAcross;
                        Poly q;
                        const float xs[4] = {xa, xb, xb, xa}, ys[4] = {ya, ya, yb, yb};
                        const int js[4] = {j, j, j + 1, j + 1};
                        for (int v = 0; v < 4; ++v)
                        {
                            const V3 p = P(xs[v], ys[v], deckZ(xs[v]));
                            q.p[v][0] = p.x, q.p[v][1] = p.y, q.p[v][2] = p.z;
                            const int gi = int(std::lround(xs[v] * 0.25f));
                            // asphalt: dark grey-blue, grain +-9, the wheel paths of each lane a little darker
                            const float lw = (ry1 - ry0) * 0.25f; // two lanes each way
                            const float lane = std::fabs(std::fmod(std::fabs(ys[v] - y0), lw) - lw * 0.5f); // 0 lane middle
                            // two scales of grain: per vertex, and patches of ~12 m (worn / newer asphalt)
                            const float patch = grain(int(std::floor(xs[v] / 12.0f)) + 7919, int(std::floor((ys[v] - y0) / 7.0f)) + 31);
                            const int base = 82 + int((grain(gi, js[v]) - 0.5f) * 26.0f) + int((patch - 0.5f) * 14.0f) -
                                             (lane > 0.6f && lane < 1.3f ? 5 : 0);
                            q.c[v] = rgb(base, base, base + 5);
                            q.glow[v] = lampGlow(xs[v], ys[v]);
                        }
                        q.kind = 0;
                        g_polys.push_back(q);
                    }
                }
            }
            // markings, 3 cm over the road: a double yellow centre line, white lane dashes (3 m in 9), white edge lines
            auto mark = [&](float xa, float xb, float ya, float yb, uint32_t col) {
                const float za = deckZ(xa) + 0.03f, zb = deckZ(xb) + 0.03f;
                Poly q;
                const V3 v[4] = {P(xa, ya, za), P(xb, ya, zb), P(xb, yb, zb), P(xa, yb, za)};
                for (int i = 0; i < 4; ++i)
                {
                    q.p[i][0] = v[i].x, q.p[i][1] = v[i].y, q.p[i][2] = v[i].z;
                    q.c[i] = col;
                    q.glow[i] = lampGlow(i == 0 || i == 3 ? xa : xb, ya);
                }
                q.kind = 0;
                g_polys.push_back(q);
            };
            const uint32_t yellow = rgb(232, 196, 48), white = rgb(225, 225, 220);
            for (float xa = x0; xa < x1 - 0.01f; xa += 6.0f) // solid lines in 6 m pieces (they follow the ramps)
            {
                const float xb = std::min(xa + 6.0f, x1);
                mark(xa, xb, y0 - 0.32f, y0 - 0.18f, yellow);
                mark(xa, xb, y0 + 0.18f, y0 + 0.32f, yellow);
                mark(xa, xb, ry0 + 0.25f, ry0 + 0.40f, white);
                mark(xa, xb, ry1 - 0.40f, ry1 - 0.25f, white);
            }
            for (float xa = x0 + 4.0f; xa < x1 - 4.0f; xa += 9.0f)
                for (int side = -1; side <= 1; side += 2)
                {
                    const float yc = y0 + side * (ry1 - ry0) * 0.25f;
                    mark(xa, xa + 3.0f, yc - 0.07f, yc + 0.07f, white);
                }

            // Vice City's end meets the seawall road (north - south) at VCS x -1833: a stop line on the eastbound half.
            // [bridge4] The chevron board of the old landing (y -656, in front of the airport fence) is gone: at y -407
            // the fence is a wall 10 m past the road, with nothing to warn of.
            mark(x1 - 2.5f, x1 - 2.0f, ry0 + 0.4f, y0 - 0.35f, white);

            // the parapets (the kerb boxes the game collides with): concrete, a steel rail on top on posts every 3 m
            const uint32_t concrete = rgb(176, 174, 166), steel = rgb(132, 140, 148), dark = rgb(104, 106, 108);
            for (int side = -1; side <= 1; side += 2)
            {
                const float yo = y0 + side * hw, yi = y0 + side * (hw - k);
                const float ya = std::min(yo, yi), yb = std::max(yo, yi);
                deckBeam(x0, x1, ya, yb, 0.0f, H, concrete, false);                 // the parapet (top, both faces)
                deckBeam(x0, x1, ya + 0.22f, yb - 0.22f, H + 0.62f, H + 0.72f, steel); // the top rail
                deckBeam(x0, x1, ya + 0.25f, yb - 0.25f, H + 0.30f, H + 0.36f, steel); // the middle rail
                for (float px = x0 + 1.5f; px < x1; px += 3.0f)
                {
                    const float z = deckZ(px) + H;
                    box(px - 0.05f, px + 0.05f, ya + 0.25f, yb - 0.25f, z, z + 0.66f, dark);
                }
                // the deck's edge below the parapet: a fascia down to the deck's underside
                deckBeam(x0, x1, side < 0 ? yo - 0.05f : yo, side < 0 ? yo : yo + 0.05f, -T, 0.0f, rgb(150, 152, 150), false);
            }
            // under the road: the slab and two box girders
            deckBeam(x0, x1, y0 - hw, y0 + hw, -T, -T + 0.01f, rgb(120, 122, 122), true);
            for (int side = -1; side <= 1; side += 2)
            {
                const float yc = y0 + side * (hw - 3.0f);
                deckBeam(x0 + 1.0f, x1 - 1.0f, yc - 1.2f, yc + 1.2f, -T - 1.4f, -T, rgb(142, 144, 140), true);
            }
            // the deck's ends
            for (int e = 0; e < 2; ++e)
            {
                const float x = e ? x1 : x0, z = deckZ(x);
                quadV(P(x, y0 - hw, z - T), P(x, y0 + hw, z - T), P(x, y0 + hw, z), P(x, y0 - hw, z), rgb(144, 150, 154));
            }

            // piers every 50 m over the sea (from x 1650, past the docks' pier), not at the towers: two columns on a
            // footing, a cap beam under the girders
            const uint32_t pierCol = rgb(158, 156, 148), footing = rgb(110, 112, 108);
            for (float px = 1520.0f; px < g_segs.back().x0 - 5.0f; px += 50.0f)
            {
                if (std::fabs(px - kTowerX[0]) < 20.0f || std::fabs(px - kTowerX[1]) < 20.0f)
                    continue;
                const float top = deckZ(px) - T - 1.4f;
                box(px - 2.0f, px + 2.0f, y0 - hw + 0.5f, y0 + hw - 0.5f, top - 1.2f, top, pierCol, 0, true); // cap
                for (int side = -1; side <= 1; side += 2)
                {
                    const float yc = y0 + side * (hw - 3.0f);
                    box(px - 1.1f, px + 1.1f, yc - 1.1f, yc + 1.1f, kSeaBed, top - 1.2f, pierCol);
                    box(px - 1.8f, px + 1.8f, yc - 1.8f, yc + 1.8f, kSeaBed, 0.6f, footing); // where it meets the sea
                }
            }

            // two cable-stayed towers: legs outside the parapets, cross beams under the deck and near the top, a red
            // warning light on each leg, eight stay cables a side fanning each way to the parapets
            const uint32_t towerCol = rgb(196, 192, 182), cableCol = rgb(214, 216, 220);
            for (float tx : kTowerX)
            {
                const float dz = deckZ(tx), ly = hw + 1.9f;
                for (int side = -1; side <= 1; side += 2)
                {
                    const float yc = y0 + side * ly;
                    box(tx - 1.4f, tx + 1.4f, yc - 1.2f, yc + 1.2f, kSeaBed, kTowerTop, towerCol);
                    box(tx - 2.2f, tx + 2.2f, yc - 2.0f, yc + 2.0f, kSeaBed, 0.8f, footing);
                    box(tx - 0.4f, tx + 0.4f, yc - 0.4f, yc + 0.4f, kTowerTop, kTowerTop + 0.8f, rgb(200, 30, 24), 2); // light
                }
                box(tx - 1.6f, tx + 1.6f, y0 - ly, y0 + ly, dz - T - 3.2f, dz - T, towerCol, 0, true); // under the deck
                box(tx - 1.0f, tx + 1.0f, y0 - ly, y0 + ly, kTowerTop - 9.0f, kTowerTop - 6.5f, towerCol, 0, true);
                box(tx - 1.0f, tx + 1.0f, y0 - ly, y0 + ly, kTowerTop - 26.0f, kTowerTop - 24.0f, towerCol, 0, true);
                for (int side = -1; side <= 1; side += 2)
                    for (int dir = -1; dir <= 1; dir += 2)
                        for (int c = 1; c <= 8; ++c)
                        {
                            const float ax = tx + dir * (8.0f + 9.5f * c);
                            if (ax < x0 + 4.0f || ax > x1 - 4.0f)
                                continue;
                            const V3 a = P(tx + dir * 0.9f, y0 + side * ly, kTowerTop - 2.0f - 1.6f * c);
                            const V3 b = P(ax, y0 + side * (hw - 0.3f), deckZ(ax) + H);
                            cable(a, b, 0.09f, cableCol);
                        }
            }

            // lamp posts on both parapets: a pole, an arm over the road, the head (lit at night)
            for (float lx : g_lampX)
                for (int side = -1; side <= 1; side += 2)
                {
                    const float z = deckZ(lx) + H, yp = y0 + side * (hw - k * 0.5f);
                    box(lx - 0.11f, lx + 0.11f, yp - 0.11f, yp + 0.11f, z, z + 8.0f, steel);
                    const float yh = y0 + side * (hw - 2.4f);
                    box(lx - 0.07f, lx + 0.07f, std::min(yp, yh), std::max(yp, yh), z + 7.8f, z + 7.95f, steel);
                    box(lx - 0.35f, lx + 0.35f, yh - 0.2f, yh + 0.2f, z + 7.6f, z + 7.8f, rgb(250, 236, 196), 1, true);
                }
        }

        // Camera: TheCamera's matrix (LCS 0x43BBF0, VCS 0x6F44D0: right +0, forward +0x10, up +0x20, position +0x30 - what
        // the game copies into the RW camera's frame) and its RwCamera (LCS TheCamera +0xBA0, VCS +0x7F4: found in RAM
        // dumps by the view window / its reciprocal / near / far): view window +0x60, near +0x78, far +0x7C, the fog
        // plane +0x80 (RwCamera's fogPlane after farPlane: used when it lies between near and far).
        struct View
        {
            float pos[3], r[3], f[3], u[3], vwx, vwy, n, fa, fog;
        };
        float g_fogRaw = 0.0f, g_nearNow = 0.0f, g_farNow = 0.0f; // (the status line)
        bool readView(const uint8_t *ram, View &v)
        {
            const uint32_t cam = game::isLcs() ? 0x0043BBF0u : 0x006F44D0u;
            const uint32_t rw = rd32(ram, cam + (game::isLcs() ? 0xBA0u : 0x7F4u));
            if (!ptr(rw))
                return false;
            auto f = [&](uint32_t a) {
                float x;
                std::memcpy(&x, ram + (a & kRamMask), 4);
                return x;
            };
            for (int i = 0; i < 3; ++i)
            {
                v.r[i] = f(cam + 0x00u + 4u * i);
                v.f[i] = f(cam + 0x10u + 4u * i);
                v.u[i] = f(cam + 0x20u + 4u * i);
                v.pos[i] = f(cam + 0x30u + 4u * i);
            }
            // [bridge2] screen right = forward x up, made here: the matrix's own right vector is RenderWare's (it points
            // to the screen's LEFT: the game's follow camera has right = -(forward x up); the trainer's free camera, which
            // the first tests used, wrote it the other way round) - taken as it was, the deck was drawn mirrored left to
            // right about the view's centre: it turned the wrong way with the camera and stood on the wrong side
            {
                float r[3] = {v.f[1] * v.u[2] - v.f[2] * v.u[1], v.f[2] * v.u[0] - v.f[0] * v.u[2], v.f[0] * v.u[1] - v.f[1] * v.u[0]};
                const float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
                if (!(rl > 1e-3f))
                    return false;
                for (int i = 0; i < 3; ++i)
                    v.r[i] = r[i] / rl;
                // up again square to both (the matrix may carry a little skew)
                const float u[3] = {v.r[1] * v.f[2] - v.r[2] * v.f[1], v.r[2] * v.f[0] - v.r[0] * v.f[2], v.r[0] * v.f[1] - v.r[1] * v.f[0]};
                const float ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
                for (int i = 0; i < 3; ++i)
                    v.u[i] = ul > 1e-3f ? u[i] / ul : v.u[i];
            }
            v.vwx = f(rw + 0x60u), v.vwy = f(rw + 0x64u), v.n = f(rw + 0x78u), v.fa = f(rw + 0x7Cu), v.fog = f(rw + 0x80u);
            g_fogRaw = v.fog;
            if (!(v.fog > v.n && v.fog < v.fa))
                v.fog = v.fa * 0.5f;
            g_nearNow = v.n, g_farNow = v.fa;
            return v.vwx > 0.05f && v.vwy > 0.05f && v.n > 0.0f && v.fa > v.n && std::isfinite(v.pos[0]);
        }

        float g_flipX = 1.0f; // STORIES_BRIDGE_FLIPX=1: the screen's x the other way (verification)

        // the light of the hour: the colours times 0.7 at night .. 1 at noon (the game's colours: 0x80 = 1.0, so full
        // white is 0x80 after this), set each frame from the clock (LCS 0x3D9B74 / 0x3D9B80, VCS 0x4CD148 / 9); g_night
        // 0 by day .. 1 at night: the lamps
        float g_light = 1.0f, g_night = 0.0f;
        uint32_t shade(uint32_t c) { return scaleRgb(c, g_light * 0.5f); }
        void setLight(const uint8_t *ram)
        {
            const bool lcs = game::isLcs();
            const float h = ram[(lcs ? 0x003D9B74u : 0x004CD148u) & kRamMask] + ram[(lcs ? 0x003D9B80u : 0x004CD149u) & kRamMask] / 60.0f;
            const float day = std::clamp(0.5f + 0.75f * std::cos((h - 13.0f) / 24.0f * 6.2831853f), 0.0f, 1.0f);
            g_light = 0.7f + 0.3f * day;
            g_night = std::clamp(1.0f - day * 2.5f, 0.0f, 1.0f); // lamps on from ~19:30 to ~6:30
        }
        // a vertex's colour now: lit by the hour, plus the lamps' light at night; lamps glow
        void vertColour(const Poly &q, int i, float out[3])
        {
            const uint32_t c = q.c[i];
            const float r = float(c & 0xFFu), g = float((c >> 8) & 0xFFu), b = float((c >> 16) & 0xFFu);
            float k = g_light * 0.5f;
            if (q.kind == 1)
                k = 0.5f * (g_light + (1.15f - g_light) * g_night); // the lamp's head: brighter than lit at night
            else if (q.kind == 2)
                k = 0.5f * (0.35f + 0.85f * g_night); // the warning light: dull red by day, bright at night
            out[0] = r * k, out[1] = g * k, out[2] = b * k;
            if (q.kind == 0 && q.glow[i] > 0.0f && g_night > 0.0f)
            {
                const float l = q.glow[i] * g_night * 0.6f; // warm sodium-ish light added
                out[0] += 150.0f * l, out[1] += 122.0f * l, out[2] += 64.0f * l;
            }
        }

        // the polygons in the camera's view, clipped at the near plane, as screen triangles
        void project(const View &v, std::vector<gsbridge::Vert> &out)
        {
            out.clear();
            const float nearClip = std::max(v.n, 0.3f);
            struct VP
            {
                float x, y, z; // view space: right, up, depth
                float c[3];
            };
            const float fogSpan = std::max(v.fa - v.fog, 1.0f);
            for (const Poly &q : g_polys)
            {
                VP in[4];
                bool anyFront = false;
                for (int i = 0; i < 4; ++i)
                {
                    const float d[3] = {q.p[i][0] - v.pos[0], q.p[i][1] - v.pos[1], q.p[i][2] - v.pos[2]};
                    in[i].x = d[0] * v.r[0] + d[1] * v.r[1] + d[2] * v.r[2];
                    in[i].y = d[0] * v.u[0] + d[1] * v.u[1] + d[2] * v.u[2];
                    in[i].z = d[0] * v.f[0] + d[1] * v.f[1] + d[2] * v.f[2];
                    anyFront |= in[i].z > nearClip;
                }
                if (!anyFront)
                    continue;
                // off screen to one side (all four corners beyond the same edge of the view): skipped
                {
                    int l = 0, rr = 0, t = 0, b = 0, far = 0;
                    for (int i = 0; i < 4; ++i)
                    {
                        const float z = std::max(in[i].z, nearClip);
                        l += in[i].x < -1.3f * v.vwx * z, rr += in[i].x > 1.3f * v.vwx * z;
                        b += in[i].y < -1.3f * v.vwy * z, t += in[i].y > 1.3f * v.vwy * z;
                        far += in[i].z > v.fa;
                    }
                    if (l == 4 || rr == 4 || t == 4 || b == 4 || far == 4)
                        continue;
                }
                for (int i = 0; i < 4; ++i)
                    vertColour(q, i, in[i].c);
                VP poly[8];
                int n = 0;
                for (int i = 0; i < 4; ++i)
                {
                    const VP &a = in[i], &b = in[(i + 1) & 3];
                    const bool ia = a.z > nearClip, ib = b.z > nearClip;
                    if (ia)
                        poly[n++] = a;
                    if (ia != ib)
                    {
                        const float t = (nearClip - a.z) / (b.z - a.z);
                        VP m{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, nearClip, {}};
                        for (int c = 0; c < 3; ++c)
                            m.c[c] = a.c[c] + (b.c[c] - a.c[c]) * t;
                        poly[n++] = m;
                    }
                }
                if (n < 3)
                    continue;
                gsbridge::Vert sv[8];
                for (int i = 0; i < n; ++i)
                {
                    const float iz = 1.0f / poly[i].z;
                    sv[i].x = 0.5f + 0.5f * g_flipX * (poly[i].x * iz) / v.vwx;
                    sv[i].y = 0.5f - 0.5f * (poly[i].y * iz) / v.vwy;
                    sv[i].depth = g_zR * v.n * iz + g_zB;
                    sv[i].w = poly[i].z;
                    sv[i].rgba = 0xFF000000u | uint32_t(std::min(255.0f, poly[i].c[2])) << 16 | uint32_t(std::min(255.0f, poly[i].c[1])) << 8 |
                                 uint32_t(std::min(255.0f, poly[i].c[0]));
                    // the game's fog: none to the fog plane, full at the far plane (the lamps' heads keep theirs)
                    sv[i].fog = q.kind ? 1.0f : std::clamp((v.fa - poly[i].z) / fogSpan, 0.0f, 1.0f);
                }
                for (int i = 1; i + 1 < n; ++i)
                {
                    out.push_back(sv[0]);
                    out.push_back(sv[i]);
                    out.push_back(sv[i + 1]);
                }
            }
        }
    }

    bool active() { return g_on && g_built; }

    float centerY(float lcsX)
    {
        return g_y + (g_on ? shiftAt(lcsX) : 0.0f);
    }

    void install(float ox, float oy, float oz)
    {
        if (const char *b = std::getenv("STORIES_BRIDGE"); b && b[0] == '0')
            return;
        g_off[0] = ox, g_off[1] = oy, g_off[2] = oz;
        G = game::isLcs() ? &kLcs : &kVcs;
        g_on = true;
        if (const char *z = std::getenv("STORIES_BRIDGE_Z"); z && *z)
            std::sscanf(z, "%f,%f", &g_zR, &g_zB);
        // the route's y (both games, LCS coordinates; north is +y): STORIES_BRIDGE_Y=-500 moves the whole bridge, ramps
        // and deck, to that line (the ramps keep their x and heights: check the shore there has room for them)
        if (const char *yy = std::getenv("STORIES_BRIDGE_Y"); yy && *yy)
            g_y = static_cast<float>(std::atof(yy));
        if (const char *dy = std::getenv("STORIES_BRIDGE_VCS_DY"); dy && *dy)
            g_vcsDy = static_cast<float>(std::atof(dy));
        makeRoute();
        if (const char *vz = std::getenv("STORIES_BRIDGE_VCS_Z"); vz && *vz)
            g_vcsZ = static_cast<float>(std::atof(vz));
        {
            // the ramp to the Vice City end: ends at g_vcsZ in Vice City's own heights at LCS x 1967 (VCS -1833). The deck's
            // heights are LCS's; Vice City draws them lifted by the dual-world offset's z (gz), so the end is g_vcsZ - oz
            // there: with the seas lined up (oz 5.6, [farsea]) it descends 4.65 m from the border's 13 over 270 m (1 deg;
            // nothing steeper for a car at 42 m/s), else (oz 0) it climbs 0.95 m at 0.7 deg as before
            const float zEnd = g_vcsZ - g_off[2], dz = zEnd - 13.0f;
            const float len = std::clamp(std::fabs(dz) / 0.0122f, 30.0f, 270.0f);
            g_segs[1].x1 = 1967.0f - len;
            float je1 = 0.0f, je0 = 0.0f; // a way down: the joint's end edges (STORIES_BRIDGE_JOINT=flat's e1,ramp's e0)
            if (const char *j = std::getenv("STORIES_BRIDGE_JOINT"); j && *j)
                std::sscanf(j, "%f,%f", &je1, &je0);
            if (dz < 0.0f)
                g_segs[1].e1 = je1;
            // a way down: the ramp's top starts this far under the deck's (STORIES_BRIDGE_DROP). Level, the flat box's front
            // edge stood bare above a ramp falling away from it and threw a car (measured, 30 m/s: +9 m/s up at the
            // joint, 45 m/s: +14.5; with a 15 cm step down nothing: vz within -1.1..0.8 up to 45 m/s)
            float drop = 0.15f;
            if (const char *d = std::getenv("STORIES_BRIDGE_DROP"); d && *d)
                drop = static_cast<float>(std::atof(d));
            g_segs[2] = {1967.0f - len, 1967.0f, dz < 0.0f ? 13.0f - drop : 13.0f, zEnd, dz < 0.0f ? je0 : 6.0f, 0.0f};
        }
        if (const char *fx = std::getenv("STORIES_BRIDGE_FLIPX"); fx && fx[0] == '1')
            g_flipX = -1.0f;
        if (const char *sy = std::getenv("STORIES_BRIDGE_SYNC"); sy && sy[0] == '0')
            g_sync = false;
        if (const char *d = std::getenv("STORIES_BRIDGE_DRAW"); !(d && d[0] == '0'))
            gsbridge::setSource([](std::vector<gsbridge::Vert> &out, uint64_t frameTag) {
                // [bridge2] the projection made with the camera of the frame being drawn, not the newest one - the game
                // thread runs a frame or two ahead of the frame the renderer draws (measured: 1.8 frames on average),
                // and the deck swam with the camera by that much
                std::shared_ptr<const Frame> f;
                {
                    std::lock_guard<std::mutex> lock(g_frameMutex);
                    // the frame being drawn, by its tag ([dwlag]: its world's native draws carry it); else (no tag yet,
                    // or `bridge sync 0`) the newest
                    const std::shared_ptr<const Frame> &t = g_tagged[frameTag % 16u];
                    if (g_sync && frameTag && t && t->tag == frameTag)
                        f = t, ++g_tagHits;
                    else
                        f = g_newestFrame, g_tagMisses += frameTag ? 1u : 0u;
                    if (f && g_newestFrame)
                    {
                        const uint64_t lag = g_newestFrame->seq - f->seq;
                        g_lagSum += lag, ++g_lagN, g_lagMax = std::max<uint64_t>(g_lagMax, lag);
                    }
                }
                if (!f || f->tris.empty())
                    return false;
                out = f->tris;
                return true;
            });
        ps2control::addCommand("bridge", [](const std::string &args, ps2control::Reply reply) {
            char buf[448];
            if (args.rfind("plane ", 0) == 0) // tests: a magenta square, `bridge plane X Y Z SIZE` (depth calibration)
            {
                float x = 0, y = 0, z = 0, s = 0;
                std::sscanf(args.c_str() + 6, "%f %f %f %f", &x, &y, &z, &s);
                std::lock_guard<std::mutex> lock(g_drawMutex);
                const float a[3] = {x - s, y - s, z}, b[3] = {x + s, y - s, z}, c[3] = {x + s, y + s, z}, d[3] = {x - s, y + s, z};
                addQuad(a, b, c, d, 0xFFFF00FFu);
                return reply("ok plane");
            }
            if (args == "clear") // tests: the test squares away (the deck is rebuilt)
            {
                std::lock_guard<std::mutex> lock(g_drawMutex);
                g_polys.clear();
                return reply("ok cleared");
            }
            if (args.rfind("z ", 0) == 0) // tests: the depth mapping, `bridge z R B`
            {
                std::sscanf(args.c_str() + 2, "%f %f", &g_zR, &g_zB);
                std::snprintf(buf, sizeof(buf), "ok z R %g B %g", g_zR, g_zB);
                return reply(buf);
            }
            if (args.rfind("sync ", 0) == 0) // tests: `bridge sync 0|1`, the frame's own projection or the newest
            {
                g_sync = args[5] != '0';
                std::lock_guard<std::mutex> lock(g_frameMutex);
                g_lagSum = g_lagN = g_lagMax = g_tagHits = g_tagMisses = 0;
                return reply(g_sync ? "ok sync 1" : "ok sync 0");
            }
            double lagAvg = 0.0;
            uint64_t lagMax = 0, tagHits = 0, tagMisses = 0;
            {
                std::lock_guard<std::mutex> lock(g_frameMutex);
                lagAvg = g_lagN ? double(g_lagSum) / double(g_lagN) : 0.0;
                lagMax = g_lagMax;
                tagHits = g_tagHits, tagMisses = g_tagMisses;
            }
            std::snprintf(buf, sizeof(buf), "ok on=%d built=%d failed=%d segments=%zu polys=%zu checks=%u relinks=%u sync=%d lag avg %.2f max %llu tag hits %llu misses %llu near %.3g far %.4g fogplane %.4g why=%s",
                          g_on ? 1 : 0, g_built ? 1 : 0, g_failed ? 1 : 0, g_ents.size(), g_polys.size(), g_checks.load(), g_relinks.load(),
                          g_sync ? 1 : 0, lagAvg, (unsigned long long)lagMax, (unsigned long long)tagHits, (unsigned long long)tagMisses, g_nearNow, g_farNow, g_fogRaw, g_why.c_str());
            reply(buf);
        });
        std::fprintf(stderr, "[bridge] on (%s): deck LCS x %.0f..%.0f at y %.0f\n", game::isLcs() ? "LCS" : "VCS", g_segs.front().x0,
                     g_segs.back().x1, g_y);
    }

    void onGameFrame(uint8_t *r, uint64_t frameTag)
    {
        if (!g_on || g_failed)
            return;
        ++g_frame;
        if (!g_built)
        {
            // in play for 2 s first (the player exists; not the title screen or a save being loaded: the world is
            // rebuilt then). LCS: CPlayerInfo ped *0x408E40; VCS *0x4E4910
            const uint32_t ped = rd32(r, game::isLcs() ? 0x00408E40u : 0x004E4910u);
            if (!gameReady() || !ptr(ped))
                g_readyFrames = 0;
            else if (++g_readyFrames > 120 && (g_frame % 30) == 0)
                build(r);
            return;
        }
        {
            View v;
            thread_local std::vector<gsbridge::Vert> tris;
            std::lock_guard<std::mutex> lock(g_drawMutex); // (the test command adds polygons)
            if (g_polys.empty())
                buildPolys();
            setLight(r);
            if (readView(r, v))
                project(v, tris);
            else
                tris.clear();
            // [bridge2] kept under this frame's tag: the renderer asks for the frame it draws (gs_bridge.h)
            auto f = std::make_shared<Frame>();
            f->seq = ++g_seq;
            f->tag = frameTag;
            f->tris = tris;
            {
                std::lock_guard<std::mutex> flock(g_frameMutex);
                g_newestFrame = f;
                if (frameTag)
                    g_tagged[frameTag % 16u] = f;
            }
        }
        if ((g_frame % 30) != 0)
            return;
        // still linked (a world restart rebuilds the lists), our model slots still ours
        g_checks.fetch_add(1, std::memory_order_relaxed);
        const uint32_t infos = rd32(r, G->modelInfos);
        for (Built &b : g_ents)
        {
            const int m = rd16(r, b.ent + G->modelOff);
            if (m <= 0 || rd32(r, infos + 4u * uint32_t(m)) == 0 || rd32(r, rd32(r, infos + 4u * uint32_t(m)) + 0x14) != b.col)
            {
                // the table was rebuilt: build again from scratch
                g_built = false;
                g_why = "model table changed: rebuilding";
                std::fprintf(stderr, "[bridge] %s\n", g_why.c_str());
                return;
            }
            for (size_t k = 0; k < b.nodes.size(); ++k)
                if (!inList(r, b.heads[k], b.nodes[k]))
                {
                    hang(r, b.nodes[k], b.ent, b.heads[k]);
                    g_relinks.fetch_add(1, std::memory_order_relaxed);
                }
        }
    }
}

// [hero] One hero in both cities: see stories_hero.h and docs/STORIES.md "One hero in both cities".
//
// How Liberty City Stories finds its player model (SLUS_214.23, read 2026-11-18):
// - CStreaming::RequestSpecialModel 0x18FF80 (id, name, flags): GTA III's. It hashes the name (0x1838D8) and compares
//   it with model info +8; if different it looks the name up in the special-model directory, a CDirectory at
//   *(streamInfo + 0x1E328) (streamInfo = *0x3D0CC8, gp - 0x6228; CDirectory::FindItem 0x1450D8: {entries, max,
//   count}, entries 0x20 bytes {sector in GTA3PS2.IMG, sectors, name[24]}), writes the sector / size into the model's
//   streaming entry (0x18A3C0: entry + 0xC / + 0x10 of the 0x14-byte entry at streamInfo + 4 + id * 0x14) and sets the
//   model's texture list by the same name (0x1B7B18 -> CTxdStore::FindTxdSlot 0x2EAC00: the pool *0x3D7CA0 = {slots,
//   flags, count}, 0x1C bytes per slot, the name at +8; a texture list's streaming index is 4900 + its slot).
// - The directory in GAME.DTZ (160 names): "player" (the default outfit), "plr", "plr2".."plr16", "player2".."player12"
//   (Toni's gameplay outfits), "csplr*" / "csplayer*" / "cstoni_a" (cutscene models), "playersdoor", ... The texture
//   pool has a slot of each outfit's name.
// So no code in the ped path changes: once the tables exist (the mod loader's model-info setup hook, vcs_mods
// onTables), every gameplay outfit's directory entry is pointed at Vic's model and its texture list's streaming entry
// at Vic's textures, both served from host memory past the disc's end (vcs_mods serveBlob). Cutscene names are left
// alone: cutscenes keep Toni (and their own cs models).
#include "game/stories_hero.h"

#include "game/game_table.h"
#include "game/vcs_modfiles.h"
#include "game/vcs_mods.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace stories::hero
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        // LCS addresses (evidence in the header comment above)
        constexpr uint32_t kLcsStreamInfoPtr = 0x003D0CC8u; // CStreaming's info array (gp - 0x6228; vcs_mods kLcsStreamInfoPtr)
        constexpr uint32_t kLcsSpecialDirField = 0x1E328u;  // streamInfo + this -> the special-model CDirectory
        constexpr uint32_t kLcsTxdPoolPtr = 0x003D7CA0u;    // CTxdStore's pool {slots, flags, count}
        constexpr uint32_t kLcsTxdSlotSize = 0x1Cu;
        constexpr uint32_t kLcsRequestSpecialModel = 0x0018FF80u;

        std::string g_wanted = "city";
        bool g_active = false, g_log = false;
        PS2Runtime::RecompiledFunction g_origRequestSpecial = nullptr;

        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        void wr32(uint8_t *ram, uint32_t a, uint32_t v) { std::memcpy(ram + (a & kRamMask), &v, 4); }
        std::string str(const uint8_t *ram, uint32_t a, size_t n)
        {
            std::string s;
            for (size_t i = 0; i < n; ++i)
            {
                const char c = static_cast<char>(ram[(a + i) & kRamMask]);
                if (!c)
                    break;
                s += c;
            }
            return s;
        }
        std::string lower(std::string s)
        {
            for (char &c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        // Toni's gameplay outfits: "player", "playerN", "plr", "plrN" (not "playersdoor", not the cutscenes' cs*)
        bool isOutfit(const std::string &name)
        {
            const std::string n = lower(name);
            for (const char *p : {"player", "plr"})
            {
                const size_t k = std::strlen(p);
                if (n.compare(0, k, p) != 0)
                    continue;
                bool digits = true;
                for (size_t i = k; i < n.size(); ++i)
                    digits &= std::isdigit(static_cast<unsigned char>(n[i])) != 0;
                if (digits)
                    return true;
            }
            return false;
        }

        std::filesystem::path modsDir() { return PS2Runtime::getIoPaths().mcRoot.parent_path() / "mods"; }

        // active.txt's `hero name=<name> mdl=<file> xtx=<file> [outfits=<file>]` for `name` (tools/stories_pack.py hero)
        struct Pack
        {
            std::string mod, mdl, xtx, outfits;
        };
        bool findPack(const std::string &name, Pack &p)
        {
            std::ifstream in(modsDir() / "active.txt");
            std::string line, cur = "?";
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                std::istringstream s(line);
                std::string kind, tok, n;
                Pack q;
                if (!(s >> kind) || kind[0] == '#')
                    continue;
                if (kind == "mod")
                {
                    s >> cur;
                    continue;
                }
                if (kind != "hero")
                    continue;
                while (s >> tok)
                {
                    if (tok.rfind("name=", 0) == 0)
                        n = tok.substr(5);
                    else if (tok.rfind("mdl=", 0) == 0)
                        q.mdl = tok.substr(4);
                    else if (tok.rfind("xtx=", 0) == 0)
                        q.xtx = tok.substr(4);
                    else if (tok.rfind("outfits=", 0) == 0)
                        q.outfits = tok.substr(8);
                }
                if (n == name && !q.mdl.empty() && !q.xtx.empty())
                {
                    q.mod = cur;
                    p = q;
                    return true;
                }
            }
            return false;
        }

        bool readFile(const std::string &value, const char *magic, std::vector<uint8_t> &bytes)
        {
            const vcs::mods::files::Location l = vcs::mods::files::resolve(value, modsDir());
            std::string err;
            if (!l.error.empty() || !vcs::mods::files::read(l, bytes, 2u << 20, err))
            {
                std::fprintf(stderr, "[hero] %s: %s\n", l.shown.c_str(), (l.error.empty() ? err : l.error).c_str());
                return false;
            }
            if (bytes.size() < 0x40 || std::memcmp(bytes.data(), magic, 4) != 0)
            {
                std::fprintf(stderr, "[hero] %s: not a %s chunk\n", l.shown.c_str(), magic);
                return false;
            }
            return true;
        }

        // The pack's outfits, served: this game's outfit name (lower case) -> {sector, sectors} of the hero's model and
        // of his textures for it; "*" = his default. Each file is served once (vcs_mods serveBlob).
        struct Served
        {
            uint32_t mdlSector = 0, mdlCount = 0, xtxSector = 0, xtxCount = 0;
        };
        std::map<std::string, Served> g_outfits;
        std::map<std::string, std::pair<uint32_t, uint32_t>> g_servedFiles;

        bool serveFile(const std::string &file, const char *magic, uint32_t &sector, uint32_t &count)
        {
            if (auto it = g_servedFiles.find(file); it != g_servedFiles.end())
                return sector = it->second.first, count = it->second.second, sector != 0;
            std::vector<uint8_t> bytes;
            sector = count = 0;
            if (readFile(file, magic, bytes))
                sector = vcs::mods::serveBlob(std::move(bytes), count);
            g_servedFiles[file] = {sector, count};
            return sector != 0;
        }

        // outfits.txt: "<outfit> <model file> <texture file>" per line ("*": the default); without one, the directive's
        // mdl / xtx for every outfit
        bool loadOutfits(const Pack &p)
        {
            g_outfits.clear();
            std::vector<std::array<std::string, 3>> rows;
            if (!p.outfits.empty())
            {
                std::vector<uint8_t> text;
                std::string err;
                const vcs::mods::files::Location l = vcs::mods::files::resolve(p.outfits, modsDir());
                if (l.error.empty() && vcs::mods::files::read(l, text, 1u << 16, err))
                {
                    std::istringstream in(std::string(text.begin(), text.end()));
                    std::string line;
                    while (std::getline(in, line))
                    {
                        std::istringstream s(line);
                        std::array<std::string, 3> r;
                        if ((s >> r[0] >> r[1] >> r[2]) && r[0][0] != '#')
                            rows.push_back({lower(r[0]), r[1], r[2]});
                    }
                }
                else
                    std::fprintf(stderr, "[hero] %s: %s (the default outfit everywhere)\n", l.shown.c_str(),
                                 (l.error.empty() ? err : l.error).c_str());
            }
            rows.push_back({"*", p.mdl, p.xtx});
            // STORIES_HERO_OUTFIT=<one of the hero's own outfits, e.g. plr6>: that one for every outfit (the pack's
            // hero_<name>.mdl / .xtx next to its default files; tests, and a fixed look)
            if (const char *o = std::getenv("STORIES_HERO_OUTFIT"); o && *o)
            {
                const size_t slash = p.mdl.find_last_of('/');
                const std::string dir = slash == std::string::npos ? std::string() : p.mdl.substr(0, slash + 1);
                const std::string name = lower(o);
                if (name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos)
                {
                    rows.clear();
                    rows.push_back({"*", dir + "hero_" + name + ".mdl", dir + "hero_" + name + ".xtx"});
                    rows.push_back({"*", p.mdl, p.xtx}); // (not found: the default)
                    std::fprintf(stderr, "[hero] STORIES_HERO_OUTFIT=%s: that outfit everywhere\n", name.c_str());
                }
            }
            for (const auto &r : rows)
            {
                Served s;
                if (g_outfits.count(r[0]) || !serveFile(r[1], "ldm", s.mdlSector, s.mdlCount) ||
                    !serveFile(r[2], "xet", s.xtxSector, s.xtxCount))
                    continue;
                g_outfits[r[0]] = s;
            }
            return g_outfits.count("*") != 0;
        }

        const Served &outfitFor(const std::string &name)
        {
            const auto it = g_outfits.find(lower(name));
            return it != g_outfits.end() ? it->second : g_outfits.at("*");
        }

        // vcs_mods onTables (LCS, game thread): GAME.DTZ is relocated, nothing streamed yet
        void lcsTables(uint8_t *ram)
        {
            Pack pack;
            if (!findPack(g_wanted, pack))
            {
                std::fprintf(stderr, "[hero] STORIES_HERO=%s but no hero pack for him (tools/stories_pack.py hero <VCS iso> "
                                     "--game lcs; the mod \"Vic Vance (from your VCS disc)\" on): Toni stays\n",
                             g_wanted.c_str());
                return;
            }
            const uint32_t streamInfo = rd32(ram, kLcsStreamInfoPtr) & kRamMask;
            const uint32_t dir = streamInfo ? rd32(ram, streamInfo + kLcsSpecialDirField) & kRamMask : 0;
            const uint32_t pool = rd32(ram, kLcsTxdPoolPtr) & kRamMask;
            if (!dir || !pool || !vcs::mods::streamEntry(0))
            {
                std::fprintf(stderr, "[hero] the special-model directory / texture pool not found (0x%x / 0x%x): Toni stays\n", dir, pool);
                return;
            }
            if (!loadOutfits(pack))
            {
                std::fprintf(stderr, "[hero] the pack could not be read or served: Toni stays\n");
                return;
            }
            const uint32_t entries = rd32(ram, dir) & kRamMask, count = std::min<uint32_t>(rd32(ram, dir + 8), 4096u);
            int models = 0, textures = 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t e = entries + i * 0x20u;
                const std::string n = str(ram, e + 8, 24);
                if (!isOutfit(n))
                    continue;
                const Served &s = outfitFor(n);
                wr32(ram, e, s.mdlSector);
                wr32(ram, e + 4, s.mdlCount);
                ++models;
            }
            const uint32_t slots = rd32(ram, pool) & kRamMask, flags = rd32(ram, pool + 4) & kRamMask;
            const uint32_t nslots = std::min<uint32_t>(rd32(ram, pool + 8), 8192u);
            for (uint32_t i = 0; i < nslots; ++i)
            {
                if (ram[(flags + i) & kRamMask] & 0x80u)
                    continue;
                const std::string n = str(ram, slots + i * kLcsTxdSlotSize + 8, 24);
                const uint32_t se = isOutfit(n) ? vcs::mods::streamEntry(vcs::mods::textureListBase() + i) : 0;
                if (!se)
                    continue;
                const Served &s = outfitFor(n);
                wr32(ram, se + 0x10, s.xtxSector);
                wr32(ram, se + 0x14, s.xtxCount);
                ++textures;
            }
            g_active = models > 0 && textures > 0;
            std::fprintf(stderr, "[hero] Vic Vance (%s) for Toni's outfits: %d models, %d texture lists, %zu of his outfits "
                                 "(%zu files served); cutscenes keep Toni\n",
                         pack.mod.c_str(), models, textures, g_outfits.size() - 1, g_servedFiles.size());
        }

        // VCS: the player's outfits are ordinary models (Undress 0x214900 looks the outfit's name up as a model,
        // 0x1A7C50, and requests it): model 0 "plr" and 155..167 "plr2".."plr14" (their model info +8 = the names'
        // keys; read from GAME.DTZ 2026-11-18), texture lists 5 and 153..165 (model info +0x1E). Their streaming
        // entries are pointed at Toni's model / textures for that outfit (vcs_mods onTables, at GAME.DTZ's relocation).
        void vcsTables(uint8_t *ram)
        {
            Pack pack;
            if (!findPack(g_wanted, pack))
            {
                std::fprintf(stderr, "[hero] STORIES_HERO=%s but no hero pack for him (tools/stories_pack.py hero <LCS iso> "
                                     "--game vcs; the mod \"Toni Cipriani (from your LCS disc)\" on): Vic stays\n",
                             g_wanted.c_str());
                return;
            }
            if (!loadOutfits(pack))
            {
                std::fprintf(stderr, "[hero] the pack could not be read or served: Vic stays\n");
                return;
            }
            int models = 0, textures = 0;
            auto point = [&](uint32_t index, uint32_t sector, uint32_t count) {
                const uint32_t se = vcs::mods::streamEntry(index);
                if (!se || !rd32(ram, se + 0x14))
                    return 0;
                wr32(ram, se + 0x10, sector);
                wr32(ram, se + 0x14, count);
                return 1;
            };
            for (uint32_t k = 1; k <= 14; ++k) // plr, plr2 .. plr14
            {
                const Served &s = outfitFor(k == 1 ? "plr" : "plr" + std::to_string(k));
                models += point(k == 1 ? 0u : 153u + k, s.mdlSector, s.mdlCount);
                textures += point(vcs::mods::textureListBase() + (k == 1 ? 5u : 151u + k), s.xtxSector, s.xtxCount);
            }
            g_active = models > 0 && textures > 0;
            std::fprintf(stderr, "[hero] Toni Cipriani (%s) for Vic's outfits: %d models, %d texture lists, %zu of his outfits "
                                 "(%zu files served); cutscenes keep Vic\n",
                         pack.mod.c_str(), models, textures, g_outfits.size() - 1, g_servedFiles.size());
        }

        // RequestSpecialModel: logged (STORIES_HERO_LOG=1), unchanged
        PS2_GUEST_CC void requestSpecial(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            std::fprintf(stderr, "[hero] RequestSpecialModel(%d, \"%s\", 0x%x)\n", static_cast<int>(GPR_U32(ctx, 4)),
                         str(rdram, GPR_U32(ctx, 5), 24).c_str(), GPR_U32(ctx, 6));
            g_origRequestSpecial(rdram, ctx, runtime);
        }
    }

    std::string wanted() { return g_wanted; }
    bool active() { return g_active; }

    void install(PS2Runtime &runtime)
    {
        if (const char *h = std::getenv("STORIES_HERO"); h && *h)
            g_wanted = lower(h);
        if (g_wanted != "vic" && g_wanted != "toni")
            g_wanted = "city";
        const char *log = std::getenv("STORIES_HERO_LOG");
        g_log = log && log[0] == '1';
        if (!game::isLcs())
        {
            if (g_wanted == "toni")
            {
                vcs::mods::onTables(&vcsTables);
                std::fprintf(stderr, "[hero] STORIES_HERO=toni: Toni Cipriani plays Vice City (when the hero pack is there)\n");
            }
            return;
        }
        if (g_log && (g_origRequestSpecial = runtime.lookupFunction(kLcsRequestSpecialModel)))
            runtime.replaceFunction(kLcsRequestSpecialModel, &requestSpecial);
        if (g_wanted != "vic")
            return; // Toni is LCS's own hero
        vcs::mods::onTables(&lcsTables);
        std::fprintf(stderr, "[hero] STORIES_HERO=vic: Vic Vance plays Liberty City (when the hero pack is there)\n");
    }
}

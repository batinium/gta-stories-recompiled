// See bridge_points.h.
#include "game/bridge_points.h"

#include "game/dualworld.h"
#include "game/game_addrs.h"
#include "game/game_table.h"
#include "ps2_runtime.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace game::bridgepoints
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        // the same fields lcs_trainer.cpp / vcs_trainer.cpp read for "Save position"
        constexpr uint32_t kVcsPlayerPed = 0x004E4910u, kVcsPedVehicle = 0x480u, kVcsPedState = 0x8C4u;
        constexpr uint32_t kLcsPedState = 0x34Cu;
        constexpr uint32_t kPosition = 0x30u;

        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        float rdf(const uint8_t *ram, uint32_t a)
        {
            float v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }

        // LCS x = this game's x - offset (VCS: x + 3800); y, z are the same
        void lcsOf(const float p[3], float out[3])
        {
            float off[3];
            dualworld::offset(off);
            for (int i = 0; i < 3; ++i)
                out[i] = game::isLcs() ? p[i] : p[i] - off[i];
        }
    }

    bool playerPosition(const uint8_t *ram, float out[3])
    {
        if (!ram)
            return false;
        uint32_t ped, veh = 0;
        if (game::isLcs())
        {
            const game::Addrs &G = game::addrs();
            ped = rd32(ram, G.world.playerPed);
            if (!ped)
                return false;
            if (rd32(ram, ped + G.off.pedInVehicle) && rd32(ram, ped + kLcsPedState) == 0x32)
                veh = rd32(ram, ped + G.off.pedVehicle);
        }
        else
        {
            ped = rd32(ram, kVcsPlayerPed);
            if (!ped)
                return false;
            if (rd32(ram, ped + kVcsPedVehicle) && rd32(ram, ped + kVcsPedState) == 0x37)
                veh = rd32(ram, ped + kVcsPedVehicle);
        }
        const uint32_t at = veh ? veh : ped;
        for (int i = 0; i < 3; ++i)
            out[i] = rdf(ram, at + kPosition + 4u * i);
        return true;
    }

    std::string positionLine(const float p[3])
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "POS %.1f %.1f %.1f", p[0], p[1], p[2]);
        std::string s = buf;
        if (!game::isLcs() && dualworld::role() != dualworld::Role::Off)
        {
            float l[3];
            lcsOf(p, l);
            std::snprintf(buf, sizeof(buf), "  (LCS %.1f %.1f)", l[0], l[1]);
            s += buf;
        }
        return s;
    }

    bool note(const float p[3])
    {
        namespace fs = std::filesystem;
        const fs::path dir = PS2Runtime::getIoPaths().mcRoot.parent_path() / "stories";
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::ofstream f(dir / "bridge_points.txt", std::ios::app);
        if (!f)
            return false;
        float l[3];
        lcsOf(p, l);
        const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char when[32];
        std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
        char line[256];
        std::snprintf(line, sizeof(line), "%s %s %.1f %.1f %.1f (LCS %.1f %.1f %.1f)\n", when, game::isLcs() ? "LCS" : "VCS", p[0], p[1],
                      p[2], l[0], l[1], l[2]);
        f << line;
        return static_cast<bool>(f);
    }
}

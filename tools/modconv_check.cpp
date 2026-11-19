// [android] PC harness for the model converter (src/game/vcs_modconv.cpp), no game, no device:
//
//   c++ -std=c++17 -O2 -Isrc tools/modconv_check.cpp src/game/vcs_modconv.cpp -o build/modconv_check
//   build/modconv_check <donor.ldm> <donor.xet> <model.dff> <model.txd> <model id> <out prefix> [max tris] [max tex]
//
// The donor entries are the stock model / texture list the conversion replaces, cut from the player's own
// GTA3PS2.IMG; the .dff / .txd are a PC mod's. Both are other people's data: keep them and the outputs out of the
// repository. Writes <out>.ldm / <out>.xet and prints the converter's report and a read-back of both models.
#include "game/vcs_modconv.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static std::vector<uint8_t> load(const char *path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static void save(const std::string &path, const std::vector<uint8_t> &b)
{
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
}

int main(int argc, char **argv)
{
    if (argc < 7)
    {
        std::fprintf(stderr, "usage: %s donor.ldm donor.xet model.dff model.txd modelId outPrefix [maxTris] [maxTex]\n", argv[0]);
        return 2;
    }
    vcs::modconv::Options o;
    if (argc > 7)
        o.maxTriangles = std::atoi(argv[7]);
    if (argc > 8)
        o.maxTexture = std::atoi(argv[8]);
    vcs::modconv::Result r;
    std::string error, dump;
    const std::vector<uint8_t> donor = load(argv[1]);
    const int id = std::atoi(argv[5]);
    // VCS's vehicle model ids; [lcs] MODCONV_KIND=vehicle / simple says it for another game's ids (LCS: 130..216)
    const char *kind = std::getenv("MODCONV_KIND");
    const char *gameName = std::getenv("MODCONV_GAME"); // [lcs] lcs: LCS's chunk dialect (Options::lcs)
    o.lcs = gameName && std::string(gameName) == "lcs";
    if (const char *part = std::getenv("MODCONV_PART")) // one weapon of a clump (Options::part)
        o.part = part;
    const bool vehicle = kind ? std::string(kind) == "vehicle" : id >= 170 && id <= 280;
    if (!vehicle)
        std::printf("stock model: %d triangles\n%s", vcs::modconv::countTriangles(donor, &dump), dump.c_str());
    if (vehicle && argc <= 7)
        o.maxTriangles = 3000, o.maxTexture = 128;
    if (!(vehicle ? vcs::modconv::convertVehicle : vcs::modconv::convertSimple)(donor, load(argv[2]), load(argv[3]), load(argv[4]), id, o,
                                                                                 r, error))
    {
        std::printf("refused: %s\n", error.c_str());
        return 1;
    }
    std::printf("%s", r.report.c_str());
    dump.clear();
    if (!vehicle)
        std::printf("read back: %d triangles\n%s", vcs::modconv::countTriangles(r.mdl, &dump), dump.c_str());
    save(std::string(argv[6]) + ".ldm", r.mdl);
    save(std::string(argv[6]) + ".xet", r.xet);
    return 0;
}

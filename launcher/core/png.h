// A PNG writer for screenshots (--screenshot): RGBA8, stored (uncompressed) deflate blocks. No library.
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace core
{
    bool writePng(const std::filesystem::path &path, int width, int height, const uint8_t *rgba);
    // The Saves sheet's pictures: 8-bit grey / RGB / palette / RGBA, not interlaced; rgba is width * height * 4, top-down.
    bool readPng(const std::filesystem::path &path, int &width, int &height, std::vector<uint8_t> &rgba);
}

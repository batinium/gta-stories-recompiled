#include "png.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace core
{
    namespace
    {
        uint32_t crc32(const uint8_t *data, size_t size, uint32_t crc = 0)
        {
            static uint32_t table[256];
            if (!table[1])
                for (uint32_t i = 0; i < 256; ++i)
                {
                    uint32_t c = i;
                    for (int k = 0; k < 8; ++k)
                        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    table[i] = c;
                }
            crc = ~crc;
            for (size_t i = 0; i < size; ++i)
                crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
            return ~crc;
        }

        // ---- inflate (RFC 1951), for readPng ----
        struct Bits
        {
            const uint8_t *p;
            size_t n, at = 0;
            uint32_t buf = 0;
            int cnt = 0;
            bool bad = false;
            int bit()
            {
                if (!cnt)
                {
                    if (at >= n)
                        return bad = true, 0;
                    buf = p[at++];
                    cnt = 8;
                }
                const int b = buf & 1;
                buf >>= 1;
                --cnt;
                return b;
            }
            uint32_t bits(int k)
            {
                uint32_t v = 0;
                for (int i = 0; i < k; ++i)
                    v |= uint32_t(bit()) << i;
                return v;
            }
        };

        struct Huff
        {
            uint16_t count[16] = {}, symbol[320] = {};
            void build(const uint8_t *lengths, int n)
            {
                std::fill(std::begin(count), std::end(count), uint16_t(0));
                for (int i = 0; i < n; ++i)
                    ++count[lengths[i]];
                count[0] = 0;
                uint16_t offs[16] = {};
                for (int i = 1; i < 15; ++i)
                    offs[i + 1] = uint16_t(offs[i] + count[i]);
                for (int i = 0; i < n; ++i)
                    if (lengths[i])
                        symbol[offs[lengths[i]]++] = uint16_t(i);
            }
            int decode(Bits &b) const
            {
                int code = 0, first = 0, index = 0;
                for (int len = 1; len < 16; ++len)
                {
                    code |= b.bit();
                    const int c = count[len];
                    if (code - c < first)
                        return symbol[index + (code - first)];
                    index += c;
                    first = (first + c) << 1;
                    code <<= 1;
                    if (b.bad)
                        return -1;
                }
                return -1;
            }
        };

        bool inflate(const uint8_t *data, size_t size, std::vector<uint8_t> &out)
        {
            static const uint16_t lenBase[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
            static const uint8_t lenExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
            static const uint16_t distBase[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
            static const uint8_t distExtra[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
            if (size < 2)
                return false;
            Bits b{data + 2, size - 2}; // past the zlib header
            for (int last = 0; !last;)
            {
                last = b.bit();
                const uint32_t type = b.bits(2);
                if (b.bad)
                    return false;
                if (type == 0)
                {
                    b.cnt = 0;
                    if (b.at + 4 > b.n)
                        return false;
                    const size_t len = b.p[b.at] | (b.p[b.at + 1] << 8);
                    b.at += 4;
                    if (b.at + len > b.n)
                        return false;
                    out.insert(out.end(), b.p + b.at, b.p + b.at + len);
                    b.at += len;
                    continue;
                }
                if (type == 3)
                    return false;
                Huff lit, dist;
                if (type == 1)
                {
                    uint8_t l[320];
                    for (int i = 0; i < 144; ++i) l[i] = 8;
                    for (int i = 144; i < 256; ++i) l[i] = 9;
                    for (int i = 256; i < 280; ++i) l[i] = 7;
                    for (int i = 280; i < 288; ++i) l[i] = 8;
                    lit.build(l, 288);
                    uint8_t d[30];
                    std::fill(d, d + 30, uint8_t(5));
                    dist.build(d, 30);
                }
                else
                {
                    const int nlen = int(b.bits(5)) + 257, ndist = int(b.bits(5)) + 1, ncode = int(b.bits(4)) + 4;
                    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                    uint8_t l[320] = {};
                    for (int i = 0; i < ncode; ++i)
                        l[order[i]] = uint8_t(b.bits(3));
                    Huff cl;
                    cl.build(l, 19);
                    uint8_t lengths[320] = {};
                    for (int i = 0; i < nlen + ndist;)
                    {
                        const int sym = cl.decode(b);
                        if (sym < 0 || b.bad)
                            return false;
                        if (sym < 16)
                            lengths[i++] = uint8_t(sym);
                        else
                        {
                            int rep, val = 0;
                            if (sym == 16)
                            {
                                if (!i)
                                    return false;
                                val = lengths[i - 1];
                                rep = 3 + int(b.bits(2));
                            }
                            else if (sym == 17)
                                rep = 3 + int(b.bits(3));
                            else
                                rep = 11 + int(b.bits(7));
                            if (i + rep > nlen + ndist)
                                return false;
                            while (rep--)
                                lengths[i++] = uint8_t(val);
                        }
                    }
                    lit.build(lengths, nlen);
                    dist.build(lengths + nlen, ndist);
                }
                for (;;)
                {
                    const int sym = lit.decode(b);
                    if (sym < 0 || b.bad)
                        return false;
                    if (sym < 256)
                        out.push_back(uint8_t(sym));
                    else if (sym == 256)
                        break;
                    else
                    {
                        const int s = sym - 257;
                        if (s >= 29)
                            return false;
                        const int len = lenBase[s] + int(b.bits(lenExtra[s]));
                        const int ds = dist.decode(b);
                        if (ds < 0 || ds >= 30)
                            return false;
                        const size_t d = distBase[ds] + b.bits(distExtra[ds]);
                        if (d > out.size())
                            return false;
                        for (int i = 0; i < len; ++i)
                            out.push_back(out[out.size() - d]);
                    }
                }
            }
            return true;
        }

        uint32_t get32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

        void put32(std::vector<uint8_t> &out, uint32_t v)
        {
            out.push_back(uint8_t(v >> 24));
            out.push_back(uint8_t(v >> 16));
            out.push_back(uint8_t(v >> 8));
            out.push_back(uint8_t(v));
        }

        void chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data)
        {
            put32(out, uint32_t(data.size()));
            const size_t start = out.size();
            out.insert(out.end(), type, type + 4);
            out.insert(out.end(), data.begin(), data.end());
            put32(out, crc32(out.data() + start, out.size() - start));
        }
    }

    bool writePng(const std::filesystem::path &path, int width, int height, const uint8_t *rgba)
    {
        std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        std::vector<uint8_t> header;
        put32(header, uint32_t(width));
        put32(header, uint32_t(height));
        header.insert(header.end(), {8, 6, 0, 0, 0}); // 8 bits, RGBA, deflate, no filter method, no interlace
        chunk(png, "IHDR", header);

        // scanlines with filter byte 0, in a zlib stream of stored blocks
        const size_t row = size_t(width) * 4;
        std::vector<uint8_t> raw;
        raw.reserve((row + 1) * size_t(height));
        for (int y = 0; y < height; ++y)
        {
            raw.push_back(0);
            raw.insert(raw.end(), rgba + row * size_t(y), rgba + row * size_t(y + 1));
        }
        std::vector<uint8_t> z = {0x78, 0x01};
        for (size_t at = 0; at < raw.size() || raw.empty();)
        {
            const size_t n = std::min<size_t>(raw.size() - at, 65535);
            const bool last = at + n == raw.size();
            z.push_back(last ? 1 : 0);
            z.push_back(uint8_t(n));
            z.push_back(uint8_t(n >> 8));
            z.push_back(uint8_t(~n));
            z.push_back(uint8_t(~n >> 8));
            z.insert(z.end(), raw.begin() + ptrdiff_t(at), raw.begin() + ptrdiff_t(at + n));
            at += n;
            if (last)
                break;
        }
        uint32_t a = 1, b = 0; // Adler-32
        for (uint8_t c : raw)
        {
            a = (a + c) % 65521;
            b = (b + a) % 65521;
        }
        put32(z, (b << 16) | a);
        chunk(png, "IDAT", z);
        chunk(png, "IEND", {});

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(png.data()), std::streamsize(png.size()));
        return bool(out);
    }

    bool readPng(const std::filesystem::path &path, int &width, int &height, std::vector<uint8_t> &rgba)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        const std::vector<uint8_t> f((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        if (f.size() < 33 || !std::equal(sig, sig + 8, f.begin()))
            return false;
        int w = 0, h = 0, depth = 0, type = 0, interlace = 0;
        std::vector<uint8_t> idat, palette;
        for (size_t at = 8; at + 12 <= f.size();)
        {
            const size_t len = get32(&f[at]);
            const std::string name(reinterpret_cast<const char *>(&f[at + 4]), 4);
            if (at + 12 + len > f.size())
                return false;
            const uint8_t *d = &f[at + 8];
            if (name == "IHDR" && len >= 13)
            {
                w = int(get32(d));
                h = int(get32(d + 4));
                depth = d[8];
                type = d[9];
                interlace = d[12];
            }
            else if (name == "PLTE")
                palette.assign(d, d + len);
            else if (name == "IDAT")
                idat.insert(idat.end(), d, d + len);
            else if (name == "IEND")
                break;
            at += 12 + len;
        }
        const int channels = type == 0 ? 1 : type == 2 ? 3 : type == 3 ? 1 : type == 4 ? 2 : type == 6 ? 4 : 0;
        if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || depth != 8 || !channels || interlace || idat.empty() ||
            (type == 3 && palette.size() < 3))
            return false;
        std::vector<uint8_t> raw;
        raw.reserve((size_t(w) * channels + 1) * size_t(h));
        if (!inflate(idat.data(), idat.size(), raw) || raw.size() < (size_t(w) * channels + 1) * size_t(h))
            return false;
        const size_t row = size_t(w) * channels;
        std::vector<uint8_t> prev(row, 0), cur(row);
        rgba.assign(size_t(w) * size_t(h) * 4, 255);
        for (int y = 0; y < h; ++y)
        {
            const uint8_t *line = &raw[size_t(y) * (row + 1)];
            const int filter = line[0];
            ++line;
            for (size_t i = 0; i < row; ++i)
            {
                const int a = i >= size_t(channels) ? cur[i - channels] : 0, b = prev[i],
                          c = i >= size_t(channels) ? prev[i - channels] : 0;
                int v = line[i];
                switch (filter)
                {
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                case 4:
                {
                    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                    v += (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c;
                    break;
                }
                default: break;
                }
                cur[i] = uint8_t(v);
            }
            uint8_t *o = &rgba[size_t(y) * size_t(w) * 4];
            for (int x = 0; x < w; ++x, o += 4)
            {
                const uint8_t *s = &cur[size_t(x) * channels];
                if (type == 0 || type == 4)
                    o[0] = o[1] = o[2] = s[0], o[3] = type == 4 ? s[1] : 255;
                else if (type == 3)
                {
                    const size_t k = size_t(s[0]) * 3;
                    if (k + 2 < palette.size())
                        o[0] = palette[k], o[1] = palette[k + 1], o[2] = palette[k + 2];
                }
                else
                    o[0] = s[0], o[1] = s[1], o[2] = s[2], o[3] = type == 6 ? s[3] : 255;
            }
            std::swap(prev, cur);
        }
        width = w;
        height = h;
        return true;
    }
}

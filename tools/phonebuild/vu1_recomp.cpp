// [phonebuild] C++ port of tools/vu1_recomp.py for the on-phone build; output byte-identical (golden-tested on LCS and VCS); keep both in step
// Usage: vu1_recomp <elf> <out.cpp> <vu1_uploads.txt>   (same command line, stdout and output file as the Python)
// Standard C++17 only (cross-compiled with the Android NDK for arm64).
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
using Bytes = std::vector<uint8_t>;

std::string F(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < static_cast<int>(sizeof buf))
        return std::string(buf, n > 0 ? n : 0);
    std::string s(static_cast<size_t>(n) + 1, '\0');
    va_start(ap, fmt);
    std::vsnprintf(&s[0], s.size(), fmt, ap);
    va_end(ap);
    s.resize(n);
    return s;
}

[[noreturn]] void die(const std::string &msg)
{
    std::fflush(stdout);
    std::fprintf(stderr, "%s\n", msg.c_str());
    std::exit(1);
}

bool read_file(const std::string &path, Bytes &out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

uint32_t u32(const Bytes &d, size_t o)
{
    if (o + 4 > d.size())
        die("vu1_recomp: truncated file");
    return d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (static_cast<uint32_t>(d[o + 3]) << 24);
}

uint16_t u16(const Bytes &d, size_t o)
{
    if (o + 2 > d.size())
        die("vu1_recomp: truncated file");
    return static_cast<uint16_t>(d[o] | (d[o + 1] << 8));
}

Bytes slice(const Bytes &d, int64_t a, int64_t b)
{
    int64_t n = static_cast<int64_t>(d.size());
    a = std::max<int64_t>(0, std::min(a, n));
    b = std::max<int64_t>(a, std::min(b, n));
    return Bytes(d.begin() + a, d.begin() + b);
}

const int CODE_SIZE = 0x4000;
const int CHUNK_PAIRS = 256;

struct Set
{
    int64_t source, vu;
    Bytes code;
};

// ---------------------------------------------------------------------------------------------------------------------
// The programs
void read_vutext(const Bytes &d, const std::string &path, uint32_t &vaddr, Bytes &out)
{
    uint32_t e_shoff = u32(d, 0x20);
    uint16_t e_shentsize = u16(d, 0x2E), e_shnum = u16(d, 0x30), e_shstrndx = u16(d, 0x32);
    auto sh = [&](int i, int k) { return u32(d, e_shoff + i * e_shentsize + k * 4); };
    uint32_t strtab = sh(e_shstrndx, 4);
    for (int i = 0; i < e_shnum; i++)
    {
        size_t off = strtab + sh(i, 0);
        std::string name;
        while (off < d.size() && d[off])
            name += static_cast<char>(d[off++]);
        if (name == ".vutext")
        {
            vaddr = sh(i, 3);
            out = slice(d, sh(i, 4), static_cast<int64_t>(sh(i, 4)) + sh(i, 5));
            return;
        }
    }
    die("vu1_recomp: no .vutext section in " + path);
}

Bytes read_vaddr_range(const Bytes &d, uint32_t start, uint32_t end)
{
    uint32_t e_phoff = u32(d, 0x1C);
    uint16_t e_phnum = u16(d, 0x2C);
    for (int k = 0; k < e_phnum; k++)
    {
        size_t b = e_phoff + k * 32;
        uint32_t off = u32(d, b + 4), va = u32(d, b + 8), fs = u32(d, b + 16);
        if (va <= start && static_cast<uint64_t>(end) <= static_cast<uint64_t>(va) + fs)
            return slice(d, static_cast<int64_t>(off) + start - va, static_cast<int64_t>(off) + end - va);
    }
    die(F("vu1_recomp: range 0x%x-0x%x not in a loadable segment", start, end));
}

std::vector<Set> program_sets(int64_t vaddr, const Bytes &v)
{
    std::vector<Set> blocks;
    int64_t o = 0, len = static_cast<int64_t>(v.size());
    while (o + 4 <= len)
    {
        int imm = u16(v, o), num = v[o + 2], cmd = v[o + 3];
        int c = cmd & 0x7F;
        o += 4;
        if (c == 0x4A)
        {
            int n = num ? num : 256;
            blocks.push_back({vaddr + o, static_cast<int64_t>(imm) * 8, slice(v, o, o + n * 8)});
            o += n * 8;
        }
        else if (c == 0x50 || c == 0x51)
            o += static_cast<int64_t>(imm ? imm : 65536) * 16;
        else if (c == 0x20)
            o += 4;
        else if (c == 0x30 || c == 0x31)
            o += 16;
        else if (c >= 0x60)
        {
            int64_t vn = (c >> 2) & 3, vl = c & 3, cnt = num ? num : 256;
            o += ((cnt * (vn + 1) * (32 >> vl) / 8) + 3) & ~static_cast<int64_t>(3);
        }
    }
    std::vector<Set> sets;
    for (auto &b : blocks)
    {
        if (!sets.empty() && sets.back().vu + static_cast<int64_t>(sets.back().code.size()) == b.vu)
            sets.back().code.insert(sets.back().code.end(), b.code.begin(), b.code.end());
        else
            sets.push_back(b);
    }
    return sets;
}

// ---------------------------------------------------------------------------------------------------------------------
// The decode
enum { NONE, FMAC, LSU, FDIV, EFU, IALU, BRANCH, XGKICK };
const int FMAC_LATENCY = 4;

int lane(int c) { return 1 << (3 - c); }
int DEST(uint32_t i) { return (i >> 21) & 0xF; }
int FT(uint32_t i) { return (i >> 16) & 0x1F; }
int FS(uint32_t i) { return (i >> 11) & 0x1F; }
int FD(uint32_t i) { return (i >> 6) & 0x1F; }
int VIT(uint32_t i) { return (i >> 16) & 0xF; }
int VIS(uint32_t i) { return (i >> 11) & 0xF; }
int VID(uint32_t i) { return (i >> 6) & 0xF; }
int IMM11(uint32_t i)
{
    int v = i & 0x7FF;
    return (v & 0x400) ? v - 0x800 : v;
}

struct Usage
{
    int vf_read[2][2] = {{0, 0}, {0, 0}};
    int n_read = 0;
    int vf_write[2] = {0, 0};
    int vi_read = 0, vi_write = 0, acc_read = 0, acc_write = 0;
    int latency = 0, vf_latency = 0, vi_latency = 0, pipeline = NONE;
    bool wait_q = false, wait_p = false, delays_branch = false, reserved = false;
};

void add_vf_read(Usage &u, int reg, int lanes)
{
    if (lanes == 0)
        return;
    for (int k = 0; k < u.n_read; k++)
        if (u.vf_read[k][0] == reg)
        {
            u.vf_read[k][1] |= lanes;
            return;
        }
    if (u.n_read < 2)
    {
        u.vf_read[u.n_read][0] = reg;
        u.vf_read[u.n_read][1] = lanes;
        u.n_read++;
    }
}

void add_vf_write(Usage &u, int reg, int lanes)
{
    if (reg == 0 || lanes == 0)
        return;
    if (u.vf_write[0] == 0)
    {
        u.vf_write[0] = reg;
        u.vf_write[1] = lanes;
    }
    else if (u.vf_write[0] == reg)
        u.vf_write[1] |= lanes;
}

int vf_read_lanes(const Usage &u, int reg)
{
    for (int k = 0; k < u.n_read; k++)
        if (u.vf_read[k][0] == reg)
            return u.vf_read[k][1];
    return 0;
}

int upper_special(uint32_t i) { return (i & 3) | ((i >> 4) & 0x7C); }
int lower_special(uint32_t i) { return (i & 3) | ((i >> 4) & 0x7C); }

bool in(int v, std::initializer_list<int> l) { return std::find(l.begin(), l.end(), v) != l.end(); }

Usage decode_upper(uint32_t upper)
{
    Usage u;
    u.pipeline = FMAC;
    u.latency = FMAC_LATENCY;
    int op = upper & 0x3F, dest = DEST(upper), fs = FS(upper), ft = FT(upper), fd = FD(upper);
    if (op <= 0x2F)
    {
        add_vf_read(u, fs, dest);
        add_vf_write(u, fd, dest);
        if (op <= 0x1B)
            add_vf_read(u, ft, lane(op & 3));
        else if (op >= 0x28)
            add_vf_read(u, ft, op == 0x2E ? 0xE : dest);
        if (in(op, {0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x21, 0x23, 0x25, 0x27, 0x29, 0x2D, 0x2E}))
            u.acc_read = dest;
        return u;
    }
    if (op >= 0x3C)
    {
        int sp = upper_special(upper);
        bool writes_acc = sp <= 0x0F || (0x18 <= sp && sp <= 0x1C) || sp == 0x1E || (0x20 <= sp && sp <= 0x2A) ||
                          (0x2C <= sp && sp <= 0x2E);
        if (writes_acc)
        {
            add_vf_read(u, fs, dest);
            if (sp <= 0x1B)
                add_vf_read(u, ft, lane(sp & 3));
            else if (0x28 <= sp && sp <= 0x2E)
                add_vf_read(u, ft, sp == 0x2E ? 0xE : dest);
            u.acc_write = dest;
            if ((0x08 <= sp && sp <= 0x0F) || in(sp, {0x21, 0x23, 0x25, 0x27, 0x29, 0x2D}))
                u.acc_read = dest;
        }
        else if ((0x10 <= sp && sp <= 0x17) || sp == 0x1D)
        {
            add_vf_read(u, fs, dest);
            add_vf_write(u, ft, dest);
        }
        else if (sp == 0x1F)
        {
            add_vf_read(u, fs, 0xE);
            add_vf_read(u, ft, 0x1);
        }
        else if (!in(sp, {0x2F, 0x30}))
            u.reserved = true;
        return u;
    }
    u.reserved = true;
    return u;
}

Usage decode_lower(uint32_t lower)
{
    Usage u;
    if (lower == 0 || lower == 0x8000033Cu)
        return u;
    int op = (lower >> 25) & 0x7F;
    int vft = FT(lower), vfs = FS(lower), vit = VIT(lower), vis = VIS(lower), vid = VID(lower), dest = DEST(lower);
    auto rvi = [&](int r) { if (r) u.vi_read |= 1 << r; };
    auto wvi = [&](int r) { if (r) u.vi_write |= 1 << r; };
    auto set_ = [&](int pipe, int lat = 0) { u.pipeline = pipe; u.latency = lat; };

    if (op == 0x00) { set_(LSU, 4); rvi(vis); add_vf_write(u, vft, dest); }
    else if (op == 0x01) { set_(LSU, 1); rvi(vit); add_vf_read(u, vfs, dest); }
    else if (op == 0x04) { set_(LSU, 4); rvi(vis); wvi(vit); }
    else if (op == 0x05) { set_(LSU, 1); rvi(vis); rvi(vit); }
    else if (in(op, {0x08, 0x09})) { set_(IALU, 1); u.delays_branch = true; rvi(vis); wvi(vit); }
    else if (in(op, {0x10, 0x12, 0x13})) { set_(IALU, 1); wvi(1); }
    else if (op == 0x11) { set_(FMAC, FMAC_LATENCY); }
    else if (in(op, {0x14, 0x16, 0x17})) { set_(IALU, 1); wvi(vit); }
    else if (op == 0x15) { set_(FMAC, FMAC_LATENCY); }
    else if (in(op, {0x18, 0x1A, 0x1B})) { set_(IALU, 1); rvi(vis); wvi(vit); }
    else if (op == 0x1C) { set_(IALU, 1); wvi(vit); }
    else if (op == 0x20) { set_(BRANCH); }
    else if (op == 0x21) { set_(BRANCH, 1); wvi(vit); }
    else if (op == 0x24) { set_(BRANCH); rvi(vis); }
    else if (op == 0x25) { set_(BRANCH, 1); rvi(vis); wvi(vit); }
    else if (in(op, {0x28, 0x29})) { set_(BRANCH); rvi(vis); rvi(vit); }
    else if (in(op, {0x2C, 0x2D, 0x2E, 0x2F})) { set_(BRANCH); rvi(vis); }
    else if (op != 0x40) u.reserved = true;
    else
    {
        int direct = lower & 0x3F;
        if (in(direct, {0x30, 0x31, 0x34, 0x35})) { set_(IALU, 1); u.delays_branch = true; rvi(vis); rvi(vit); wvi(vid); }
        else if (direct == 0x32) { set_(IALU, 1); u.delays_branch = true; rvi(vis); wvi(vit); }
        else if (direct < 0x3C) u.reserved = true;
        else
        {
            int sp = lower_special(lower);
            int l21 = lane((lower >> 21) & 3), l23 = lane((lower >> 23) & 3);
            if (in(sp, {0x30, 0x31})) { set_(FMAC, 4); add_vf_read(u, vfs, sp == 0x31 ? 0xF : dest); add_vf_write(u, vft, dest); }
            else if (in(sp, {0x34, 0x36})) { set_(LSU, 4); u.vi_latency = 1; u.delays_branch = true; rvi(vis); wvi(vis); add_vf_write(u, vft, dest); }
            else if (in(sp, {0x35, 0x37})) { set_(LSU, 1); u.delays_branch = true; rvi(vit); wvi(vit); add_vf_read(u, vfs, dest); }
            else if (sp == 0x38) { set_(FDIV, 7); add_vf_read(u, vfs, l21); add_vf_read(u, vft, l23); }
            else if (sp == 0x39) { set_(FDIV, 7); add_vf_read(u, vft, l23); }
            else if (sp == 0x3A) { set_(FDIV, 13); add_vf_read(u, vfs, l21); add_vf_read(u, vft, l23); }
            else if (sp == 0x3B) { set_(FDIV); u.wait_q = true; }
            else if (sp == 0x3C) { set_(IALU, 1); u.delays_branch = true; add_vf_read(u, vfs, l21); wvi(vit); }
            else if (sp == 0x3D) { set_(FMAC, 4); rvi(vis); add_vf_write(u, vft, dest); }
            else if (sp == 0x3E) { set_(LSU, 4); rvi(vis); wvi(vit); }
            else if (sp == 0x3F) { set_(LSU, 1); rvi(vis); rvi(vit); }
            else if (in(sp, {0x40, 0x41, 0x64})) { set_(FMAC, 4); add_vf_write(u, vft, dest); }
            else if (in(sp, {0x42, 0x43})) { set_(IALU, 1); add_vf_read(u, vfs, l21); }
            else if (in(sp, {0x68, 0x69})) { set_(IALU, 1); wvi(vit); }
            else if (sp == 0x6C) { set_(XGKICK, 2); rvi(vis); }
            else if (0x70 <= sp && sp <= 0x7D && sp != 0x7B)
            {
                int lat = 0;
                switch (sp)
                {
                case 0x70: lat = 11; break;
                case 0x71: case 0x72: case 0x77: lat = 18; break;
                case 0x73: lat = 24; break;
                case 0x74: case 0x75: case 0x7C: lat = 54; break;
                case 0x76: case 0x78: case 0x7A: lat = 12; break;
                case 0x79: lat = 29; break;
                case 0x7D: lat = 44; break;
                }
                set_(EFU, lat);
                if (sp <= 0x73) add_vf_read(u, vfs, 0xE);
                else if (sp == 0x74) add_vf_read(u, vfs, 0xC);
                else if (sp == 0x75) add_vf_read(u, vfs, 0xA);
                else if (sp == 0x76) add_vf_read(u, vfs, 0xF);
                else add_vf_read(u, vfs, l21);
            }
            else if (sp == 0x7B) { set_(EFU); u.wait_p = true; }
            else u.reserved = true;
        }
    }
    return u;
}

std::vector<int> lanes_of(int dest)
{
    std::vector<int> r;
    for (int c = 0; c < 4; c++)
        if (dest & lane(c))
            r.push_back(c);
    return r;
}

int components(int lanes)
{
    int s = 0;
    for (int c : lanes_of(lanes))
        s += 1 << c;
    return s;
}

struct Pair
{
    uint32_t address, index, lower, upper;
    bool i_bit, e_bit, d_bit, t_bit;
    Usage up, lo;
    int lower_vi_write = 0, shadow = 0, suppressed = 0;
    std::vector<std::pair<int, int>> stall_vf;
    int stall_vi, stall_acc;
    int mark_lower[2], mark_lower_lat, mark_upper[2], mark_upper_lat, mark_vi, mark_vi_lat, mark_acc;
    bool has_settle = false;
    int settle = 0;
    bool reserved, branch;

    Pair(uint32_t address_, uint32_t lower_, uint32_t upper_) : address(address_), index(address_ / 8), lower(lower_), upper(upper_)
    {
        i_bit = (upper >> 31) & 1;
        e_bit = (upper >> 30) & 1;
        d_bit = (upper >> 28) & 1;
        t_bit = (upper >> 27) & 1;
        up = decode_upper(upper);
        lo = !i_bit ? decode_lower(lower) : Usage();
        for (int r = 1; r < 16; r++)
            if (lo.vi_write & (1 << r))
            {
                lower_vi_write = r;
                break;
            }
        int w = up.vf_write[0];
        if (w && (vf_read_lanes(lo, w) || lo.vf_write[0] == w))
        {
            shadow = w;
            if (lo.vf_write[0] == w)
                suppressed = w;
        }
        for (const Usage *u : {&up, &lo})
            for (int k = 0; k < u->n_read; k++)
                if (u->vf_read[k][0] && u->vf_read[k][1] && stall_vf.size() < 4)
                    stall_vf.push_back({u->vf_read[k][0], u->vf_read[k][1]});
        stall_vi = (up.vi_read | lo.vi_read) & 0xFFFE;
        stall_acc = up.acc_read | lo.acc_read;
        if (lo.vf_write[0] && suppressed != lo.vf_write[0])
        {
            mark_lower[0] = lo.vf_write[0];
            mark_lower[1] = lo.vf_write[1];
        }
        else
            mark_lower[0] = mark_lower[1] = 0;
        mark_lower_lat = mark_lower[0] ? (lo.vf_latency ? lo.vf_latency : lo.latency) : 0;
        if (up.vf_write[0])
        {
            mark_upper[0] = up.vf_write[0];
            mark_upper[1] = up.vf_write[1];
        }
        else
            mark_upper[0] = mark_upper[1] = 0;
        mark_upper_lat = mark_upper[0] ? (up.vf_latency ? up.vf_latency : up.latency) : 0;
        mark_vi = lo.vi_write & 0xFFFE;
        mark_vi_lat = lo.vi_latency ? lo.vi_latency : lo.latency;
        mark_acc = up.acc_write;
        auto add = [&](int v) {
            settle = has_settle ? std::max(settle, v) : v;
            has_settle = true;
        };
        if (mark_lower[0]) add(mark_lower_lat);
        if (mark_upper[0]) add(mark_upper_lat);
        if (lower_vi_write) add(mark_vi_lat);
        if (mark_acc) add(1);
        reserved = up.reserved || lo.reserved;
        branch = !i_bit && lo.pipeline == BRANCH;
    }

    std::string shape() const
    {
        std::string s = "{{";
        for (int k = 0; k < 4; k++)
        {
            if (k)
                s += ", ";
            int a = k < static_cast<int>(stall_vf.size()) ? stall_vf[k].first : 0;
            int b = k < static_cast<int>(stall_vf.size()) ? stall_vf[k].second : 0;
            s += F("{%d, %d}", a, b);
        }
        int flags = (lo.wait_q ? 1 : 0) | (lo.wait_p ? 2 : 0) | (i_bit ? 4 : 0) | (e_bit ? 8 : 0) | (d_bit ? 16 : 0) |
                    (t_bit ? 32 : 0) | (reserved ? 64 : 0) | (lo.delays_branch ? 128 : 0);
        s += F("}, %d, 0x%x, 0x%x, %d, 0x%x, {%d, %d}, %d, {%d, %d}, %d, 0x%x, %d, 0x%x, %d, %d, %d}",
               static_cast<int>(stall_vf.size()), stall_vi, stall_acc, lo.pipeline, flags, mark_lower[0], mark_lower[1],
               mark_lower_lat, mark_upper[0], mark_upper[1], mark_upper_lat, mark_vi, mark_vi_lat, mark_acc,
               lower_vi_write, shadow, suppressed);
        return s;
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// Instruction names
const char *BC[] = {"x", "y", "z", "w"};

const char *upper_names(int op)
{
    switch (op)
    {
    case 0x1C: return "MULq"; case 0x1D: return "MAXi"; case 0x1E: return "MULi"; case 0x1F: return "MINIi";
    case 0x20: return "ADDq"; case 0x21: return "MADDq"; case 0x22: return "ADDi"; case 0x23: return "MADDi";
    case 0x24: return "SUBq"; case 0x25: return "MSUBq"; case 0x26: return "SUBi"; case 0x27: return "MSUBi";
    case 0x28: return "ADD"; case 0x29: return "MADD"; case 0x2A: return "MUL"; case 0x2B: return "MAX";
    case 0x2C: return "SUB"; case 0x2D: return "MSUB"; case 0x2E: return "OPMSUB"; case 0x2F: return "MINI";
    }
    return "reserved";
}

const char *upper_special_names(int sp)
{
    switch (sp)
    {
    case 0x10: return "ITOF0"; case 0x11: return "ITOF4"; case 0x12: return "ITOF12"; case 0x13: return "ITOF15";
    case 0x14: return "FTOI0"; case 0x15: return "FTOI4"; case 0x16: return "FTOI12"; case 0x17: return "FTOI15";
    case 0x1C: return "MULAq"; case 0x1D: return "ABS"; case 0x1E: return "MULAi"; case 0x1F: return "CLIP";
    case 0x20: return "ADDAq"; case 0x21: return "MADDAq"; case 0x22: return "ADDAi"; case 0x23: return "MADDAi";
    case 0x24: return "SUBAq"; case 0x25: return "MSUBAq"; case 0x26: return "SUBAi"; case 0x27: return "MSUBAi";
    case 0x28: return "ADDA"; case 0x29: return "MADDA"; case 0x2A: return "MULA"; case 0x2C: return "SUBA";
    case 0x2D: return "MSUBA"; case 0x2E: return "OPMULA"; case 0x2F: return "NOP"; case 0x30: return "NOP";
    }
    return "reserved";
}

const char *lower_names(int op)
{
    switch (op)
    {
    case 0x00: return "LQ"; case 0x01: return "SQ"; case 0x04: return "ILW"; case 0x05: return "ISW";
    case 0x08: return "IADDIU"; case 0x09: return "ISUBIU"; case 0x10: return "FCEQ"; case 0x11: return "FCSET";
    case 0x12: return "FCAND"; case 0x13: return "FCOR"; case 0x14: return "FSEQ"; case 0x15: return "FSSET";
    case 0x16: return "FSAND"; case 0x17: return "FSOR"; case 0x18: return "FMEQ"; case 0x1A: return "FMAND";
    case 0x1B: return "FMOR"; case 0x1C: return "FCGET"; case 0x20: return "B"; case 0x21: return "BAL";
    case 0x24: return "JR"; case 0x25: return "JALR"; case 0x28: return "IBEQ"; case 0x29: return "IBNE";
    case 0x2C: return "IBLTZ"; case 0x2D: return "IBGTZ"; case 0x2E: return "IBLEZ"; case 0x2F: return "IBGEZ";
    }
    return "reserved";
}

const char *lower_direct_names(int d)
{
    switch (d)
    {
    case 0x30: return "IADD"; case 0x31: return "ISUB"; case 0x32: return "IADDI"; case 0x34: return "IAND";
    case 0x35: return "IOR";
    }
    return "reserved";
}

const char *lower_special_names(int sp)
{
    switch (sp)
    {
    case 0x30: return "MOVE"; case 0x31: return "MR32"; case 0x34: return "LQI"; case 0x35: return "SQI";
    case 0x36: return "LQD"; case 0x37: return "SQD"; case 0x38: return "DIV"; case 0x39: return "SQRT";
    case 0x3A: return "RSQRT"; case 0x3B: return "WAITQ"; case 0x3C: return "MTIR"; case 0x3D: return "MFIR";
    case 0x3E: return "ILWR"; case 0x3F: return "ISWR"; case 0x40: return "RNEXT"; case 0x41: return "RGET";
    case 0x42: return "RINIT"; case 0x43: return "RXOR"; case 0x64: return "MFP"; case 0x68: return "XTOP";
    case 0x69: return "XITOP"; case 0x6C: return "XGKICK"; case 0x70: return "ESADD"; case 0x71: return "ERSADD";
    case 0x72: return "ELENG"; case 0x73: return "ERLENG"; case 0x74: return "EATANxy"; case 0x75: return "EATANxz";
    case 0x76: return "ESUM"; case 0x77: return "ERSQRT"; case 0x78: return "ESQRT"; case 0x79: return "ESIN";
    case 0x7A: return "ERCPR"; case 0x7B: return "WAITP"; case 0x7C: return "EATAN"; case 0x7D: return "EEXP";
    }
    return "reserved";
}

std::string upper_name(uint32_t i)
{
    int op = i & 0x3F;
    if (op < 0x1C)
    {
        static const char *n[] = {"ADD", "SUB", "MADD", "MSUB", "MAX", "MINI", "MUL"};
        return std::string(n[op >> 2]) + BC[op & 3];
    }
    if (op < 0x3C)
        return upper_names(op);
    int sp = upper_special(i);
    if (sp <= 0x0F)
    {
        static const char *n[] = {"ADDA", "SUBA", "MADDA", "MSUBA"};
        return std::string(n[sp >> 2]) + BC[sp & 3];
    }
    if (0x18 <= sp && sp <= 0x1B)
        return std::string("MULA") + BC[sp & 3];
    return upper_special_names(sp);
}

std::string lower_name(uint32_t i)
{
    if (i == 0 || i == 0x8000033Cu)
        return "NOP";
    int op = (i >> 25) & 0x7F;
    if (op != 0x40)
        return lower_names(op);
    int direct = i & 0x3F;
    if (direct < 0x3C)
        return lower_direct_names(direct);
    return lower_special_names(lower_special(i));
}

// ---------------------------------------------------------------------------------------------------------------------
// Code for the instructions
uint32_t f32_normalised_bits(uint32_t bits)
{
    uint32_t exponent = bits & 0x7F800000u;
    if (exponent == 0)
        return bits & 0x80000000u;
    if (exponent == 0x7F800000u)
        return (bits & 0x80000000u) | 0x7F7FFFFFu;
    return bits;
}

enum { BR_NONE, BR_ALWAYS, BR_IF, BR_JUMP };

struct Emitted
{
    std::vector<std::string> lines;
    bool writes_vf0 = false, writes_vi0 = false;
    std::string generic; // empty = None
    int branch = BR_NONE;
    int64_t target = 0;
};

struct Fast
{
    int key;
    const char *kind, *source;
};
const Fast FAST_UPPER[] = {{0x1C, "mul", "q"}, {0x1D, "max", "i"}, {0x1E, "mul", "i"}, {0x1F, "mini", "i"},
                           {0x20, "add", "q"}, {0x21, "madd", "q"}, {0x22, "add", "i"}, {0x23, "madd", "i"},
                           {0x24, "sub", "q"}, {0x25, "msub", "q"}, {0x26, "sub", "i"}, {0x27, "msub", "i"},
                           {0x28, "add", "lane"}, {0x29, "madd", "lane"}, {0x2A, "mul", "lane"},
                           {0x2B, "max", "lane"}, {0x2C, "sub", "lane"}, {0x2D, "msub", "lane"},
                           {0x2F, "mini", "lane"}};
const Fast FAST_UPPER_ACC[] = {{0x1C, "mul", "q"}, {0x1E, "mul", "i"}, {0x20, "add", "q"}, {0x21, "madd", "q"},
                               {0x22, "add", "i"}, {0x23, "madd", "i"}, {0x24, "sub", "q"}, {0x25, "msub", "q"},
                               {0x26, "sub", "i"}, {0x27, "msub", "i"}, {0x28, "add", "lane"},
                               {0x29, "madd", "lane"}, {0x2A, "mul", "lane"}, {0x2C, "sub", "lane"},
                               {0x2D, "msub", "lane"}};

template <size_t N> const Fast *find_fast(const Fast (&t)[N], int key)
{
    for (const Fast &f : t)
        if (f.key == key)
            return &f;
    return nullptr;
}

bool THROUGHPUT = false;

void emit_upper(Emitted &e, uint32_t address, uint32_t upper)
{
    int op = upper & 0x3F, dest = DEST(upper), fs = FS(upper), ft = FT(upper), fd = FD(upper);
    std::string kind, source;
    int bc = 0;
    bool to_acc = false;
    if (op < 0x3C)
    {
        if (op <= 0x1B)
        {
            static const char *n[] = {"add", "sub", "madd", "msub", "max", "mini", "mul"};
            kind = n[op >> 2];
            source = "bc";
            bc = op & 3;
            to_acc = false;
        }
        else if (const Fast *f = find_fast(FAST_UPPER, op))
        {
            kind = f->kind;
            source = f->source;
            bc = 0;
            to_acc = false;
        }
    }
    else
    {
        int sp = upper_special(upper);
        if (sp == 0x2F || sp == 0x30)
            return;
        if (0x14 <= sp && sp <= 0x17)
        {
            static const char *scale[] = {"1.0f", "16.0f", "4096.0f", "32768.0f"};
            if (lanes_of(dest).empty())
                return;
            e.lines.push_back(F("vu1v::store<0x%xu>(s.vf[%d], vu1v::floatToInt(vu1v::operand(vu1v::load(s.vf[%d])), %s));",
                                components(dest), ft, fs, scale[sp & 3]));
            e.writes_vf0 |= ft == 0;
            return;
        }
        if (0x10 <= sp && sp <= 0x13)
        {
            static const char *div[] = {"", ", 16.0f", ", 4096.0f", ", 32768.0f"};
            if (lanes_of(dest).empty())
                return;
            e.lines.push_back(F("vu1v::store<0x%xu>(s.vf[%d], vu1v::intToFloat(vu1v::load(s.vf[%d])%s));",
                                components(dest), ft, fs, div[sp & 3]));
            e.writes_vf0 |= ft == 0;
            return;
        }
        if (sp == 0x1D)
        {
            if (lanes_of(dest).empty())
                return;
            e.lines.push_back(F("vu1v::store<0x%xu>(s.vf[%d], vu1v::abs(vu1v::operand(vu1v::load(s.vf[%d]))));",
                                components(dest), ft, fs));
            e.writes_vf0 |= ft == 0;
            return;
        }
        if (sp == 0x1F)
        {
            e.lines.push_back(F("Vu1Compiled::queueClip(vu, vu1fast::clipFlags(s.vf[%d], s.vf[%d]));", fs, ft));
            return;
        }
        if (sp <= 0x0F)
        {
            static const char *n[] = {"add", "sub", "madd", "msub"};
            kind = n[sp >> 2];
            source = "bc";
            bc = sp & 3;
            to_acc = true;
        }
        else if (0x18 <= sp && sp <= 0x1B)
        {
            kind = "mul";
            source = "bc";
            bc = sp & 3;
            to_acc = true;
        }
        else if (const Fast *f = find_fast(FAST_UPPER_ACC, sp))
        {
            kind = f->kind;
            source = f->source;
            bc = 0;
            to_acc = true;
        }
    }
    if (kind.empty())
    {
        e.lines.push_back(F("s.pc = 0x%04xu;", address));
        e.lines.push_back(F("Vu1Compiled::runUpper(vu, 0x%08xu); // %s", upper, upper_name(upper).c_str()));
        e.generic = upper_name(upper);
        e.writes_vf0 = true;
        return;
    }
    if (lanes_of(dest).empty())
        return;
    int mask = components(dest);
    e.lines.push_back("{");
    e.lines.push_back(F("    const vu1v::V a = vu1v::operand(vu1v::load(s.vf[%d]));", fs));
    if (source == "bc")
        e.lines.push_back(F("    const vu1v::V b = vu1v::splat(vu1fast::operand(s.vf[%d][%d]));", ft, bc));
    else if (source == "q")
        e.lines.push_back("    const vu1v::V b = vu1v::splat(vu1fast::operand(s.q));");
    else if (source == "i")
        e.lines.push_back("    const vu1v::V b = vu1v::splat(vu1fast::operand(s.i));");
    else
        e.lines.push_back(F("    const vu1v::V b = vu1v::operand(vu1v::load(s.vf[%d]));", ft));
    std::string result;
    if (kind == "add" || kind == "sub" || kind == "mul")
        result = "vu1v::operand(vu1v::" + kind + "(a, b))";
    else if (kind == "max" || kind == "mini")
        result = std::string("vu1v::") + (kind == "max" ? "max" : "min") + "(a, b)";
    else if (THROUGHPUT)
        result = std::string("vu1v::operand(vu1v::") + (kind == "madd" ? "add" : "sub") +
                 "(vu1v::operand(vu1v::load(s.acc)), vu1v::mul(a, b)))";
    else
        result = F("vu1v::fused<0x%xu, %s>", mask, kind == "madd" ? "false" : "true") +
                 "(vu1v::operand(vu1v::load(s.acc)), a, b)";
    std::string target = to_acc ? std::string("s.acc") : F("s.vf[%d]", fd);
    e.lines.push_back(F("    vu1v::store<0x%xu>(%s, %s);", mask, target.c_str(), result.c_str()));
    e.lines.push_back("}");
    if (!to_acc)
        e.writes_vf0 |= fd == 0;
}

void load_lanes(Emitted &e, int reg, int dest, const char *src)
{
    auto ls = lanes_of(dest);
    if (ls.size() == 4)
        e.lines.push_back(F("    std::memcpy(s.vf[%d], %s, 16);", reg, src));
    else if (!ls.empty())
        e.lines.push_back(F("    vu1v::store<0x%xu>(s.vf[%d], vu1v::loadBytes(%s));", components(dest), reg, src));
    e.writes_vf0 |= reg == 0 && !ls.empty();
}

std::string branch_vi(int reg)
{
    if (reg == 0)
        return "0";
    return F("(bkValid && bkReg == %d ? bkValue : s.vi[%d])", reg, reg);
}

void emit_lower(Emitted &e, uint32_t address, uint32_t lower, uint32_t upper)
{
    if (lower == 0 || lower == 0x8000033Cu)
        return;
    int op = (lower >> 25) & 0x7F;
    int vft = FT(lower), vfs = FS(lower), vit = VIT(lower), vis = VIS(lower), vid = VID(lower), dest = DEST(lower);
    int imm = IMM11(lower);
    auto &L = e.lines;
    auto first_lane = [&]() { return (dest & 8) ? 0 : (dest & 4) ? 1 : (dest & 2) ? 2 : 3; };
    auto generic = [&]() {
        L.push_back(F("s.pc = 0x%04xu;", address));
        L.push_back(F("Vu1Compiled::execLower(vu, 0x%08xu, 0x%08xu); // %s", lower, upper, lower_name(lower).c_str()));
        e.generic = lower_name(lower);
        e.writes_vf0 = e.writes_vi0 = true;
    };
    int64_t target = (static_cast<int64_t>(address) + 8 + static_cast<int64_t>(imm) * 8) & 0x3FFF;
    unsigned c24 = lower & 0xFFFFFFu;
    if (op == 0x00)
    {
        L.push_back("{");
        L.push_back(F("    const uint32_t addr = (static_cast<uint32_t>(s.vi[%d] + (%d)) * 16u) & 0x3FFFu;", vis, imm));
        load_lanes(e, vft, dest, "data + addr");
        L.push_back("}");
    }
    else if (op == 0x01)
    {
        L.push_back("{");
        L.push_back(F("    const uint32_t addr = (static_cast<uint32_t>(s.vi[%d] + (%d)) * 16u) & 0x3FFFu;", vit, imm));
        L.push_back(F("    uint32_t w[4]; std::memcpy(w, s.vf[%d], 16);", vfs));
        L.push_back(F("    Vu1Compiled::queueStore(vu, addr, w, %d);", dest));
        L.push_back("}");
    }
    else if (op == 0x04)
    {
        if (vit)
        {
            L.push_back("{");
            L.push_back(F("    const uint32_t addr = (static_cast<uint32_t>(s.vi[%d] + (%d)) * 16u) & 0x3FFFu;", vis, imm));
            L.push_back(F("    uint32_t v; std::memcpy(&v, data + addr + %d, 4);", first_lane() * 4));
            L.push_back(F("    s.vi[%d] = static_cast<int32_t>(static_cast<int16_t>(v & 0xFFFFu));", vit));
            L.push_back("}");
        }
    }
    else if (op == 0x05)
    {
        L.push_back("{");
        L.push_back(F("    const uint32_t addr = (static_cast<uint32_t>(s.vi[%d] + (%d)) * 16u) & 0x3FFFu;", vis, imm));
        L.push_back(F("    const uint32_t v = static_cast<uint32_t>(static_cast<uint16_t>(s.vi[%d] & 0xFFFF));", vit));
        L.push_back("    const uint32_t w[4] = {v, v, v, v};");
        L.push_back(F("    Vu1Compiled::queueStore(vu, addr, w, %d);", dest));
        L.push_back("}");
    }
    else if (op == 0x08 || op == 0x09)
    {
        unsigned imm15 = (lower & 0x7FF) | ((lower >> 10) & 0x7800);
        if (vit)
            L.push_back(F("s.vi[%d] = static_cast<int16_t>(s.vi[%d] %s %u);", vit, vis, op == 0x08 ? "+" : "-", imm15));
    }
    else if (op == 0x10)
        L.push_back(F("s.vi[1] = ((s.clip & 0xFFFFFFu) == 0x%xu) ? 1 : 0;", c24));
    else if (op == 0x11)
        L.push_back(F("Vu1Compiled::queueFcset(vu, 0x%xu);", c24));
    else if (op == 0x12)
        L.push_back(F("s.vi[1] = ((s.clip & 0x%xu) != 0u) ? 1 : 0;", c24));
    else if (op == 0x13)
        L.push_back(F("s.vi[1] = ((s.clip | 0x%xu) == 0xFFFFFFu) ? 1 : 0;", c24));
    else if (op == 0x1C)
    {
        if (vit)
            L.push_back(F("s.vi[%d] = static_cast<int32_t>(s.clip & 0x0FFFu);", vit));
    }
    else if (op == 0x20)
    {
        e.branch = BR_ALWAYS;
        e.target = target;
    }
    else if (op == 0x21)
    {
        if (vit)
            L.push_back(F("s.vi[%d] = %u;", vit, (address + 16) / 8));
        e.branch = BR_ALWAYS;
        e.target = target;
    }
    else if (op == 0x24 || op == 0x25)
    {
        std::string v = vis == 0 ? std::string("0") : F("s.vi[%d]", vis);
        L.push_back(F("jumpTarget = (static_cast<uint32_t>(static_cast<uint16_t>(%s)) * 8u) & 0x3FFFu;", v.c_str()));
        if (op == 0x25 && vit)
            L.push_back(F("s.vi[%d] = %u;", vit, (address + 16) / 8));
        e.branch = BR_JUMP;
    }
    else if (op == 0x28 || op == 0x29)
    {
        L.push_back(F("taken = static_cast<int16_t>(%s) %s static_cast<int16_t>(%s);", branch_vi(vis).c_str(),
                      op == 0x28 ? "==" : "!=", branch_vi(vit).c_str()));
        e.branch = BR_IF;
        e.target = target;
    }
    else if (op >= 0x2C && op <= 0x2F)
    {
        static const char *sym[] = {"<", ">", "<=", ">="};
        L.push_back(F("taken = static_cast<int16_t>(%s) %s 0;", branch_vi(vis).c_str(), sym[op - 0x2C]));
        e.branch = BR_IF;
        e.target = target;
    }
    else if (op == 0x40 && in(lower & 0x3F, {0x30, 0x31, 0x32, 0x34, 0x35}))
    {
        int direct = lower & 0x3F;
        if (direct == 0x32)
        {
            int imm5 = (lower >> 6) & 0x1F;
            imm5 = (imm5 & 0x10) ? imm5 - 32 : imm5;
            if (vit)
                L.push_back(F("s.vi[%d] = static_cast<int16_t>(s.vi[%d] + (%d));", vit, vis, imm5));
        }
        else if (vid)
        {
            if (direct == 0x30)
                L.push_back(F("s.vi[%d] = static_cast<int16_t>(s.vi[%d] + s.vi[%d]);", vid, vis, vit));
            else if (direct == 0x31)
                L.push_back(F("s.vi[%d] = static_cast<int16_t>(s.vi[%d] - s.vi[%d]);", vid, vis, vit));
            else if (direct == 0x34)
                L.push_back(F("s.vi[%d] = s.vi[%d] & s.vi[%d];", vid, vis, vit));
            else
                L.push_back(F("s.vi[%d] = s.vi[%d] | s.vi[%d];", vid, vis, vit));
        }
    }
    else if (op == 0x40 && (lower & 0x3F) >= 0x3C)
    {
        int sp = lower_special(lower);
        unsigned l21 = (lower >> 21) & 3, l23 = (lower >> 23) & 3;
        if (sp == 0x30)
        {
            if (!lanes_of(dest).empty())
            {
                L.push_back(F("vu1v::store<0x%xu>(s.vf[%d], vu1v::load(s.vf[%d]));", components(dest), vft, vfs));
                e.writes_vf0 |= vft == 0;
            }
        }
        else if (sp == 0x31)
        {
            if (!lanes_of(dest).empty())
            {
                L.push_back(F("vu1v::store<0x%xu>(s.vf[%d], vu1v::rotate(vu1v::load(s.vf[%d])));", components(dest), vft, vfs));
                e.writes_vf0 |= vft == 0;
            }
        }
        else if (sp == 0x34 || sp == 0x36)
        {
            L.push_back("{");
            if (sp == 0x36 && vis)
                L.push_back(F("    s.vi[%d] = static_cast<int16_t>(s.vi[%d] - 1);", vis, vis));
            L.push_back(F("    const uint32_t addr = (static_cast<uint32_t>(static_cast<uint16_t>(s.vi[%d])) * 16u) & 0x3FFFu;", vis));
            load_lanes(e, vft, dest, "data + addr");
            if (sp == 0x34 && vis)
                L.push_back(F("    s.vi[%d] = static_cast<int16_t>(s.vi[%d] + 1);", vis, vis));
            L.push_back("}");
        }
        else if (sp == 0x35 || sp == 0x37)
        {
            L.push_back("{");
            if (sp == 0x37 && vit)
                L.push_back(F("    s.vi[%d] = static_cast<int16_t>(s.vi[%d] - 1);", vit, vit));
            L.push_back(F("    const uint32_t addr = (static_cast<uint32_t>(static_cast<uint16_t>(s.vi[%d])) * 16u) & 0x3FFFu;", vit));
            L.push_back(F("    uint32_t w[4]; std::memcpy(w, s.vf[%d], 16);", vfs));
            L.push_back(F("    Vu1Compiled::queueStore(vu, addr, w, %d);", dest));
            if (sp == 0x35 && vit)
                L.push_back(F("    s.vi[%d] = static_cast<int16_t>(s.vi[%d] + 1);", vit, vit));
            L.push_back("}");
        }
        else if (sp == 0x38)
            L.push_back(F("Vu1Compiled::div(vu, s.vf[%d][%u], s.vf[%d][%u]);", vfs, l21, vft, l23));
        else if (sp == 0x39)
            L.push_back(F("Vu1Compiled::sqrt(vu, s.vf[%d][%u]);", vft, l23));
        else if (sp == 0x3A)
            L.push_back(F("Vu1Compiled::rsqrt(vu, s.vf[%d][%u], s.vf[%d][%u]);", vfs, l21, vft, l23));
        else if (sp == 0x3B)
        {
        }
        else if (sp == 0x3C)
        {
            if (vit)
            {
                L.push_back("{");
                L.push_back(F("    uint32_t f; std::memcpy(&f, &s.vf[%d][%u], 4);", vfs, l21));
                L.push_back(F("    s.vi[%d] = static_cast<int32_t>(static_cast<int16_t>(f & 0xFFFFu));", vit));
                L.push_back("}");
            }
        }
        else if (sp == 0x3D)
        {
            if (!lanes_of(dest).empty())
            {
                L.push_back(F("vu1v::store<0x%xu>(s.vf[%d], vu1v::splatBits(static_cast<int32_t>(static_cast<int16_t>(s.vi[%d] & 0xFFFF))));",
                              components(dest), vft, vis));
                e.writes_vf0 |= vft == 0;
            }
        }
        else if (sp == 0x3E)
        {
            if (vit)
            {
                L.push_back("{");
                L.push_back(F("    const uint32_t addr = (static_cast<uint32_t>(static_cast<uint16_t>(s.vi[%d])) * 16u) & 0x3FFFu;", vis));
                L.push_back(F("    uint32_t v; std::memcpy(&v, data + addr + %d, 4);", first_lane() * 4));
                L.push_back(F("    s.vi[%d] = static_cast<int32_t>(static_cast<int16_t>(v & 0xFFFFu));", vit));
                L.push_back("}");
            }
        }
        else if (sp == 0x3F)
        {
            L.push_back("{");
            L.push_back(F("    const uint32_t addr = (static_cast<uint32_t>(static_cast<uint16_t>(s.vi[%d])) * 16u) & 0x3FFFu;", vis));
            L.push_back(F("    const uint32_t v = static_cast<uint32_t>(static_cast<uint16_t>(s.vi[%d] & 0xFFFF));", vit));
            L.push_back("    const uint32_t w[4] = {v, v, v, v};");
            L.push_back(F("    Vu1Compiled::queueStore(vu, addr, w, %d);", dest));
            L.push_back("}");
        }
        else if (sp == 0x68 || sp == 0x69)
        {
            if (vit)
                L.push_back(F("s.vi[%d] = static_cast<int32_t>(s.%s & 0x3FFu);", vit, sp == 0x68 ? "top" : "itop"));
        }
        else if (sp == 0x6C)
            L.push_back(F("Vu1Compiled::startXgkick(vu, static_cast<uint32_t>(static_cast<uint16_t>(s.vi[%d])));", vis));
        else
            generic();
    }
    else
        generic();
}

// ---------------------------------------------------------------------------------------------------------------------
// The pairs
struct Program
{
    int number;
    int64_t vu;
    std::map<int64_t, Pair> pairs;
    std::map<std::string, int> generic;

    Program(int number_, int64_t vu_, const Bytes &code) : number(number_), vu(vu_)
    {
        for (size_t k = 0; k < code.size() / 8; k++)
        {
            uint32_t lower = code[k * 8] | (code[k * 8 + 1] << 8) | (code[k * 8 + 2] << 16) | (static_cast<uint32_t>(code[k * 8 + 3]) << 24);
            uint32_t upper = code[k * 8 + 4] | (code[k * 8 + 5] << 8) | (code[k * 8 + 6] << 16) | (static_cast<uint32_t>(code[k * 8 + 7]) << 24);
            int64_t a = vu + static_cast<int64_t>(k) * 8;
            pairs.emplace(a, Pair(static_cast<uint32_t>(a), lower, upper));
        }
    }

    const Pair *get(int64_t a) const
    {
        auto it = pairs.find(a);
        return it == pairs.end() ? nullptr : &it->second;
    }

    bool plain(int64_t a) const
    {
        const Pair *p = get(a);
        return p && !(p->branch || p->e_bit || p->d_bit || p->t_bit || p->reserved);
    }

    bool compiled(int64_t a) const
    {
        const Pair *p = get(a);
        return p && !(p->reserved || p->d_bit || p->t_bit || (p->e_bit && p->branch));
    }
};

int64_t next_pc(int64_t address) { return (address + 8) % CODE_SIZE; }

struct Work
{
    std::string comment;
    std::vector<std::string> out;
    int branch;
    int64_t target;
};

Work pair_work(Program &prog, const Pair &p, const std::string &exit_stmt)
{
    Work w;
    auto &out = w.out;
    out.push_back("if (cyc >= budgetEnd || stop) " + exit_stmt);
    std::vector<std::string> late;
    if (!THROUGHPUT)
        for (auto &r : p.stall_vf)
            late.push_back(F("vu1fast::lanesAfter<0x%xu>(vfReady + %d, cyc)", components(r.second), r.first * 4));
    for (int r = 1; r < 16; r++)
        if ((p.stall_vi & (1 << r)) && !THROUGHPUT)
            late.push_back(F("static_cast<unsigned>(viReady[%d] > cyc)", r));
    if (p.stall_acc && !THROUGHPUT)
        late.push_back(F("vu1fast::lanesAfter<0x%xu>(accReady, cyc)", components(p.stall_acc)));
    if (p.lo.pipeline == FDIV || p.lo.wait_q)
        late.push_back("static_cast<unsigned>(fdiv.valid && fdiv.readyCycle > cyc)");
    if (p.lo.pipeline == EFU)
        late.push_back("static_cast<unsigned>(efuReady > cyc)");
    if (p.lo.wait_p)
        late.push_back("static_cast<unsigned>((efu[0].valid && efu[0].readyCycle > cyc) || (efu[1].valid && efu[1].readyCycle > cyc))");
    if (p.lo.pipeline == XGKICK)
        late.push_back("static_cast<unsigned>(kickActive)");
    if (!late.empty())
    {
        std::string j;
        for (size_t k = 0; k < late.size(); k++)
            j += (k ? " | " : "") + late[k];
        out.push_back("if ((" + j + ") != 0u)");
        out.push_back("{");
        out.push_back(F("    if (!Vu1Compiled::stall(vu, dec[%u], budgetEnd)) ", p.index) + exit_stmt);
        out.push_back("    cyc = cycle;");
        out.push_back("}");
    }

    Emitted e_up, e_lo;
    emit_upper(e_up, p.address, p.upper);
    if (!p.i_bit)
        emit_lower(e_lo, p.address, p.lower, p.upper);
    for (Emitted *e : {&e_up, &e_lo})
        if (!e->generic.empty())
            prog.generic[e->generic] += 1;
    bool backup = p.lower_vi_write && p.lo.delays_branch;
    if (backup)
        out.push_back(F("const int32_t oldVi = s.vi[%d];", p.lower_vi_write));
    auto app = [&](const std::vector<std::string> &v) { out.insert(out.end(), v.begin(), v.end()); };
    if (p.i_bit)
    {
        app(e_up.lines);
        out.push_back(F("s.i = vu1fast::fromBits(0x%08xu);", f32_normalised_bits(p.lower)));
    }
    else if (p.shadow)
    {
        out.push_back(F("float shadowOld[4]; std::memcpy(shadowOld, s.vf[%d], 16);", p.shadow));
        app(e_up.lines);
        out.push_back(F("float shadowNew[4]; std::memcpy(shadowNew, s.vf[%d], 16);", p.shadow));
        out.push_back(F("std::memcpy(s.vf[%d], shadowOld, 16);", p.shadow));
        app(e_lo.lines);
        out.push_back(F("std::memcpy(s.vf[%d], shadowNew, 16);", p.shadow));
    }
    else
    {
        app(e_up.lines);
        app(e_lo.lines);
    }
    out.push_back("bkValid = false;");
    const int marks[2][3] = {{p.mark_lower[0], p.mark_lower[1], p.mark_lower_lat},
                             {p.mark_upper[0], p.mark_upper[1], p.mark_upper_lat}};
    for (auto &m : marks)
        if (m[0] && m[1] && !THROUGHPUT)
            out.push_back(F("vu1fast::markReady<0x%xu>(vfReady + %d, cyc + %du);", components(m[1]), m[0] * 4, m[2]));
    for (int r = 1; r < 16; r++)
        if ((p.mark_vi & (1 << r)) && !THROUGHPUT)
            out.push_back(F("viReady[%d] = cyc + %du;", r, p.mark_vi_lat));
    if (p.mark_acc && !THROUGHPUT)
        out.push_back(F("vu1fast::markReady<0x%xu>(accReady, cyc + 1u);", components(p.mark_acc)));
    if (p.has_settle && !THROUGHPUT)
        out.push_back(F("settle = std::max(settle, cyc + %du);", p.settle));
    if (backup)
        out.push_back(F("bkValue = oldVi; bkReg = %d; bkValid = true;", p.lower_vi_write));
    if (e_up.writes_vf0 || e_lo.writes_vf0)
        out.push_back("{ static const float vf0[4] = {0.0f, 0.0f, 0.0f, 1.0f}; std::memcpy(s.vf[0], vf0, 16); }");
    if (e_up.writes_vi0 || e_lo.writes_vi0)
        out.push_back("s.vi[0] = 0;");
    w.branch = e_lo.branch;
    w.target = e_lo.target;
    w.comment = F("// 0x%04x: ", p.address) + upper_name(p.upper) + (p.e_bit ? "[E]" : "") + (p.i_bit ? "[I]" : "") +
                " | " + (p.i_bit ? F("LOI 0x%08x", p.lower) : lower_name(p.lower));
    return w;
}

std::vector<std::string> emit_chunk(Program &prog, int64_t start, int64_t end, const std::string &fn_name,
                                    std::vector<int64_t> &inside)
{
    inside.clear();
    for (auto &kv : prog.pairs)
        if (start <= kv.first && kv.first < end && prog.compiled(kv.first))
            inside.push_back(kv.first);
    std::set<int64_t> labels(inside.begin(), inside.end());
    std::vector<std::string> body, tails;
    auto has = [&](int64_t t) { return labels.count(t) != 0; };
    auto go = [&](int64_t t) {
        return has(t) ? F("goto P_%04x;", static_cast<unsigned>(t)) : F("VU1C_EXIT(0x%04xu);", static_cast<unsigned>(t));
    };
    auto ind = [](std::vector<std::string> &dst, const std::vector<std::string> &src) {
        for (auto &x : src)
            dst.push_back("    " + x);
    };

    for (int64_t a : inside)
    {
        const Pair &p = prog.pairs.at(a);
        unsigned ua = static_cast<unsigned>(a);
        Work w = pair_work(prog, p, F("VU1C_EXIT(0x%04xu);", ua));
        body.push_back(F("P_%04x: ", ua) + w.comment);
        body.push_back("{");
        if (w.branch == BR_IF)
            body.push_back("    bool taken;");
        ind(body, w.out);
        int64_t nxt = next_pc(a);
        unsigned un = static_cast<unsigned>(nxt);
        if (p.e_bit)
        {
            body.push_back("    s.ebit = true;");
            body.push_back("    VU1C_ADVANCE();");
            body.push_back(F("    goto E_%04x;", ua));
            tails.push_back(F("E_%04x: // the E-bit delay slot", ua));
            if (prog.plain(nxt))
            {
                Work w2 = pair_work(prog, prog.pairs.at(nxt), F("VU1C_EXIT(0x%04xu);", un));
                tails.push_back("{ " + w2.comment);
                ind(tails, w2.out);
                tails.push_back(F("    s.pc = 0x%04xu;", static_cast<unsigned>(next_pc(nxt))));
                tails.push_back("    VU1C_ADVANCE();");
                tails.push_back("    pairsRun += pairs;");
                tails.push_back("    return Vu1CompiledResult::ProgramEnded;");
                tails.push_back("}");
            }
            else
                tails.push_back(F("VU1C_EXIT(0x%04xu);", un));
        }
        else if (w.branch != BR_NONE)
        {
            body.push_back("    VU1C_ADVANCE();");
            if (w.branch == BR_IF)
            {
                body.push_back(F("    if (taken) goto D_%04x;", ua));
                if (nxt != end || has(nxt))
                    body.push_back("    " + go(nxt));
                else
                    body.push_back(F("    VU1C_EXIT(0x%04xu);", un));
            }
            else
                body.push_back(F("    goto D_%04x;", ua));
            bool dynamic = w.branch == BR_JUMP;
            std::string tgt = dynamic ? std::string("jumpTarget") : F("0x%04xu", static_cast<unsigned>(w.target));
            tails.push_back(F("D_%04x: // the delay slot of the branch at 0x%04x", ua, ua));
            if (prog.plain(nxt))
            {
                Work w2 = pair_work(prog, prog.pairs.at(nxt), F("VU1C_EXIT_DELAY(0x%04xu, %s);", un, tgt.c_str()));
                tails.push_back("{ " + w2.comment);
                ind(tails, w2.out);
                tails.push_back("    VU1C_ADVANCE();");
                if (dynamic)
                {
                    tails.push_back("    s.pc = jumpTarget;");
                    tails.push_back("    goto dispatch;");
                }
                else
                    tails.push_back("    " + go(w.target));
                tails.push_back("}");
            }
            else
                tails.push_back(F("VU1C_EXIT_DELAY(0x%04xu, %s);", un, tgt.c_str()));
        }
        else
        {
            body.push_back("    VU1C_ADVANCE();");
            if (!has(nxt) || nxt != a + 8)
                body.push_back("    " + go(nxt));
        }
        body.push_back("}");
    }

    std::vector<std::string> out;
    out.push_back("static Vu1CompiledResult " + fn_name + "(VU1Interpreter &vu, uint64_t budgetEnd, uint64_t &pairsRun)");
    out.push_back("{");
    static const char *pre[] = {
        "VU1State &s = Vu1Compiled::state(vu);",
        "uint64_t &cycle = Vu1Compiled::cycle(vu);",
        "uint64_t cyc = cycle; // in a register: written through at every advance, reloaded after a stall",
        "const uint64_t &nextCommit = Vu1Compiled::nextCommitCycle(vu);",
        "const bool &kickActive = Vu1Compiled::xgkickActive(vu);",
        "const bool &stop = Vu1Compiled::stopRequested(vu);",
        "uint64_t *const vfReady = Vu1Compiled::vfReadyData(vu);",
        "uint64_t *const viReady = Vu1Compiled::viReadyData(vu);",
        "uint64_t *const accReady = Vu1Compiled::accReadyData(vu);",
        "uint64_t &settle = Vu1Compiled::settleCycle(vu);",
        "bool &bkValid = Vu1Compiled::branchBackupValid(vu);",
        "int32_t &bkValue = Vu1Compiled::branchBackupValue(vu);",
        "uint8_t &bkReg = Vu1Compiled::branchBackupReg(vu);",
        "const auto &fdiv = Vu1Compiled::fdiv(vu);",
        "const auto &efu = Vu1Compiled::efu(vu);",
        "const uint64_t &efuReady = Vu1Compiled::efuResourceReady(vu);",
        "uint8_t *const data = Vu1Compiled::vuData(vu);",
        "const auto *const dec = Vu1Compiled::decodedPairs(vu);",
        "uint64_t pairs = 0u;",
        "uint32_t jumpTarget = 0u;",
        "(void)fdiv; (void)efu; (void)efuReady; (void)data; (void)jumpTarget; (void)kickActive;"};
    for (const char *x : pre)
        out.push_back(std::string("    ") + x);
    out.push_back("dispatch:");
    out.push_back("    switch (s.pc)");
    out.push_back("    {");
    for (int64_t a : inside)
        out.push_back(F("    case 0x%04xu: goto P_%04x;", static_cast<unsigned>(a), static_cast<unsigned>(a)));
    out.push_back("    default: VU1C_EXIT(s.pc);");
    out.push_back("    }");
    out.insert(out.end(), body.begin(), body.end());
    out.insert(out.end(), tails.begin(), tails.end());
    out.push_back("}");
    return out;
}

std::vector<std::string> split_ws(const std::string &s)
{
    std::vector<std::string> r;
    std::istringstream is(s);
    std::string t;
    while (is >> t)
        r.push_back(t);
    return r;
}

std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n\f\v"), b = s.find_last_not_of(" \t\r\n\f\v");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

int64_t parse_int(const std::string &s, int base)
{
    std::string t = trim(s);
    char *endp = nullptr;
    long long v = std::strtoll(t.c_str(), &endp, base);
    if (t.empty() || *endp)
        die("vu1_recomp: invalid literal for int(): '" + s + "'");
    return v;
}

std::string upper_str(std::string s)
{
    for (char &c : s)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 32);
    return s;
}
} // namespace

int main(int argc, char **argv)
{
    namespace fs = std::filesystem;
    std::string ELF = argc > 1 ? argv[1] : (fs::path("build") / "elf" / "SLUS_215.90").string();
    std::string OUT = argc > 2 ? argv[2] : (fs::path("generated") / "vu1_compiled.cpp").string();
    std::string UPLOADS = argc > 3 ? argv[3] : "";

    if (!fs::exists(ELF))
        die("vu1_recomp: " + ELF + " not found (python tools/recompile.py extracts it from your disc)");
    Bytes elf;
    if (!read_file(ELF, elf))
        die("vu1_recomp: cannot read " + ELF);
    std::vector<Set> sets;
    if (!UPLOADS.empty())
    {
        std::ifstream f(UPLOADS);
        if (!f)
            die("vu1_recomp: cannot open " + UPLOADS);
        std::string raw;
        while (std::getline(f, raw))
        {
            auto tok = split_ws(raw.substr(0, raw.find('#')));
            if (tok.size() != 3)
                continue;
            int64_t src = parse_int(tok[0], 16), vu = parse_int(tok[1], 16), pairs = parse_int(tok[2], 10);
            Bytes code = read_vaddr_range(elf, static_cast<uint32_t>(src), static_cast<uint32_t>(src + pairs * 8));
            if (!sets.empty() && sets.back().source + static_cast<int64_t>(sets.back().code.size()) == src + 4 &&
                sets.back().vu + static_cast<int64_t>(sets.back().code.size()) == vu)
                sets.back().code.insert(sets.back().code.end(), code.begin(), code.end());
            else
                sets.push_back({src + 4, vu, code});
        }
        std::set<int64_t> cutset;
        for (auto &s : sets)
        {
            cutset.insert(s.vu);
            cutset.insert(s.vu + static_cast<int64_t>(s.code.size()));
        }
        std::vector<Set> split;
        for (auto &st : sets)
        {
            int64_t lo = st.vu, hi = st.vu + static_cast<int64_t>(st.code.size());
            std::vector<int64_t> edges{lo};
            for (int64_t c : cutset)
                if (lo < c && c < hi)
                    edges.push_back(c);
            edges.push_back(hi);
            for (size_t k = 0; k + 1 < edges.size(); k++)
            {
                int64_t a = edges[k], b = edges[k + 1];
                split.push_back({st.source + (a - lo), a, slice(st.code, a - lo, b - lo)});
            }
        }
        sets = split;
    }
    else
    {
        const char *env = std::getenv("HCTP_VU1_RANGES");
        std::string ranges = env ? env : "";
        if (ranges.empty() && upper_str(fs::path(ELF).filename().string()).rfind("SLUS_207.87", 0) == 0)
        {
            fs::path rp = fs::path(argv[0]).parent_path() / "hctp_vu1_ranges.txt";
            if (!fs::exists(rp))
                rp = fs::path("tools") / "hctp_vu1_ranges.txt";
            std::ifstream rf(rp, std::ios::binary);
            if (!rf)
                die("vu1_recomp: cannot open " + rp.string());
            ranges = trim(std::string(std::istreambuf_iterator<char>(rf), std::istreambuf_iterator<char>()));
        }
        if (!ranges.empty())
        {
            std::stringstream ss(ranges);
            std::string r;
            while (std::getline(ss, r, ','))
            {
                size_t c = r.find(':');
                if (c == std::string::npos)
                    die("vu1_recomp: bad range " + r);
                uint32_t start = static_cast<uint32_t>(parse_int(r.substr(0, c), 16));
                uint32_t end = static_cast<uint32_t>(parse_int(r.substr(c + 1), 16));
                auto more = program_sets(start, read_vaddr_range(elf, start, end));
                sets.insert(sets.end(), more.begin(), more.end());
            }
        }
        else
        {
            uint32_t vaddr = 0;
            Bytes vutext;
            read_vutext(elf, ELF, vaddr, vutext);
            sets = program_sets(vaddr, vutext);
        }
    }

    std::vector<std::string> lines = {
        "// Generated by tools/vu1_recomp.py from SLUS_206.39 (the game's VU1 microprograms) - do not edit, do not commit.",
        "// See PS2Recomp ps2xRuntime/include/runtime/ps2_vu1_compiled.h.",
        "#include \"runtime/ps2_vu1_compiled.h\"",
        "",
        "#include <algorithm>",
        "#include <cmath>",
        "#include <cstdint>",
        "#include <cstdlib>",
        "#include <cstring>",
        "",
        "#if defined(_MSC_VER)",
        "#pragma float_control(precise, on, push) // the VU maths must not be reordered or fused",
        "#endif",
        "",
        "#define VU1C_EXIT(pcValue) do { s.pc = (pcValue); pairsRun += pairs; return Vu1CompiledResult::Continue; } while (0)",
        "#define VU1C_EXIT_DELAY(pcValue, target) do { s.branchPending = true; s.branchTarget = (target); s.branchDelay = 0u; VU1C_EXIT(pcValue); } while (0)",
        "#define VU1C_ADVANCE() do { cycle = ++cyc; if (cyc >= nextCommit) Vu1Compiled::commit(vu); if (kickActive) Vu1Compiled::progressXgkick(vu); ++pairs; } while (0)",
        "",
        "namespace",
        "{"};
    size_t total_pairs = 0, total_compiled = 0;
    for (int variant = 0; variant < 2; variant++)
    {
        THROUGHPUT = variant == 1;
        const char *sfx = THROUGHPUT ? "_t" : "";
        std::vector<std::string> pieces;
        total_pairs = total_compiled = 0;
        for (size_t n = 0; n < sets.size(); n++)
        {
            const Set &st = sets[n];
            Program prog(static_cast<int>(n), st.vu, st.code);
            std::map<int64_t, std::string> fns;
            int64_t start = st.vu;
            int64_t end_all = st.vu + static_cast<int64_t>(st.code.size());
            int chunk = 0;
            while (start < end_all)
            {
                int64_t end = std::min<int64_t>(start + CHUNK_PAIRS * 8, end_all);
                std::string name = F("vu1c_p%zu_c%d%s", n, chunk, sfx);
                std::vector<int64_t> inside;
                auto code = emit_chunk(prog, start, end, name, inside);
                lines.insert(lines.end(), code.begin(), code.end());
                lines.push_back("");
                for (int64_t a : inside)
                    fns[a] = name;
                start = end;
                chunk++;
            }
            size_t nwords = st.code.size() / 4;
            lines.push_back(F("const uint32_t vu1c_p%zu%s_code[] = {", n, sfx));
            for (size_t k = 0; k < nwords; k += 8)
            {
                std::string l = "    ";
                for (size_t j = k; j < std::min(nwords, k + 8); j++)
                {
                    const uint8_t *b = &st.code[j * 4];
                    uint32_t wv = b[0] | (b[1] << 8) | (b[2] << 16) | (static_cast<uint32_t>(b[3]) << 24);
                    if (j > k)
                        l += ", ";
                    l += F("0x%08xu", wv);
                }
                lines.push_back(l + ",");
            }
            lines.push_back("};");
            lines.push_back(F("const Vu1CompiledFn vu1c_p%zu%s_fns[] = {", n, sfx));
            std::vector<int64_t> addrs;
            for (auto &kv : prog.pairs)
                addrs.push_back(kv.first);
            for (size_t k = 0; k < addrs.size(); k += 8)
            {
                std::string l = "    ";
                for (size_t j = k; j < std::min(addrs.size(), k + 8); j++)
                {
                    if (j > k)
                        l += ", ";
                    auto it = fns.find(addrs[j]);
                    l += it == fns.end() ? std::string("nullptr") : it->second;
                }
                lines.push_back(l + ",");
            }
            lines.push_back("};");
            lines.push_back(F("const Vu1PairShape vu1c_p%zu%s_shapes[] = {", n, sfx));
            for (int64_t a : addrs)
                lines.push_back("    " + prog.pairs.at(a).shape() + ",");
            lines.push_back("};");
            lines.push_back("");
            std::string pname = F("VU1 program %zu (ELF 0x%06llx)", n, static_cast<unsigned long long>(st.source - 4));
            pieces.push_back(F("{0x%04xu, %zuu, vu1c_p%zu%s_code, vu1c_p%zu%s_fns, vu1c_p%zu%s_shapes, \"%s\"}",
                               static_cast<unsigned>(st.vu), addrs.size(), n, sfx, n, sfx, n, sfx, pname.c_str()));
            total_pairs += addrs.size();
            total_compiled += fns.size();
            std::string generic;
            for (auto &kv : prog.generic)
            {
                if (!generic.empty())
                    generic += ", ";
                generic += F("%s x%d", kv.first.c_str(), kv.second);
            }
            if (generic.empty())
                generic = "none";
            std::printf("  program %zu: VU 0x%04x..0x%04x, %zu pairs, %zu compiled, left to the interpreter: %s\n", n,
                        static_cast<unsigned>(st.vu), static_cast<unsigned>(end_all), addrs.size(), fns.size(),
                        generic.c_str());
        }
        lines.push_back(F("const Vu1CompiledPiece vu1c_pieces%s[] = {", sfx));
        for (auto &p : pieces)
            lines.push_back("    " + p + ",");
        lines.push_back("};");
        lines.push_back("");
    }
    const char *tail[] = {
        "struct Vu1CompiledRegistrar",
        "{",
        "    Vu1CompiledRegistrar()",
        "    {",
        "        const char *fast = std::getenv(\"PS2X_VU1_FAST\"); // throughput variant: no stall/ready bookkeeping, plain MADD",
        "        if (!(fast && fast[0] == '0')) // default on; PS2X_VU1_FAST=0 = the cycle-exact variant",
        "            vu1RegisterCompiledPieces(vu1c_pieces_t, sizeof(vu1c_pieces_t) / sizeof(vu1c_pieces_t[0]));",
        "        else",
        "            vu1RegisterCompiledPieces(vu1c_pieces, sizeof(vu1c_pieces) / sizeof(vu1c_pieces[0]));",
        "    }",
        "} vu1c_registrar;",
        "}",
        "",
        "#undef VU1C_EXIT",
        "#undef VU1C_EXIT_DELAY",
        "#undef VU1C_ADVANCE",
        "#if defined(_MSC_VER)",
        "#pragma float_control(pop)",
        "#endif"};
    for (const char *t : tail)
        lines.push_back(t);

    size_t total = 0;
    for (auto &l : lines)
        total += l.size() + 1;
    std::string text;
    text.reserve(total);
    for (auto &l : lines)
    {
        text += l;
        text += '\n';
    }

    fs::path outp(OUT);
    if (outp.has_parent_path())
    {
        std::error_code ec;
        fs::create_directories(outp.parent_path(), ec);
    }
    Bytes old;
    if (fs::exists(outp) && read_file(OUT, old))
    {
        // Python reads it back in text mode (universal newlines)
        std::string o;
        o.reserve(old.size());
        for (size_t k = 0; k < old.size(); k++)
        {
            if (old[k] == '\r')
            {
                o += '\n';
                if (k + 1 < old.size() && old[k + 1] == '\n')
                    k++;
            }
            else
                o += static_cast<char>(old[k]);
        }
        if (o == text)
        {
            std::printf("vu1_recomp: %zu programs, %zu pairs (%zu compiled) - %s unchanged\n", sets.size(), total_pairs,
                        total_compiled, OUT.c_str());
            return 0;
        }
    }
    {
        std::ofstream f(OUT, std::ios::binary | std::ios::trunc);
        if (!f)
            die("vu1_recomp: cannot write " + OUT);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!f)
            die("vu1_recomp: write failed: " + OUT);
    }
    std::printf("vu1_recomp: %zu programs, %zu pairs (%zu compiled) -> %s (%zu lines)\n", sets.size(), total_pairs,
                total_compiled, OUT.c_str(), lines.size());
    return 0;
}

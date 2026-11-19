// [mods] A mod's files where they are: the manager's copy, the player's own file, or one entry of a .zip / .vcm
// (vcs_modfiles.h; docs/ANDROID.md "Mods").
#include "game/vcs_modfiles.h"

#include "platform/platform.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

namespace fs = std::filesystem;

namespace vcs::mods::files
{
    namespace
    {
        constexpr size_t kMaxContainer = 256u << 20; // a container read whole (a pipe, a .vcm): the manager's limit

        std::string decode(const std::string &s)
        {
            std::string out;
            out.reserve(s.size());
            for (size_t i = 0; i < s.size(); ++i)
            {
                if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                    std::isxdigit(static_cast<unsigned char>(s[i + 2])))
                {
                    out.push_back(static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16)));
                    i += 2;
                }
                else
                    out.push_back(s[i]);
            }
            return out;
        }

        bool absolute(const std::string &p)
        {
            return (!p.empty() && (p[0] == '/' || p[0] == '\\')) ||
                   (p.size() > 2 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':' && (p[2] == '/' || p[2] == '\\'));
        }

        bool climbs(const std::string &p)
        {
            size_t start = 0;
            while (start <= p.size())
            {
                const size_t end = p.find_first_of("/\\", start);
                const std::string part = p.substr(start, end == std::string::npos ? std::string::npos : end - start);
                if (part == "..")
                    return true;
                if (end == std::string::npos)
                    break;
                start = end + 1;
            }
            return false;
        }

        // VCS_MODS_PLACES: "<handle>=<fd> ..." (VCSActivity); a value that isn't a number is a path (a desktop test)
        std::string place(const std::string &handle)
        {
            const char *env = std::getenv("VCS_MODS_PLACES");
            if (!env)
                return "";
            const std::string all = env;
            size_t at = 0;
            while (at < all.size())
            {
                const size_t end = std::min(all.find(' ', at), all.size());
                const std::string tok = all.substr(at, end - at);
                const size_t eq = tok.find('=');
                if (eq != std::string::npos && tok.compare(0, eq, handle) == 0 && eq == handle.size())
                {
                    const std::string v = tok.substr(eq + 1);
                    const bool fd = !v.empty() && std::all_of(v.begin(), v.end(), [](char c) { return c >= '0' && c <= '9'; });
                    return fd ? "/proc/self/fd/" + v : decode(v);
                }
                at = end + 1;
            }
            return "";
        }

        uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }
        uint16_t le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

        std::string lower(std::string s)
        {
            for (char &c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        std::string leaf(const std::string &s)
        {
            const size_t slash = s.find_last_of("/\\");
            return slash == std::string::npos ? s : s.substr(slash + 1);
        }

        // Random access to a file; a stream that can't seek (a document provider's pipe) is read whole first
        class Source
        {
        public:
            explicit Source(const fs::path &p) : in_(p, std::ios::binary)
            {
                if (!in_)
                    return;
                in_.seekg(0, std::ios::end);
                const std::streamoff end = in_.tellg();
                if (end > 0)
                {
                    size_ = static_cast<uint64_t>(end);
                    ok_ = true;
                    return;
                }
                in_.clear();
                in_.seekg(0);
                std::vector<uint8_t> all;
                char buf[1 << 15];
                while (in_.read(buf, sizeof buf) || in_.gcount() > 0)
                {
                    all.insert(all.end(), buf, buf + in_.gcount());
                    if (all.size() > kMaxContainer)
                        return;
                }
                whole_ = std::move(all);
                size_ = whole_.size();
                buffered_ = ok_ = size_ > 0;
            }
            bool ok() const { return ok_; }
            uint64_t size() const { return size_; }
            bool at(uint64_t off, size_t n, std::vector<uint8_t> &out)
            {
                if (off > size_ || n > size_ - off)
                    return false;
                if (buffered_)
                {
                    out.assign(whole_.begin() + static_cast<std::ptrdiff_t>(off), whole_.begin() + static_cast<std::ptrdiff_t>(off + n));
                    return true;
                }
                out.resize(n);
                in_.clear();
                in_.seekg(static_cast<std::streamoff>(off));
                return n == 0 || static_cast<bool>(in_.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(n)));
            }

        private:
            std::ifstream in_;
            std::vector<uint8_t> whole_;
            uint64_t size_ = 0;
            bool ok_ = false, buffered_ = false;
        };

        // One entry of a .zip: the central directory names it (exactly, else the only one with that name in any
        // folder, case ignored), its local header says where the data is. Stored (0) and deflated (8) entries; no
        // zip64, no encryption (the manager refuses those at import).
        bool readZip(Source &src, const std::string &entry, std::vector<uint8_t> &out, size_t limit, std::string &error)
        {
            std::vector<uint8_t> tail;
            const uint64_t tailSize = std::min<uint64_t>(src.size(), 65557u);
            if (!src.at(src.size() - tailSize, static_cast<size_t>(tailSize), tail))
                return error = "can't read the zip", false;
            size_t eocd = std::string::npos;
            for (size_t i = tail.size() >= 22 ? tail.size() - 22 : 0; i + 22 <= tail.size(); --i)
            {
                if (le32(&tail[i]) == 0x06054B50u)
                {
                    eocd = i;
                    break;
                }
                if (i == 0)
                    break;
            }
            if (eocd == std::string::npos)
                return error = "not a zip (no end of central directory)", false;
            const uint32_t count = le16(&tail[eocd + 10]), cdSize = le32(&tail[eocd + 12]), cdOff = le32(&tail[eocd + 16]);
            std::vector<uint8_t> cd;
            if (cdSize > (64u << 20) || !src.at(cdOff, cdSize, cd))
                return error = "the zip's directory can't be read", false;
            struct Hit
            {
                uint16_t method;
                uint32_t comp, size, local;
            };
            Hit exact{}, byLeaf{};
            bool haveExact = false;
            int leafHits = 0;
            const std::string wantLeaf = lower(leaf(entry));
            size_t p = 0;
            for (uint32_t i = 0; i < count && p + 46 <= cd.size(); ++i)
            {
                if (le32(&cd[p]) != 0x02014B50u)
                    break;
                const uint16_t n = le16(&cd[p + 28]), extra = le16(&cd[p + 30]), comment = le16(&cd[p + 32]);
                if (p + 46 + n > cd.size())
                    break;
                const std::string name(reinterpret_cast<const char *>(&cd[p + 46]), n);
                const Hit h{le16(&cd[p + 10]), le32(&cd[p + 20]), le32(&cd[p + 24]), le32(&cd[p + 42])};
                if (name == entry)
                {
                    exact = h;
                    haveExact = true;
                    break;
                }
                if (!name.empty() && name.back() != '/' && lower(leaf(name)) == wantLeaf)
                {
                    byLeaf = h;
                    ++leafHits;
                }
                p += 46u + n + extra + comment;
            }
            if (!haveExact && leafHits != 1)
                return error = leafHits ? "more than one " + leaf(entry) + " in the zip" : leaf(entry) + " is not in the zip", false;
            const Hit h = haveExact ? exact : byLeaf;
            if (h.size > limit || h.comp > kMaxContainer)
                return error = leaf(entry) + " is too big", false;
            std::vector<uint8_t> local;
            if (!src.at(h.local, 30, local) || le32(local.data()) != 0x04034B50u)
                return error = "the zip is damaged (local header)", false;
            const uint64_t data = static_cast<uint64_t>(h.local) + 30u + le16(&local[26]) + le16(&local[28]);
            std::vector<uint8_t> packed;
            if (!src.at(data, h.comp, packed))
                return error = "the zip is cut short", false;
            if (h.method == 0)
                out = std::move(packed);
            else if (h.method != 8 || !platform::inflate(packed.data(), packed.size(), true, limit, out))
                return error = h.method == 8 ? leaf(entry) + " can't be unpacked" : "an unknown zip method", false;
            return true;
        }

        // A VC Mod Manager .vcm: "vcm\0" + 32 hex digits, then !NAME!0sf<bytes>!NAME!ef (stored) / !NAME!1sf<zlib>...
        // (the same reading as ModFormats.unvcm / mods.py unvcm)
        bool readVcm(Source &src, const std::string &entry, std::vector<uint8_t> &out, size_t limit, std::string &error)
        {
            std::vector<uint8_t> d;
            if (src.size() > kMaxContainer || !src.at(0, static_cast<size_t>(src.size()), d))
                return error = "can't read the .vcm", false;
            size_t at = 36;
            while (at < d.size() && d[at] == '!')
            {
                const auto nameEnd = std::find(d.begin() + static_cast<std::ptrdiff_t>(at + 1),
                                               d.begin() + static_cast<std::ptrdiff_t>(std::min(d.size(), at + 200)), '!');
                const size_t ne = static_cast<size_t>(nameEnd - d.begin());
                if (ne >= d.size() || ne + 3 >= d.size() || d[ne + 2] != 's' || d[ne + 3] != 'f')
                    break;
                const std::string name(reinterpret_cast<const char *>(&d[at + 1]), ne - at - 1);
                const bool zlib = d[ne + 1] == '1';
                const std::string endTag = "!" + name + "!ef";
                const size_t body = ne + 4;
                const auto stop = std::search(d.begin() + static_cast<std::ptrdiff_t>(body), d.end(), endTag.begin(), endTag.end());
                if (stop == d.end())
                    break;
                const size_t s = static_cast<size_t>(stop - d.begin());
                if (name == entry || lower(leaf(name)) == lower(leaf(entry)))
                {
                    if (!zlib)
                    {
                        if (s - body > limit)
                            return error = leaf(entry) + " is too big", false;
                        out.assign(d.begin() + static_cast<std::ptrdiff_t>(body), stop);
                        return true;
                    }
                    if (!platform::inflate(&d[body], s - body, false, limit, out))
                        return error = leaf(entry) + " can't be unpacked", false;
                    return true;
                }
                at = s + endTag.size();
            }
            return error = leaf(entry) + " is not in the .vcm", false;
        }
    }

    fs::path pathOf(const std::string &utf8)
    {
        return fs::path(std::u8string(reinterpret_cast<const char8_t *>(utf8.data()), utf8.size()));
    }

    Location resolve(const std::string &value, const fs::path &modsDir)
    {
        Location l;
        const size_t bang = value.find('!');
        const std::string file = decode(value.substr(0, bang));
        l.entry = bang == std::string::npos ? "" : decode(value.substr(bang + 1));
        l.shown = file + (l.entry.empty() ? "" : "!" + l.entry);
        if (file.empty())
            return l.error = "no file", l;
        if (file.rfind("place:", 0) == 0)
        {
            l.inPlace = true;
            const std::string p = place(file.substr(6));
            if (p.empty())
                return l.error = "original missing (no access to it: the app's permission is gone, or it was moved)", l;
            l.file = pathOf(p);
            return l;
        }
        if (absolute(file))
        {
            l.inPlace = true;
            l.file = pathOf(file);
            return l;
        }
        if (climbs(file))
            return l.error = "a path out of the mods folder", l;
        l.file = modsDir / pathOf(file);
        return l;
    }

    bool available(const Location &l)
    {
        if (!l.error.empty())
            return false;
        std::ifstream in(l.file, std::ios::binary);
        return static_cast<bool>(in);
    }

    std::string stamp(const Location &l)
    {
        std::error_code ec;
        if (!l.error.empty() || !fs::is_regular_file(l.file, ec))
            return "";
        const uintmax_t size = fs::file_size(l.file, ec);
        if (ec || size == 0)
            return "";
        const auto time = fs::last_write_time(l.file, ec);
        if (ec)
            return "";
        return std::to_string(size) + " " + std::to_string(static_cast<long long>(time.time_since_epoch().count())) +
               (l.entry.empty() ? "" : " " + l.entry);
    }

    bool read(const Location &l, std::vector<uint8_t> &out, size_t limit, std::string &error)
    {
        out.clear();
        if (!l.error.empty())
            return error = l.error, false;
        Source src(l.file);
        if (!src.ok())
            return error = l.inPlace ? "original missing (" + l.shown + ")" : l.shown + " missing", false;
        if (l.entry.empty())
        {
            if (src.size() > limit)
                return error = l.shown + " is too big", false;
            return src.at(0, static_cast<size_t>(src.size()), out) || (error = "can't read " + l.shown, false);
        }
        std::vector<uint8_t> magic;
        if (!src.at(0, 4, magic))
            return error = "can't read " + l.shown, false;
        if (magic[0] == 'P' && magic[1] == 'K')
            return readZip(src, l.entry, out, limit, error);
        if (std::memcmp(magic.data(), "vcm", 4) == 0)
            return readVcm(src, l.entry, out, limit, error);
        return error = l.shown + ": not a .zip or a .vcm", false;
    }
}

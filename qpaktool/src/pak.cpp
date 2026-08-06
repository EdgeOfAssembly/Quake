/**
 * @file pak.cpp
 * @brief Quake PAK reader implementation.
 */

#include "qpak/pak.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <fnmatch.h>
#include <sys/stat.h>

namespace qpak
{
namespace
{

constexpr std::size_t kHeaderSize = 12;
constexpr std::size_t kEntrySize = 64;
constexpr std::size_t kNameSize = 56;

[[nodiscard]] bool read_all(std::string const& path, std::vector<std::uint8_t>& out,
                            std::string& err)
{
    std::ifstream in{path, std::ios::binary};
    if (!in)
    {
        err = "cannot open: " + path + " (" + std::strerror(errno) + ")";
        return false;
    }
    in.seekg(0, std::ios::end);
    auto const end = in.tellg();
    if (end < 0)
    {
        err = "seek failed: " + path;
        return false;
    }
    auto const n = static_cast<std::size_t>(end);
    in.seekg(0, std::ios::beg);
    out.resize(n);
    if (n > 0)
    {
        in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(n));
        if (!in)
        {
            err = "read failed: " + path;
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::uint32_t rd_u32_le(std::uint8_t const* p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

} // namespace

bool PakArchive::open(std::string const& path, std::string& err)
{
    path_.clear();
    entries_.clear();

    std::vector<std::uint8_t> data{};
    if (!read_all(path, data, err))
    {
        return false;
    }
    if (data.size() < kHeaderSize)
    {
        err = "file too small for PAK header";
        return false;
    }
    if (data[0] != 'P' || data[1] != 'A' || data[2] != 'C' || data[3] != 'K')
    {
        err = "not a Quake PAK (missing PACK magic)";
        return false;
    }

    std::uint32_t const dirofs = rd_u32_le(data.data() + 4);
    std::uint32_t const dirlen = rd_u32_le(data.data() + 8);
    if (dirlen % kEntrySize != 0)
    {
        err = "invalid directory size (not multiple of 64)";
        return false;
    }
    if (static_cast<std::uint64_t>(dirofs) + dirlen > data.size())
    {
        err = "directory table out of range";
        return false;
    }

    std::size_t const n = dirlen / kEntrySize;
    entries_.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        std::uint8_t const* ent = data.data() + dirofs + i * kEntrySize;
        char namebuf[kNameSize + 1]{};
        std::memcpy(namebuf, ent, kNameSize);
        namebuf[kNameSize] = '\0';
        // trim at first NUL
        std::string name{namebuf};
        std::uint32_t const off = rd_u32_le(ent + 56);
        std::uint32_t const sz = rd_u32_le(ent + 60);
        if (static_cast<std::uint64_t>(off) + sz > data.size())
        {
            err = "entry data out of range: " + name;
            return false;
        }
        PakEntry e{};
        e.name = std::move(name);
        e.offset = off;
        e.size = sz;
        entries_.push_back(std::move(e));
    }

    path_ = path;
    return true;
}

bool PakArchive::read_entry(PakEntry const& entry, std::vector<std::uint8_t>& out,
                            std::string& err) const
{
    out.clear();
    if (path_.empty())
    {
        err = "archive not open";
        return false;
    }
    std::ifstream in{path_, std::ios::binary};
    if (!in)
    {
        err = "cannot reopen: " + path_;
        return false;
    }
    in.seekg(static_cast<std::streamoff>(entry.offset), std::ios::beg);
    if (!in)
    {
        err = "seek failed for " + entry.name;
        return false;
    }
    out.resize(entry.size);
    if (entry.size > 0)
    {
        in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(entry.size));
        if (!in)
        {
            err = "read failed for " + entry.name;
            return false;
        }
    }
    return true;
}

bool PakArchive::matches(std::string_view name, std::string_view pattern, MatchMode mode)
{
    switch (mode)
    {
    case MatchMode::All:
        return true;
    case MatchMode::Exact:
        return name == pattern;
    case MatchMode::Wildcard:
    {
        // FNM_PATHNAME would make * not cross / — we want full-path globs like progs/*.mdl
        std::string n{name};
        std::string p{pattern};
        int const rc = ::fnmatch(p.c_str(), n.c_str(), 0);
        return rc == 0;
    }
    case MatchMode::Regex:
    {
        try
        {
            std::regex re{std::string{pattern}, std::regex::ECMAScript};
            return std::regex_search(std::string{name}, re);
        }
        catch (std::regex_error const&)
        {
            return false;
        }
    }
    }
    return false;
}

bool ensure_parent_dirs(std::string const& file_path, std::string& err)
{
    namespace fs = std::filesystem;
    fs::path const p{file_path};
    fs::path const parent = p.parent_path();
    if (parent.empty())
    {
        return true;
    }
    std::error_code ec{};
    fs::create_directories(parent, ec);
    if (ec)
    {
        err = "mkdir failed: " + parent.string() + " (" + ec.message() + ")";
        return false;
    }
    return true;
}

bool write_file(std::string const& path, std::uint8_t const* data, std::size_t len,
                std::string& err)
{
    if (!ensure_parent_dirs(path, err))
    {
        return false;
    }
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out)
    {
        err = "cannot write: " + path;
        return false;
    }
    if (len > 0 && data != nullptr)
    {
        out.write(reinterpret_cast<char const*>(data), static_cast<std::streamsize>(len));
        if (!out)
        {
            err = "write failed: " + path;
            return false;
        }
    }
    return true;
}

} // namespace qpak

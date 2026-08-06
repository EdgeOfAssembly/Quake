/**
 * @file pak.hpp
 * @brief Quake .pak archive reader (PACK format).
 *
 * Spec: https://quakewiki.org/wiki/.pak
 * Header 12 bytes little-endian; directory entries 64 bytes (56-byte path).
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace qpak
{

/**
 * @brief One file entry inside a PAK.
 */
struct PakEntry
{
    std::string name{};   ///< Path inside archive, e.g. progs/ogre.mdl
    std::uint32_t offset{0};
    std::uint32_t size{0};
};

/**
 * @brief Match mode for list/extract filters.
 */
enum class MatchMode
{
    All,      ///< No pattern — match everything
    Exact,    ///< Full path equality
    Wildcard, ///< Shell-style * ? (fnmatch)
    Regex     ///< ECMAScript std::regex
};

/**
 * @brief In-memory view of a Quake PAK file.
 */
class PakArchive
{
public:
    /**
     * @brief Open and parse a PAK from disk.
     * @param[in] path Filesystem path to .pak
     * @param[out] err Human-readable error on failure
     * @return true on success
     */
    [[nodiscard]] bool open(std::string const& path, std::string& err);

    /**
     * @brief Number of directory entries.
     */
    [[nodiscard]] std::size_t size() const noexcept
    {
        return entries_.size();
    }

    /**
     * @brief All entries (const).
     */
    [[nodiscard]] std::vector<PakEntry> const& entries() const noexcept
    {
        return entries_;
    }

    /**
     * @brief Source path last opened.
     */
    [[nodiscard]] std::string const& path() const noexcept
    {
        return path_;
    }

    /**
     * @brief Read raw bytes for one entry into @p out.
     * @return true on success
     */
    [[nodiscard]] bool read_entry(PakEntry const& entry, std::vector<std::uint8_t>& out,
                                  std::string& err) const;

    /**
     * @brief Test whether @p name matches @p pattern under @p mode.
     */
    [[nodiscard]] static bool matches(std::string_view name, std::string_view pattern,
                                      MatchMode mode);

private:
    std::string path_{};
    std::vector<PakEntry> entries_{};
};

/**
 * @brief Create parent directories for a file path (mkdir -p).
 */
[[nodiscard]] bool ensure_parent_dirs(std::string const& file_path, std::string& err);

/**
 * @brief Write buffer to path (creates parents).
 */
[[nodiscard]] bool write_file(std::string const& path, std::uint8_t const* data, std::size_t len,
                              std::string& err);

} // namespace qpak

/**
 * @file main.cpp
 * @brief qpak — Quake .pak list / extract CLI (C++23).
 */

#include "qpak/pak.hpp"
#include "qpak/version.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace
{

void print_usage(std::FILE* out)
{
    std::fputs(
        "Usage:\n"
        "  qpak list <pak> [--exact|--wildcard|--regex] [pattern]\n"
        "  qpak extract <pak> <outdir> [--exact|--wildcard|--regex] [pattern]\n"
        "  qpak -h | --help\n"
        "  qpak -v | --version\n"
        "\n"
        "List or extract files from a Quake .pak (PACK) archive.\n"
        "With no pattern, list/extract everything.\n"
        "\n"
        "Match modes (optional; default with a pattern is --wildcard):\n"
        "  --exact      Full path must equal pattern\n"
        "  --wildcard   Shell glob (* ?), e.g. 'progs/*.mdl' or '*ogre*'\n"
        "  --regex      ECMAScript regex search on the full path\n"
        "\n"
        "Examples:\n"
        "  qpak list id1/PAK0.PAK\n"
        "  qpak list id1/PAK0.PAK --exact progs/ogre.mdl\n"
        "  qpak list id1/PAK0.PAK --wildcard 'progs/*.mdl'\n"
        "  qpak list id1/PAK0.PAK --regex 'ogre\\.(mdl|wav)$'\n"
        "  qpak extract id1/PAK0.PAK out/\n"
        "  qpak extract id1/PAK0.PAK out/ --wildcard '*ogre*'\n"
        "\n"
        "Stdout: listing lines or extract paths. Diagnostics on stderr.\n",
        out);
}

void print_version()
{
    std::printf("qpak %s\n", QPAK_VERSION_STRING);
}

enum class Cmd
{
    None,
    List,
    Extract
};

struct Options
{
    Cmd cmd{Cmd::None};
    std::string pak_path{};
    std::string out_dir{};
    std::string pattern{};
    qpak::MatchMode mode{qpak::MatchMode::All};
    bool mode_set{false};
};

[[nodiscard]] bool parse_args(int argc, char** argv, Options& opt, std::string& err)
{
    if (argc <= 1)
    {
        return false; // usage
    }

    std::vector<std::string> pos{};
    for (int i = 1; i < argc; ++i)
    {
        std::string_view a{argv[i]};
        if (a == "-h" || a == "--help")
        {
            print_usage(stdout);
            std::exit(0);
        }
        if (a == "-v" || a == "--version")
        {
            print_version();
            std::exit(0);
        }
        if (a == "--exact")
        {
            opt.mode = qpak::MatchMode::Exact;
            opt.mode_set = true;
            continue;
        }
        if (a == "--wildcard" || a == "--glob")
        {
            opt.mode = qpak::MatchMode::Wildcard;
            opt.mode_set = true;
            continue;
        }
        if (a == "--regex")
        {
            opt.mode = qpak::MatchMode::Regex;
            opt.mode_set = true;
            continue;
        }
        if (a.starts_with("-"))
        {
            err = "unknown option: " + std::string{a};
            return false;
        }
        pos.emplace_back(a);
    }

    if (pos.empty())
    {
        err = "missing command";
        return false;
    }

    if (pos[0] == "list")
    {
        opt.cmd = Cmd::List;
        if (pos.size() < 2)
        {
            err = "list requires <pak>";
            return false;
        }
        opt.pak_path = pos[1];
        if (pos.size() >= 3)
        {
            opt.pattern = pos[2];
        }
        if (pos.size() > 3)
        {
            err = "too many arguments for list";
            return false;
        }
    }
    else if (pos[0] == "extract")
    {
        opt.cmd = Cmd::Extract;
        if (pos.size() < 3)
        {
            err = "extract requires <pak> <outdir>";
            return false;
        }
        opt.pak_path = pos[1];
        opt.out_dir = pos[2];
        if (pos.size() >= 4)
        {
            opt.pattern = pos[3];
        }
        if (pos.size() > 4)
        {
            err = "too many arguments for extract";
            return false;
        }
    }
    else
    {
        err = "unknown command: " + pos[0] + " (use list or extract)";
        return false;
    }

    if (!opt.pattern.empty())
    {
        if (!opt.mode_set)
        {
            opt.mode = qpak::MatchMode::Wildcard;
        }
    }
    else
    {
        opt.mode = qpak::MatchMode::All;
    }

    return true;
}

[[nodiscard]] int cmd_list(Options const& opt)
{
    qpak::PakArchive pak{};
    std::string err{};
    if (!pak.open(opt.pak_path, err))
    {
        std::fprintf(stderr, "qpak: %s\n", err.c_str());
        return 1;
    }

    std::size_t matched = 0;
    for (auto const& e : pak.entries())
    {
        if (!qpak::PakArchive::matches(e.name, opt.pattern, opt.mode))
        {
            continue;
        }
        // stdout = data: path\tsize
        std::printf("%s\t%u\n", e.name.c_str(), e.size);
        ++matched;
    }
    std::fprintf(stderr, "qpak: %zu / %zu entries\n", matched, pak.size());
    return 0;
}

[[nodiscard]] int cmd_extract(Options const& opt)
{
    qpak::PakArchive pak{};
    std::string err{};
    if (!pak.open(opt.pak_path, err))
    {
        std::fprintf(stderr, "qpak: %s\n", err.c_str());
        return 1;
    }

    std::string base = opt.out_dir;
    if (!base.empty() && base.back() != '/')
    {
        base.push_back('/');
    }

    std::size_t matched = 0;
    std::size_t failed = 0;
    for (auto const& e : pak.entries())
    {
        if (!qpak::PakArchive::matches(e.name, opt.pattern, opt.mode))
        {
            continue;
        }
        // Reject path traversal
        if (e.name.find("..") != std::string::npos ||
            (!e.name.empty() && e.name[0] == '/'))
        {
            std::fprintf(stderr, "qpak: skip unsafe path: %s\n", e.name.c_str());
            ++failed;
            continue;
        }

        std::vector<std::uint8_t> bytes{};
        if (!pak.read_entry(e, bytes, err))
        {
            std::fprintf(stderr, "qpak: read %s: %s\n", e.name.c_str(), err.c_str());
            ++failed;
            continue;
        }
        std::string const out_path = base + e.name;
        if (!qpak::write_file(out_path, bytes.data(), bytes.size(), err))
        {
            std::fprintf(stderr, "qpak: write %s: %s\n", out_path.c_str(), err.c_str());
            ++failed;
            continue;
        }
        std::printf("%s\n", out_path.c_str());
        ++matched;
    }
    std::fprintf(stderr, "qpak: extracted %zu file(s)", matched);
    if (failed > 0)
    {
        std::fprintf(stderr, ", %zu failed", failed);
    }
    std::fprintf(stderr, "\n");
    return failed > 0 ? 1 : 0;
}

} // namespace

int main(int argc, char** argv)
{
    Options opt{};
    std::string err{};
    if (!parse_args(argc, argv, opt, err))
    {
        if (!err.empty())
        {
            std::fprintf(stderr, "qpak: %s\n", err.c_str());
        }
        print_usage(stderr);
        return err.empty() ? 2 : 2;
    }

    switch (opt.cmd)
    {
    case Cmd::List:
        return cmd_list(opt);
    case Cmd::Extract:
        return cmd_extract(opt);
    case Cmd::None:
    default:
        print_usage(stderr);
        return 2;
    }
}

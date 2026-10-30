module;

#include <unistd.h>

export module llmgate.org_fetch;

import std;
import llmgate.org_pack;
import llmgate.process;

export namespace llmgate::org {

constexpr std::size_t longest_pack_bytes = 1048576;
constexpr std::size_t commit_hex_sha1 = 40;
constexpr std::size_t commit_hex_sha256 = 64;

enum class source_kind { directory, git };

struct fetched {
    std::string text{};
    std::string commit{};
    std::string source{};
    std::string ref{};
    source_kind kind = source_kind::directory;
};

namespace detail {

constexpr bool ref_character(char const c) {
    constexpr std::string_view marks = "._-/";
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || marks.contains(c);
}

constexpr bool plain_ref(std::string_view const ref) {
    return !ref.empty() && ref.front() != '-' && std::ranges::all_of(ref, ref_character);
}

constexpr bool full_commit(std::string_view const ref) {
    return (ref.size() == commit_hex_sha1 || ref.size() == commit_hex_sha256) && std::ranges::all_of(ref, [](char const c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
}

constexpr bool scp_style(std::string_view const source) {
    auto const at = source.find('@');
    auto const colon = source.find(':');
    auto const slash = source.find('/');
    return at != std::string_view::npos && colon != std::string_view::npos && at < colon && (slash == std::string_view::npos || colon < slash);
}

inline std::optional<std::string> read_limited(std::filesystem::path const& path) {
    std::ifstream in{path};
    if (!in) {
        return std::nullopt;
    }
    std::string text(longest_pack_bytes + 1, '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(in.gcount()));
    return text.size() > longest_pack_bytes ? std::nullopt : std::optional<std::string>{std::move(text)};
}

struct git_runner {
    std::filesystem::path env;
    std::filesystem::path git;

    std::string run(std::vector<std::string> arguments) const {
        std::vector<std::string> full{"GIT_TERMINAL_PROMPT=0", git.string(), "-c", "core.hooksPath=/dev/null", "-c", "advice.detachedHead=false"};
        std::ranges::move(arguments, std::back_inserter(full));
        auto const output = process::run_capturing(env, full);
        return output ? *output : std::string{};
    }
};

inline std::string trimmed_line(std::string text) {
    text.erase(std::ranges::find_if(text, [](char const c) { return c == '\n' || c == '\r'; }), text.end());
    return text;
}

inline std::expected<fetched, std::string> from_directory(std::filesystem::path const& directory, std::string_view const source) {
    auto text = read_limited(directory / pack_file);
    if (!text) {
        return std::unexpected{std::format("{} has no readable {} (or it is larger than {} bytes)", source, pack_file, longest_pack_bytes)};
    }
    return fetched{.text = std::move(*text), .source = std::string{source}};
}

constexpr std::string lowered(std::string_view const text) {
    std::string out{text};
    std::ranges::transform(out, out.begin(), [](char const c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return out;
}

inline std::string tag_commit(std::string const& listing, std::string_view const ref) {
    std::string const plain = std::format("refs/tags/{}", ref);
    std::string const peeled = plain + "^{}";
    std::string found;
    for (auto const line : std::views::split(std::string_view{listing}, '\n')) {
        std::string_view const row{line.begin(), line.end()};
        auto const tab = row.find('\t');
        if (tab == std::string_view::npos) {
            continue;
        }
        if (auto const name = row.substr(tab + 1); name == peeled || (name == plain && found.empty())) {
            found = std::string{row.substr(0, tab)};
        }
    }
    return found;
}

inline std::expected<std::string, std::string> resolve_pin(git_runner const& git, std::string_view const url, std::string_view const ref) {
    if (!plain_ref(ref)) {
        return std::unexpected{std::format("--ref {} is not a plain tag or commit name", ref)};
    }
    if (full_commit(ref)) {
        return lowered(ref);
    }
    auto commit = tag_commit(git.run({"ls-remote", "--tags", "--", std::string{url}, std::format("refs/tags/{}", ref), std::format("refs/tags/{}^{{}}", ref)}), ref);
    if (commit.empty()) {
        return std::unexpected{std::format("{} is not a tag of {} (a branch is not pinned: use a tag or a full commit), or the repository cannot be reached", ref, url)};
    }
    return lowered(commit);
}

inline std::expected<void, std::string> fetch_pinned(git_runner const& git, std::filesystem::path const& target, std::string_view const url, std::string_view const ref,
                                                     std::string const& commit) {
    auto const by_commit = full_commit(ref);
    auto const spec = by_commit ? std::string{ref} : std::format("refs/tags/{}:refs/tags/{}", ref, ref);
    git.run({"init", "--quiet", target.string()});
    git.run({"-C", target.string(), "fetch", "--quiet", "--depth", "1", "--no-tags", "--", std::string{url}, spec});
    auto const reached = lowered(trimmed_line(git.run({"-C", target.string(), "rev-parse", by_commit ? "FETCH_HEAD" : std::format("refs/tags/{}^{{commit}}", ref)})));
    if (reached != commit) {
        return std::unexpected{std::format("{} of {} did not fetch as the commit it was resolved to; nothing was installed", ref, url)};
    }
    git.run({"-C", target.string(), "checkout", "--quiet", commit});
    return {};
}

inline std::expected<fetched, std::string> from_git(std::string_view const url, std::string_view const ref) {
    auto const git = process::find_program("git");
    auto const env = process::find_program("env");
    if (!git || !env) {
        return std::unexpected{std::string{"git is required to install a pack from a URL"}};
    }
    git_runner const runner{.env = *env, .git = *git};
    auto const commit = resolve_pin(runner, url, ref);
    if (!commit) {
        return std::unexpected{commit.error()};
    }
    auto const target = std::filesystem::temp_directory_path() / std::format("llmgate-org-{}-{}", ::getpid(), std::chrono::steady_clock::now().time_since_epoch().count());
    auto pinned = fetch_pinned(runner, target, url, ref, *commit);
    auto result = pinned ? from_directory(target, url) : std::unexpected{pinned.error()};
    if (result) {
        result->commit = *commit;
        result->ref = std::string{ref};
    }
    std::error_code ignored;
    std::filesystem::remove_all(target, ignored);
    return result;
}

}  // namespace detail

constexpr source_kind classify(std::string_view const source) {
    return source.contains("://") || detail::scp_style(source) ? source_kind::git : source_kind::directory;
}

inline std::expected<fetched, std::string> fetch(std::string_view const source, std::string_view const ref, source_kind const kind) {
    if (kind == source_kind::git) {
        if (ref.empty()) {
            return std::unexpected{std::string{"a pack from a git URL needs --ref (a tag or a full commit)"}};
        }
        auto result = detail::from_git(source, ref);
        if (result) {
            result->kind = kind;
        }
        return result;
    }
    std::error_code problem;
    auto const directory = std::filesystem::canonical(std::filesystem::path{process::expand_home(source)}, problem);
    if (problem || !std::filesystem::is_directory(directory, problem)) {
        return std::unexpected{std::format("{} is neither a directory nor a git URL", source)};
    }
    auto result = detail::from_directory(directory, directory.string());
    if (result) {
        result->kind = kind;
    }
    return result;
}

}  // namespace llmgate::org

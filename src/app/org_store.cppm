export module llmgate.org_store;

import std;
import llmgate.json;
import llmgate.org_pack;
import llmgate.process;
import llmgate.sigv4;

export namespace llmgate::org {

constexpr std::string_view directory_variable = "LLMGATE_ORG_DIR";
constexpr std::string_view default_directory = "~/.config/llmgate/org";
constexpr std::string_view installed_name = "pack.jsonc";
constexpr std::string_view source_name = "source.json";
constexpr std::string_view applied_name = "applied.json";
constexpr unsigned private_mode = 0600;

struct source_record {
    std::string source{};
    std::string kind{};
    std::string ref{};
    std::string commit{};
    std::string digest{};
    std::string installed_at{};
};

struct stored {
    loaded pack{};
    source_record from{};
    std::string text{};
};

inline std::filesystem::path directory() {
    auto const* named = std::getenv(directory_variable.data());
    return std::filesystem::path{process::expand_home(named == nullptr || *named == '\0' ? default_directory : std::string_view{named})};
}

inline std::string digest_of(std::string_view const text) {
    return sigv4::hex(sigv4::sha256(text));
}

inline std::string now_stamp() {
    return sigv4::stamp_of(std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}

namespace detail {

inline std::optional<std::string> read_text(std::filesystem::path const& path) {
    std::ifstream in{path};
    if (!in) {
        return std::nullopt;
    }
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

inline std::expected<void, std::string> write_text(std::filesystem::path const& path, std::string_view const text) {
    if (auto const written = process::write_file_atomic(path, text, private_mode, true); !written) {
        return std::unexpected{std::format("cannot write {}: {}", path.string(), written.error().message())};
    }
    return {};
}

}  // namespace detail

inline std::expected<std::optional<stored>, std::string> read_installed() {
    auto const text = detail::read_text(directory() / installed_name);
    if (!text) {
        return std::optional<stored>{};
    }
    auto pack = read_pack(*text);
    if (!pack) {
        return std::unexpected{std::format("the installed organisation pack is invalid: {}", pack.error())};
    }
    stored out{.pack = std::move(*pack), .text = *text};
    if (auto const note = detail::read_text(directory() / source_name)) {
        if (auto record = json::read_text<source_record>(*note)) {
            out.from = std::move(*record);
        }
    }
    return std::optional<stored>{std::move(out)};
}

inline std::expected<void, std::string> write_installed(std::string_view const pack_text, source_record const& from) {
    if (auto written = detail::write_text(directory() / installed_name, pack_text); !written) {
        return written;
    }
    return detail::write_text(directory() / source_name, json::write(json::to_value(from), 2));
}

inline applied read_applied() {
    auto const text = detail::read_text(directory() / applied_name);
    auto record = text ? json::read_text<applied>(*text) : std::expected<applied, json::read_error>{applied{}};
    return record ? std::move(*record) : applied{};
}

inline std::expected<void, std::string> write_applied(applied const& record) {
    return detail::write_text(directory() / applied_name, json::write(json::to_value(record), 2));
}

inline void remove_installed() {
    std::error_code ignored;
    for (auto const name : {installed_name, source_name, applied_name}) {
        std::filesystem::remove(directory() / name, ignored);
    }
}

inline std::expected<std::string, std::string> effective_text(std::string_view const user_text) {
    auto installed = read_installed();
    if (!installed) {
        return std::unexpected{std::move(installed.error())};
    }
    if (!*installed) {
        return std::string{user_text};
    }
    auto user = json::parse(user_text, {.jsonc = true});
    if (!user) {
        return std::string{user_text};
    }
    return json::write(effective_config((*installed)->pack, *user));
}

inline json::value apply_to_settings(json::value settings, bool const persist) {
    auto installed = read_installed();
    if (!installed || !*installed) {
        return settings;
    }
    auto result = apply_policy(std::move(settings), (*installed)->pack.described.claude, read_applied());
    if (persist) {
        (void)write_applied(result.added);
    }
    return std::move(result.settings);
}

inline json::value remove_from_settings(json::value settings) {
    return remove_policy(std::move(settings), read_applied());
}

}  // namespace llmgate::org

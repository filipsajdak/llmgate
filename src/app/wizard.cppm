module;

#include <meta>

export module llmgate.wizard;

import std;
import llmgate.claude;
import llmgate.config_edit;
import llmgate.doc;
import llmgate.gateway;
import llmgate.members;
import llmgate.reflect;

export namespace llmgate::wizard {

struct preset {
    gateway::preset_kind which;
};

enum class kind {
    claude [[= reflect::doc{"Your Claude subscription or API key, used through Claude Code's own login."}]] [[= preset{gateway::preset_kind::anthropic}]],
    copilot [[= reflect::doc{"A GitHub Copilot seat, on github.com or on an enterprise host."}]] [[= preset{gateway::preset_kind::copilot}]],
    chatgpt [[= reflect::doc{"Your ChatGPT plan (Codex models)."}]] [[= preset{gateway::preset_kind::chatgpt}]],
    gateway [[= reflect::doc{"Another gateway or Anthropic-compatible service that takes a key."}]],
    local [[= reflect::doc{"A model served on this machine that speaks the Anthropic Messages API."}]]
};

struct answers {
    kind which = kind::claude;
    std::string name{};
    std::string host{};
    std::string base_url{};
    std::string auth{};
    std::string secret{};
};

constexpr std::string_view default_local_url = "http://127.0.0.1:8000";

constexpr std::size_t kind_count = std::meta::enumerators_of(^^kind).size();

constexpr std::string_view summary_of(kind const which) {
    std::string_view found;
    template for (constexpr auto item : std::define_static_array(std::meta::enumerators_of(^^kind))) {
        if (which == [:item:]) {
            found = std::define_static_string(std::string_view{reflect::annotation_of<reflect::doc>(item).value_or(reflect::doc{""}).text});
        }
    }
    return found;
}

constexpr std::string_view key_of(kind const which) {
    std::string_view found;
    template for (constexpr auto item : std::define_static_array(std::meta::enumerators_of(^^kind))) {
        if (which == [:item:]) {
            found = std::define_static_string(std::meta::identifier_of(item));
        }
    }
    return found;
}

constexpr std::optional<gateway::preset_kind> preset_of(kind const which) {
    std::optional<gateway::preset_kind> found;
    template for (constexpr auto item : std::define_static_array(std::meta::enumerators_of(^^kind))) {
        if (which == [:item:]) {
            constexpr auto note = reflect::annotation_of<preset>(item);
            if constexpr (note) {
                found = note->which;
            }
        }
    }
    return found;
}

constexpr std::string_view default_name(kind const which) {
    return key_of(which);
}

constexpr bool signs_in(kind const which) {
    return which == kind::copilot || which == kind::chatgpt;
}

constexpr bool can_list_models(kind const which) {
    return which != kind::claude;
}

constexpr bool asks_for_url(kind const which) {
    return which == kind::gateway || which == kind::local;
}

constexpr bool takes_a_key(answers const& given) {
    return given.which == kind::gateway && given.auth != reflect::enum_name(gateway::auth_kind::none);
}

constexpr std::size_t title_width = 10;

inline std::string menu() {
    std::string out;
    for (std::size_t index = 0; index < kind_count; ++index) {
        auto const which = static_cast<kind>(index);
        auto const title = key_of(which);
        auto const gap = std::string(title.size() < title_width ? title_width - title.size() : 0, ' ');
        out += std::format("  {}) {}{} {}\n", index + 1, title, gap, summary_of(which));
    }
    return out;
}

inline std::optional<std::size_t> number_in_menu(std::string_view const text) {
    std::size_t number = 0;
    auto const parsed = std::from_chars(text.data(), text.data() + text.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || number < 1 || number > kind_count) {
        return std::nullopt;
    }
    return number;
}

inline std::expected<std::vector<kind>, std::string> choose(std::string_view const line) {
    std::vector<kind> chosen;
    std::string cleaned{line};
    std::ranges::replace(cleaned, ',', ' ');
    for (auto const word : std::views::split(std::string_view{cleaned}, ' ') | std::views::filter([](auto part) { return !std::ranges::empty(part); })) {
        auto const text = std::string_view{word.begin(), word.end()};
        auto const number = number_in_menu(text);
        if (!number) {
            return std::unexpected{std::format("'{}' is not a number from 1 to {}", text, kind_count)};
        }
        if (auto const which = static_cast<kind>(*number - 1); !std::ranges::contains(chosen, which)) {
            chosen.push_back(which);
        }
    }
    if (chosen.empty()) {
        return std::unexpected{std::format("choose at least one number from 1 to {}", kind_count)};
    }
    return chosen;
}

inline std::optional<bool> yes_no(std::string_view const line, bool const fallback) {
    std::string lowered{line};
    std::ranges::transform(lowered, lowered.begin(), [](char const c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    if (lowered.empty()) {
        return fallback;
    }
    if (lowered == "y" || lowered == "yes") {
        return true;
    }
    if (lowered == "n" || lowered == "no") {
        return false;
    }
    return std::nullopt;
}

inline config_edit::request provider_request(answers const& given) {
    auto const which = preset_of(given.which);
    return {.which = config_edit::group::provider,
            .words = {"add", given.name},
            .preset = which ? std::string{reflect::enum_name(*which)} : std::string{},
            .base_url = given.base_url,
            .auth = given.auth,
            .secret = given.secret,
            .ghe_host = given.host};
}

inline std::optional<config_edit::change> credential_edit(kind const which) {
    if (which != kind::claude) {
        return std::nullopt;
    }
    return config_edit::change{.path = {"claude", "credential"}, .value = std::format("\"{}\"", reflect::enum_name(claude::credential_mode::login))};
}

constexpr std::string_view minimal_config = "{\n  \"listen\": \"127.0.0.1:9480\"\n}\n";

}  // namespace llmgate::wizard

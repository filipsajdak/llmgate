export module llmgate.config_edit;

import std;
import llmgate.control;
import llmgate.gateway;
import llmgate.json;
import llmgate.jsonedit;
import llmgate.reflect;

export namespace llmgate::config_edit {

enum class group { routes, models, config, provider };

struct request {
    group which = group::config;
    std::vector<std::string> words{};
    std::string preset{};
    std::string labels{};
    std::string base_url{};
    std::string token_env{};
};

struct change {
    std::vector<std::string> path{};
    std::optional<std::string> value{};
};

struct outcome {
    std::string text{};
    std::string diff{};
};

}  // namespace llmgate::config_edit

namespace llmgate::config_edit::detail {

using planned = std::expected<change, std::string>;

constexpr std::string_view providers_key = "providers";

bool plain_name(std::string_view const name) {
    return !name.empty() && name.size() <= control::longest_label * 2 && std::ranges::all_of(name, control::label_character);
}

std::vector<std::string> split_commas(std::string_view const text) {
    return std::views::split(text, ',') | std::views::transform([](auto part) { return std::string{part.begin(), part.end()}; }) |
           std::views::filter([](std::string const& part) { return !part.empty(); }) | std::ranges::to<std::vector>();
}

json::value strings(std::span<std::string const> const items) {
    json::array out;
    for (auto const& item : items) {
        out.push_back(json::make_string(item));
    }
    return json::value{std::move(out)};
}

planned usage(std::string_view const shape) {
    return std::unexpected{std::format("usage: llmgate {}", shape)};
}

planned routes_plan(std::vector<std::string> const& words) {
    if (words.size() == 3 && words[0] == "set") {
        auto const names = split_commas(words[2]);
        if (names.empty()) {
            return usage("routes set ROLE PROVIDER[,PROVIDER...]");
        }
        return change{.path = {"routes", words[1]}, .value = json::write(strings(names))};
    }
    if (words.size() == 2 && words[0] == "clear") {
        return change{.path = {"routes", words[1]}};
    }
    return usage("routes set ROLE PROVIDER[,PROVIDER...] | routes clear ROLE");
}

planned models_plan(std::vector<std::string> const& words) {
    if (words.size() != 4 || words[0] != "set") {
        return usage("models set PROVIDER NAME UPSTREAM  (NAME is the model name Claude Code sends)");
    }
    return change{.path = {std::string{providers_key}, words[1], "models", words[2]}, .value = json::write(json::make_string(words[3]))};
}

std::optional<std::string> dotted_problem(std::vector<std::string> const& path) {
    if (path.empty() || std::ranges::any_of(path, &std::string::empty)) {
        return "the path must be keys joined by dots, such as fallback.failures";
    }
    return std::nullopt;
}

std::string json_or_string(std::string const& word) {
    auto const parsed = json::parse(word, {.jsonc = false});
    return parsed ? word : json::write(json::make_string(word));
}

planned config_plan(std::vector<std::string> const& words) {
    if (words.size() < 2 || (words[0] == "set") != (words.size() == 3)) {
        return usage("config set PATH VALUE | config unset PATH  (PATH is keys joined by dots; VALUE is JSON, or plain text for a string)");
    }
    std::vector<std::string> path;
    for (auto const part : std::views::split(std::string_view{words[1]}, '.')) {
        path.emplace_back(part.begin(), part.end());
    }
    if (auto const problem = dotted_problem(path)) {
        return std::unexpected{*problem};
    }
    if (words[0] == "set") {
        return change{.path = std::move(path), .value = json_or_string(words[2])};
    }
    if (words[0] == "unset") {
        return change{.path = std::move(path)};
    }
    return usage("config set PATH VALUE | config unset PATH");
}

json::value credentials_of(gateway::preset_kind const kind, request const& given) {
    auto const file = std::format("~/.config/llmgate/{}-{}.json", kind == gateway::preset_kind::copilot ? "copilot" : "chatgpt", given.words[1]);
    json::object out;
    if (kind == gateway::preset_kind::copilot) {
        out.emplace_back("copilot", json::make_object({{"token_file", json::make_string(file)}}));
    } else if (kind == gateway::preset_kind::chatgpt) {
        out.emplace_back("chatgpt", json::make_object({{"token_file", json::make_string(file)}}));
    } else if (!given.token_env.empty()) {
        out.emplace_back("static_token_env", json::make_string(given.token_env));
    } else {
        out.emplace_back("forward_credentials", json::value{true});
    }
    return json::value{std::move(out)};
}

constexpr std::string_view provider_shape = "provider add NAME --preset copilot|anthropic|chatgpt [--labels a,b] [--base-url URL] [--token-env VAR]";

std::optional<gateway::preset_kind> buildable_preset(std::string_view const name) {
    auto const kind = reflect::enum_from_name<gateway::preset_kind>(name);
    return kind && *kind != gateway::preset_kind::none && *kind != gateway::preset_kind::openai_chat ? kind : std::nullopt;
}

json::value provider_value(gateway::preset_kind const kind, request const& given, std::span<std::string const> const labels) {
    auto provider = credentials_of(kind, given);
    auto& members = std::get<json::object>(provider.data);
    members.emplace(members.begin(), "preset", json::make_string(given.preset));
    if (!given.base_url.empty()) {
        members.emplace_back("base_url", json::make_string(given.base_url));
    }
    if (!labels.empty()) {
        members.emplace_back("labels", strings(labels));
    }
    return provider;
}

planned provider_add(request const& given) {
    auto const& name = given.words[1];
    auto const kind = buildable_preset(given.preset);
    if (given.words.size() != 2 || !plain_name(name) || !kind) {
        return usage(provider_shape);
    }
    auto const labels = split_commas(given.labels);
    if (!std::ranges::all_of(labels, control::valid_label)) {
        return std::unexpected{std::format("a label is 1 to {} letters, digits, - or _", control::longest_label)};
    }
    return change{.path = {std::string{providers_key}, name}, .value = json::write(provider_value(*kind, given, labels))};
}

planned provider_plan(request const& given) {
    auto const& words = given.words;
    if (words.size() == 2 && words[0] == "remove") {
        return change{.path = {std::string{providers_key}, words[1]}};
    }
    if (!words.empty() && words[0] == "add" && words.size() >= 2) {
        return provider_add(given);
    }
    return usage("provider add NAME --preset copilot|anthropic|chatgpt | provider remove NAME");
}

std::string name_of_added(change const& what) { return what.path.size() == 2 && what.path.front() == providers_key ? what.path.back() : std::string{}; }

std::string problem_of(std::string_view const text) {
    auto const parsed = json::read_text<gateway::config>(text);
    if (!parsed) {
        return std::format("the edited config would not parse: {} ({})", parsed.error().message, parsed.error().path);
    }
    return gateway::config_problem(*parsed);
}

}  // namespace llmgate::config_edit::detail

export namespace llmgate::config_edit {

std::expected<change, std::string> plan(request const& given) {
    switch (given.which) {
        case group::routes: return detail::routes_plan(given.words);
        case group::models: return detail::models_plan(given.words);
        case group::config: return detail::config_plan(given.words);
        case group::provider: return detail::provider_plan(given);
    }
    return std::unexpected{"unknown edit group"};
}

std::expected<outcome, std::string> apply(std::string_view const text, change const& what) {
    auto const adding = detail::name_of_added(what);
    if (what.value && !adding.empty()) {
        auto const present = jsonedit::set(text, what.path, "null");
        auto const existing = json::read_text<gateway::config>(text);
        if (existing && existing->providers.contains(adding)) {
            return std::unexpected{std::format("provider {} already exists: remove it first or edit it with config set", adding)};
        }
        (void)present;
    }
    auto edited = what.value ? jsonedit::set(text, what.path, *what.value) : jsonedit::remove(text, what.path);
    if (!edited) {
        return std::unexpected{std::move(edited.error())};
    }
    if (auto const problem = detail::problem_of(*edited); !problem.empty()) {
        return std::unexpected{problem};
    }
    auto shown = jsonedit::diff(text, *edited);
    return outcome{.text = std::move(*edited), .diff = std::move(shown)};
}

}  // namespace llmgate::config_edit

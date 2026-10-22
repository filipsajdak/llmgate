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
    std::string auth{};
    std::string secret{};
    std::string ghe_host{};
    std::string offer{};
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

constexpr std::string_view provider_shape =
    "provider add NAME [--preset copilot|anthropic|chatgpt] [--base-url URL] [--auth bearer|none|header:NAME[:SCHEME]] [--secret env:VAR|file:PATH] [--offer a,b] [--labels a,b] [--ghe-host HOST]";

std::optional<gateway::preset_kind> buildable_preset(std::string_view const name) {
    auto const kind = reflect::enum_from_name<gateway::preset_kind>(name);
    return kind && *kind != gateway::preset_kind::none && *kind != gateway::preset_kind::openai_chat ? kind : std::nullopt;
}

std::vector<std::string> split_on(std::string_view const text, char const separator) {
    return std::views::split(text, separator) | std::views::transform([](auto part) { return std::string{part.begin(), part.end()}; }) | std::ranges::to<std::vector>();
}

constexpr std::string_view auth_shape = "--auth is bearer, none or header:NAME[:SCHEME]";

bool auth_words_fit(std::vector<std::string> const& words) {
    auto const& kind = words.front();
    if (kind == "header") {
        return words.size() == 2 || words.size() == 3;
    }
    return (kind == "bearer" || kind == "none") && words.size() == 1;
}

std::expected<std::pair<std::string, std::string>, std::string> secret_source(request const& given) {
    auto const colon = given.secret.find(':');
    auto const kind = given.secret.substr(0, colon);
    auto const where = colon == std::string::npos ? std::string{} : given.secret.substr(colon + 1);
    if (given.auth == "none" || (kind != "env" && kind != "file") || where.empty()) {
        return std::unexpected{"--secret is env:VAR or file:PATH, and not with --auth none"};
    }
    return std::pair{kind, where};
}

std::expected<json::value, std::string> auth_value(request const& given) {
    auto const words = split_on(given.auth, ':');
    if (!auth_words_fit(words)) {
        return std::unexpected{std::string{auth_shape}};
    }
    json::object out;
    out.emplace_back("kind", json::make_string(words.front()));
    if (words.front() == "header") {
        out.emplace_back("header", json::make_string(words[1]));
        if (words.size() == 3) {
            out.emplace_back("scheme", json::make_string(words[2]));
        }
    }
    if (!given.secret.empty()) {
        auto const source = secret_source(given);
        if (!source) {
            return std::unexpected{source.error()};
        }
        out.emplace_back(source->first, json::make_string(source->second));
    }
    return json::value{std::move(out)};
}

json::value login_of(gateway::preset_kind const kind, request const& given) {
    auto const file = std::format("~/.config/llmgate/{}-{}.json", kind == gateway::preset_kind::copilot ? "copilot" : "chatgpt", given.words[1]);
    json::object token{{"token_file", json::make_string(file)}};
    if (kind == gateway::preset_kind::copilot && !given.ghe_host.empty()) {
        token.emplace_back("github_url", json::make_string("https://" + given.ghe_host));
        token.emplace_back("api_url", json::make_string("https://api." + given.ghe_host));
    }
    return json::value{std::move(token)};
}

void add_credentials(json::object& out, std::optional<gateway::preset_kind> const kind, request const& given) {
    if (kind == gateway::preset_kind::copilot) {
        out.emplace_back("copilot", login_of(*kind, given));
    } else if (kind == gateway::preset_kind::chatgpt) {
        out.emplace_back("chatgpt", login_of(*kind, given));
    } else if (!given.token_env.empty()) {
        out.emplace_back("static_token_env", json::make_string(given.token_env));
    } else if (given.auth.empty()) {
        out.emplace_back("forward_credentials", json::value{true});
    }
}

std::expected<json::value, std::string> provider_value(std::optional<gateway::preset_kind> const kind, request const& given, std::span<std::string const> const labels) {
    json::object out;
    if (kind) {
        out.emplace_back("preset", json::make_string(given.preset));
    }
    if (!given.base_url.empty()) {
        out.emplace_back("base_url", json::make_string(given.base_url));
    }
    add_credentials(out, kind, given);
    if (!given.auth.empty()) {
        auto auth = auth_value(given);
        if (!auth) {
            return std::unexpected{std::move(auth.error())};
        }
        out.emplace_back("auth", std::move(*auth));
    }
    if (auto const offers = split_commas(given.offer); !offers.empty()) {
        out.emplace_back("offer", strings(offers));
    }
    if (!labels.empty()) {
        out.emplace_back("labels", strings(labels));
    }
    return json::value{std::move(out)};
}

bool wants_a_provider(request const& given, std::optional<gateway::preset_kind> const kind) {
    auto const& name = given.words[1];
    auto const plain = !kind && given.preset.empty() && !given.base_url.empty() && !given.auth.empty();
    return given.words.size() == 2 && plain_name(name) && (kind || plain);
}

planned provider_add(request const& given) {
    auto const kind = buildable_preset(given.preset);
    if (!wants_a_provider(given, kind)) {
        return usage(provider_shape);
    }
    auto const labels = split_commas(given.labels);
    if (!std::ranges::all_of(labels, control::valid_label)) {
        return std::unexpected{std::format("a label is 1 to {} letters, digits, - or _", control::longest_label)};
    }
    auto value = provider_value(kind, given, labels);
    if (!value) {
        return std::unexpected{std::move(value.error())};
    }
    return change{.path = {std::string{providers_key}, given.words[1]}, .value = json::write(*value)};
}

planned provider_plan(request const& given) {
    auto const& words = given.words;
    if (words.size() == 2 && words[0] == "remove") {
        return change{.path = {std::string{providers_key}, words[1]}};
    }
    if (!words.empty() && words[0] == "add" && words.size() >= 2) {
        return provider_add(given);
    }
    return usage("provider add NAME ... | provider remove NAME (provider add without options shows the shape)");
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

struct alias_line {
    std::string_view name{};
    std::string_view value{};
};

bool plain_alias(std::string_view const name) {
    return !name.empty() && std::ranges::none_of(name, [](char const c) { return std::string_view{" ,\"'"}.contains(c); });
}

std::optional<alias_line> alias_of_line(std::string_view const row) {
    auto const first = row.find_first_not_of(' ');
    auto const colon = row.find(':');
    if (first == std::string_view::npos || row[first] == '#' || colon == std::string_view::npos || colon < first) {
        return std::nullopt;
    }
    alias_line const found{.name = row.substr(first, colon - first), .value = row.substr(colon + 1)};
    return plain_alias(found.name) ? std::optional<alias_line>{found} : std::nullopt;
}

bool starts_a_block(std::string_view const row) { return !row.empty() && row.front() != ' ' && row.front() != '#'; }

bool wanted(alias_line const& alias, std::string_view const filter) { return filter.empty() || alias.value.contains(filter) || alias.name.contains(filter); }

std::vector<std::string> alias_names(std::string_view const yaml, std::string_view const filter) {
    std::vector<std::string> names;
    bool inside = false;
    for (auto const line : std::views::split(yaml, '\n')) {
        std::string_view const row{line.begin(), line.end()};
        if (starts_a_block(row)) {
            inside = row.starts_with("aliases:");
        } else if (auto const alias = inside ? alias_of_line(row) : std::nullopt; alias && wanted(*alias, filter)) {
            names.emplace_back(alias->name);
        }
    }
    return names;
}

std::expected<std::string, std::string> secret_file(std::string_view const name) {
    if (!detail::plain_name(name)) {
        return std::unexpected{"a secret name is letters, digits, - and _"};
    }
    return std::format("~/.config/llmgate/secrets/{}.key", name);
}

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

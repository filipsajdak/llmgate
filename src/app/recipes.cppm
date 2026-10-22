export module llmgate.recipes;

import std;
import llmgate.control;
import llmgate.gateway;
import llmgate.json;
import llmgate.reflect;

export namespace llmgate::recipes {

enum class provider_kind { claude, chatgpt, copilot, other };

constexpr provider_kind kind_of(gateway::provider_config const& provider) {
    if (provider.forward_credentials || provider.preset == gateway::preset_kind::anthropic) {
        return provider_kind::claude;
    }
    if (provider.chatgpt || provider.preset == gateway::preset_kind::chatgpt) {
        return provider_kind::chatgpt;
    }
    if (provider.copilot || provider.preset == gateway::preset_kind::copilot) {
        return provider_kind::copilot;
    }
    return provider_kind::other;
}

struct edit {
    std::vector<std::string> path{};
    std::optional<std::string> value{};
};

struct role_order {
    std::string role{};
    std::vector<provider_kind> kinds{};
};

struct class_target {
    std::string request_class{};
    provider_kind kind = provider_kind::chatgpt;
    std::string via_role{};
};

struct recipe {
    std::string id{};
    std::string title{};
    std::string summary{};
    std::vector<std::string> pros{};
    std::vector<std::string> cons{};
    std::vector<provider_kind> needs{};
    std::string tokens{};
    std::vector<role_order> roles{};
    std::vector<class_target> classes{};
};

struct planned {
    std::vector<std::string> missing{};
    std::vector<edit> edits{};
};

struct info {
    std::string id{};
    std::string title{};
    std::string summary{};
    std::vector<std::string> pros{};
    std::vector<std::string> cons{};
    std::vector<std::string> needs{};
    std::string tokens{};
};

}  // namespace llmgate::recipes

namespace llmgate::recipes::detail {

using kinds = std::vector<provider_kind>;

std::string kind_name(provider_kind const kind) { return std::string{reflect::enum_name(kind)}; }

std::vector<role_order> routed(kinds const& opus, kinds const& sonnet, kinds const& haiku) {
    return {{.role = "opus", .kinds = opus}, {.role = "sonnet", .kinds = sonnet}, {.role = "haiku", .kinds = haiku}};
}

std::vector<recipe> built_in() {
    using enum provider_kind;
    kinds const claude_first{claude, copilot, chatgpt};
    std::vector<class_target> const cheap_background{{.request_class = "compaction", .kind = chatgpt, .via_role = "haiku"}, {.request_class = "auxiliary", .kind = chatgpt, .via_role = "haiku"}};
    return {
        {.id = "quality-first",
         .title = "Quality first, with fallback",
         .summary = "Claude answers every role; Copilot and then ChatGPT only take over when Claude is limited or down.",
         .pros = {"one vendor's behaviour, the best-known results with Claude Code", "the other plans are a safety net"},
         .cons = {"spends the Claude quota fastest", "a fallback answers in another style and starts with a cold cache"},
         .needs = {claude},
         .tokens = "1x",
         .roles = routed(claude_first, claude_first, claude_first)},
        {.id = "spend-evenly",
         .title = "Spend the subscriptions evenly",
         .summary = "Opus stays on Claude, sonnet runs on Copilot, haiku and background work run on ChatGPT, so each plan lasts longer.",
         .pros = {"each quota lasts longer", "the cheap tier carries the bulk of the requests"},
         .cons = {"quality varies by role", "the cache is cold at each switch", "needs three sign-ins"},
         .needs = {claude, chatgpt, copilot},
         .tokens = "1x",
         .roles = routed(claude_first, {copilot, chatgpt, claude}, {chatgpt, copilot, claude}),
         .classes = cheap_background},
        {.id = "second-opinion",
         .title = "A second opinion",
         .summary = "Main roles on Claude, with a different vendor available for review (the reviewer agent arrives with the agent types).",
         .pros = {"a different vendor catches different mistakes (reported: errors correlate within one model family)"},
         .cons = {"a review costs extra tokens and time", "the reviewer agent type is not installed yet"},
         .needs = {claude},
         .tokens = "about 1.3x",
         .roles = routed(claude_first, claude_first, claude_first)},
        {.id = "best-per-job",
         .title = "The best model for each job",
         .summary = "The strongest model plans, a cheaper one executes, haiku and background work use the cheap tier.",
         .pros = {"each job gets the model that fits it", "a strong planner with a cheaper executor has the clearest published gains"},
         .cons = {"the most moving parts", "task-fit claims are reported until your own usage confirms them"},
         .needs = {claude, chatgpt},
         .tokens = "1x to 1.5x",
         .roles = routed(claude_first, {chatgpt, copilot, claude}, {chatgpt, copilot, claude}),
         .classes = cheap_background},
    };
}

std::vector<std::string> providers_of(std::map<std::string, gateway::provider_config> const& all, kinds const& wanted) {
    std::vector<std::string> names;
    for (auto const kind : wanted) {
        for (auto const& [name, item] : all) {
            if (kind_of(item) == kind) {
                names.push_back(name);
            }
        }
    }
    return names;
}

std::string array_text(std::span<std::string const> const items) {
    json::array out;
    for (auto const& item : items) {
        out.push_back(json::make_string(item));
    }
    return json::write(json::value{std::move(out)});
}

std::optional<std::string> target_of(std::map<std::string, gateway::provider_config> const& all, class_target const& wanted) {
    for (auto const& [name, item] : all) {
        if (kind_of(item) != wanted.kind) {
            continue;
        }
        for (auto const& [claude_name, upstream] : item.models) {
            if (claude_name.contains(wanted.via_role)) {
                return control::id_of(upstream, name);
            }
        }
        return name;
    }
    return std::nullopt;
}

std::vector<std::string> missing_kinds(recipe const& wanted, std::map<std::string, gateway::provider_config> const& providers) {
    std::vector<std::string> missing;
    for (auto const kind : std::array{provider_kind::claude, provider_kind::chatgpt, provider_kind::copilot}) {
        if (std::ranges::contains(wanted.needs, kind) && providers_of(providers, {kind}).empty()) {
            missing.push_back(kind_name(kind));
        }
    }
    return missing;
}

void add_role_edits(planned& out, recipe const& wanted, std::map<std::string, gateway::provider_config> const& providers) {
    for (auto const& order : wanted.roles) {
        if (auto const names = providers_of(providers, order.kinds); !names.empty()) {
            out.edits.push_back({.path = {"routes", order.role}, .value = array_text(names)});
        }
    }
}

void add_class_edits(planned& out, recipe const& wanted, std::map<std::string, gateway::provider_config> const& providers) {
    for (auto const& item : wanted.classes) {
        if (auto const target = target_of(providers, item)) {
            out.edits.push_back({.path = {"routes_by_class", item.request_class}, .value = array_text(std::array{*target})});
        }
    }
}

}  // namespace llmgate::recipes::detail

export namespace llmgate::recipes {

std::vector<recipe> const& all() {
    static std::vector<recipe> const found = detail::built_in();
    return found;
}

std::optional<recipe> find(std::string_view const id) {
    auto const& everything = all();
    auto const at = std::ranges::find(everything, id, &recipe::id);
    return at == everything.end() ? std::nullopt : std::optional<recipe>{*at};
}

planned plan(recipe const& wanted, gateway::config const& settings) {
    auto const providers = gateway::effective_providers(settings);
    planned out{.missing = detail::missing_kinds(wanted, providers)};
    if (!out.missing.empty()) {
        return out;
    }
    detail::add_role_edits(out, wanted, providers);
    detail::add_class_edits(out, wanted, providers);
    return out;
}

std::string name_of(provider_kind const kind) { return detail::kind_name(kind); }

std::string list_json() {
    std::vector<info> rows;
    for (auto const& item : all()) {
        rows.push_back({.id = item.id,
                        .title = item.title,
                        .summary = item.summary,
                        .pros = item.pros,
                        .cons = item.cons,
                        .needs = item.needs | std::views::transform(detail::kind_name) | std::ranges::to<std::vector>(),
                        .tokens = item.tokens});
    }
    return json::write(json::to_value(rows));
}

}  // namespace llmgate::recipes

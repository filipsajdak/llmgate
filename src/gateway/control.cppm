module;

export module llmgate.control;

import std;
import llmgate.json;
import llmgate.role;
import llmgate.text;

export namespace llmgate::control {

constexpr std::string_view provider_separator = "@";
constexpr std::string_view discovery_prefix = "claude-";

struct target {
    std::string id{};
    std::string provider{};
    std::string upstream{};
    std::string display_name{};
};

struct offer {
    std::string provider{};
    std::vector<std::string> models{};
};

constexpr bool discoverable(std::string_view const id) {
    return roles::mentions(id, "claude") || roles::mentions(id, "anthropic");
}

constexpr std::string id_of(std::string_view const upstream, std::string_view const provider) {
    std::string out;
    text::append_all(out, discoverable(upstream) ? std::string_view{} : discovery_prefix, upstream, provider_separator, provider);
    return out;
}

constexpr std::vector<target> catalogue(std::span<offer const> const offers) {
    std::vector<target> out;
    for (auto const& one : offers) {
        for (auto const& model : one.models) {
            auto id = id_of(model, one.provider);
            if (std::ranges::none_of(out, [&id](target const& seen) { return seen.id == id; })) {
                std::string label;
                text::append_all(label, model, " (", one.provider, ")");
                out.push_back({.id = std::move(id), .provider = one.provider, .upstream = model, .display_name = std::move(label)});
            }
        }
    }
    return out;
}

struct pin {
    std::string provider{};
    std::string upstream{};
    bool operator==(pin const&) const = default;
};

using pins = std::map<std::string, pin, std::less<>>;

constexpr std::size_t max_sessions = 256;

struct overrides {
    pins global{};
    std::map<std::string, pins, std::less<>> session{};
    std::deque<std::string> order{};
};

enum class via { config, global_remap, session_remap, picked };

struct step {
    std::string provider{};
    std::optional<std::string> upstream{};
};

struct plan {
    std::vector<step> steps{};
    via source = via::config;
};

struct request_shape {
    std::string_view requested{};
    std::string_view role{};
    std::string_view session{};
};

constexpr std::optional<pin> pin_of(pins const& book, std::string_view const role) {
    auto const found = book.find(role);
    return found == book.end() ? std::nullopt : std::optional<pin>{found->second};
}

constexpr std::optional<pin> session_pin(overrides const& book, std::string_view const session, std::string_view const role) {
    auto const mine = session.empty() ? book.session.end() : book.session.find(session);
    return mine == book.session.end() ? std::nullopt : pin_of(mine->second, role);
}

constexpr plan single(pin const& where, via const source) {
    return {.steps = {{.provider = where.provider, .upstream = where.upstream}}, .source = source};
}

constexpr plan in_configured_order(std::span<std::string const> const order) {
    plan out;
    for (auto const& name : order) {
        out.steps.push_back({.provider = name, .upstream = std::nullopt});
    }
    return out;
}

constexpr plan resolve(request_shape const& asked, std::span<target const> const known, overrides const& book, std::span<std::string const> const order) {
    if (auto const picked = std::ranges::find(known, asked.requested, &target::id); picked != known.end()) {
        return single({.provider = picked->provider, .upstream = picked->upstream}, via::picked);
    }
    if (auto const mine = session_pin(book, asked.session, asked.role)) {
        return single(*mine, via::session_remap);
    }
    if (auto const global = pin_of(book.global, asked.role)) {
        return single(*global, via::global_remap);
    }
    return in_configured_order(order);
}

constexpr std::string pin_problem(pin const& wanted, std::span<target const> const known) {
    auto const same_provider = [&wanted](target const& item) { return item.provider == wanted.provider; };
    if (std::ranges::any_of(known, [&](target const& item) { return same_provider(item) && item.upstream == wanted.upstream; })) {
        return {};
    }
    std::string out;
    if (std::ranges::none_of(known, same_provider)) {
        text::append_all(out, "unknown provider ", wanted.provider);
    } else {
        text::append_all(out, wanted.provider, " does not offer the model ", wanted.upstream);
    }
    return out;
}

struct command {
    std::string action{};
    std::string role{};
    std::string provider{};
    std::string upstream{};
    std::string session{};
};

constexpr void remember(overrides& book, std::string const& session) {
    std::erase(book.order, session);
    book.order.push_back(session);
    while (book.session.size() > max_sessions) {
        auto const oldest = book.order.empty() ? book.session.begin()->first : book.order.front();
        book.session.erase(oldest);
        std::erase(book.order, oldest);
    }
}

constexpr std::string use(overrides& book, command const& what, std::span<target const> const known) {
    if (!roles::from_name(what.role)) {
        return std::format("{} is not a role ({})", what.role, roles::list_text());
    }
    pin const wanted{.provider = what.provider, .upstream = what.upstream};
    if (auto problem = pin_problem(wanted, known); !problem.empty()) {
        return problem;
    }
    if (what.session.empty()) {
        book.global[what.role] = wanted;
    } else {
        book.session[what.session][what.role] = wanted;
        remember(book, what.session);
    }
    return {};
}

constexpr std::string clear(overrides& book, command const& what) {
    auto const scope = [&](pins& mine) {
        if (what.role.empty()) {
            mine.clear();
        } else {
            mine.erase(what.role);
        }
    };
    if (what.session.empty()) {
        scope(book.global);
    } else if (auto const found = book.session.find(what.session); found != book.session.end()) {
        scope(found->second);
        if (found->second.empty()) {
            book.session.erase(found);
            std::erase(book.order, what.session);
        }
    }
    return {};
}

constexpr std::string apply(overrides& book, std::string_view const text, std::span<target const> const known) {
    auto const what = json::read_text<command>(text);
    if (!what) {
        return std::format("the command is not valid JSON of the expected shape: {}", what.error().message);
    }
    if (what->action == "use") {
        return use(book, *what, known);
    }
    if (what->action == "clear") {
        return clear(book, *what);
    }
    return std::format("unknown action {}", what->action);
}

static_assert(id_of("claude-sonnet-5.5", "copilot") == "claude-sonnet-5.5@copilot");
static_assert(id_of("gpt-5-codex", "chatgpt") == "claude-gpt-5-codex@chatgpt");
static_assert(discoverable("Anthropic-X") && !discoverable("gpt-5"));

}  // namespace llmgate::control

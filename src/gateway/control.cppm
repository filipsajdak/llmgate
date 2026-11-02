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
    std::vector<std::string> labels{};
};

struct offer {
    std::string provider{};
    std::vector<std::string> models{};
    std::vector<std::string> labels{};
    std::map<std::string, std::vector<std::string>> model_labels{};
};

constexpr std::size_t longest_label = 24;
constexpr std::size_t most_labels = 8;

constexpr bool within(char const c, char const low, char const high) { return c >= low && c <= high; }

constexpr bool label_character(char const c) { return within(c, 'a', 'z') || within(c, 'A', 'Z') || within(c, '0', '9') || std::string_view{"-_"}.contains(c); }

constexpr bool valid_label(std::string_view const label) { return !label.empty() && label.size() <= longest_label && std::ranges::all_of(label, label_character); }

constexpr std::vector<std::string> merged_labels(std::span<std::string const> const first, std::span<std::string const> const second) {
    std::vector<std::string> out;
    for (auto const& label : std::views::join(std::array{first, second})) {
        if (std::ranges::find(out, label) == out.end()) {
            out.push_back(label);
        }
    }
    return out;
}

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
                auto const own = one.model_labels.find(model);
                auto labels = merged_labels(one.labels, own == one.model_labels.end() ? std::span<std::string const>{} : std::span<std::string const>{own->second});
                std::string label;
                text::append_all(label, model, " (", one.provider, ")");
                if (!labels.empty()) {
                    text::append_all(label, " [", labels.front());
                    for (auto const& more : labels | std::views::drop(1)) {
                        text::append_all(label, ", ", more);
                    }
                    text::append_all(label, "]");
                }
                out.push_back({.id = std::move(id), .provider = one.provider, .upstream = model, .display_name = std::move(label), .labels = std::move(labels)});
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

constexpr std::string_view reload_action = "reload";
constexpr std::string_view stop_action = "stop";

struct command {
    std::string action{};
    std::string role{};
    std::string provider{};
    std::string upstream{};
    std::string target{};
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
    pin wanted{.provider = what.provider, .upstream = what.upstream};
    if (!what.target.empty()) {
        auto const named = std::ranges::find(known, what.target, &target::id);
        if (named == known.end()) {
            return std::format("{} is not a listed target", what.target);
        }
        wanted = {.provider = named->provider, .upstream = named->upstream};
    }
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

struct served {
    std::string provider{};
    std::string upstream{};
    std::string source{};
    bool fallback = false;
    bool switched = false;
};

struct registry {
    std::map<std::string, served, std::less<>> items{};
    std::deque<std::string> order{};
};

constexpr void record(registry& book, std::string const& session, served now) {
    if (session.empty()) {
        return;
    }
    if (auto const before = book.items.find(session); before != book.items.end()) {
        now.switched = before->second.provider != now.provider || before->second.upstream != now.upstream;
    }
    book.items[session] = std::move(now);
    std::erase(book.order, session);
    book.order.push_back(session);
    while (book.items.size() > max_sessions) {
        book.items.erase(book.order.front());
        book.order.pop_front();
    }
}

struct saved_pin {
    std::string role{};
    std::string provider{};
    std::string upstream{};
};

struct saved_pins {
    std::vector<saved_pin> pins{};
};

inline std::string save_pins(overrides const& book) {
    saved_pins out;
    for (auto const& [role, where] : book.global) {
        out.pins.push_back({.role = role, .provider = where.provider, .upstream = where.upstream});
    }
    return json::write(json::to_value(out));
}

inline std::expected<overrides, std::string> load_pins(std::string_view const text) {
    overrides out;
    if (text.empty()) {
        return out;
    }
    auto const read = json::read_text<saved_pins>(text);
    if (!read) {
        return std::unexpected{read.error().message};
    }
    for (auto const& item : read->pins) {
        out.global[item.role] = {.provider = item.provider, .upstream = item.upstream};
    }
    return out;
}

static_assert(id_of("claude-sonnet-5.5", "copilot") == "claude-sonnet-5.5@copilot");
static_assert(id_of("gpt-5-codex", "chatgpt") == "claude-gpt-5-codex@chatgpt");
static_assert(discoverable("Anthropic-X") && !discoverable("gpt-5"));

}  // namespace llmgate::control

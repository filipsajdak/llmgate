export module llmgate.selection;

import std;

export namespace llmgate::selection {

constexpr unsigned default_failure_threshold = 3;
constexpr std::int64_t default_cooldown_ms = 30000;
constexpr std::string_view fallback_role = "other";
constexpr auto known_roles = std::to_array<std::string_view>({"opus", "sonnet", "haiku", "other"});

struct breaker_settings {
    unsigned failures = default_failure_threshold;
    std::int64_t cooldown_ms = default_cooldown_ms;
};

enum class circuit { closed, open, half_open };

struct breaker {
    unsigned failures = 0;
    std::int64_t opened_at = 0;
    bool tripped = false;
    bool probing = false;
};

constexpr circuit state_of(breaker const& book, breaker_settings const& settings, std::int64_t const now) noexcept {
    if (!book.tripped) {
        return circuit::closed;
    }
    return now - book.opened_at < settings.cooldown_ms ? circuit::open : circuit::half_open;
}

constexpr bool available(breaker const& book, breaker_settings const& settings, std::int64_t const now) noexcept {
    auto const state = state_of(book, settings, now);
    return state == circuit::closed || (state == circuit::half_open && !book.probing);
}

constexpr bool try_acquire(breaker& book, breaker_settings const& settings, std::int64_t const now) noexcept {
    if (!available(book, settings, now)) {
        return false;
    }
    book.probing = state_of(book, settings, now) == circuit::half_open;
    return true;
}

constexpr void on_success(breaker& book) noexcept {
    book = breaker{};
}

constexpr void on_release(breaker& book) noexcept {
    book.probing = false;
}

constexpr void on_failure(breaker& book, breaker_settings const& settings, std::int64_t const now) noexcept {
    constexpr unsigned most_failures = std::numeric_limits<unsigned>::max();
    book.probing = false;
    book.failures = book.failures == most_failures ? most_failures : book.failures + 1;
    if (book.tripped || book.failures >= std::max(1U, settings.failures)) {
        book.tripped = true;
        book.opened_at = now;
    }
}

constexpr std::string_view circuit_name(circuit const state) noexcept {
    switch (state) {
        case circuit::closed: return "closed";
        case circuit::open: return "open";
        case circuit::half_open: return "half_open";
    }
    return "closed";
}

struct role_route {
    std::string role{};
    std::vector<std::string> providers{};
};

constexpr bool contains_name(std::span<std::string const> const names, std::string_view const wanted) {
    return std::ranges::any_of(names, [wanted](std::string const& item) { return item == wanted; });
}

constexpr bool has_duplicate(std::span<std::string const> const names) {
    for (std::size_t at = 0; at < names.size(); ++at) {
        if (contains_name(names.subspan(at + 1), names[at])) {
            return true;
        }
    }
    return false;
}

constexpr std::string list_problem(role_route const& item, std::span<std::string const> const declared) {
    if (!std::ranges::contains(known_roles, std::string_view{item.role})) {
        return "routes." + item.role + " is not a role (opus, sonnet, haiku or other)";
    }
    if (item.providers.empty()) {
        return "routes." + item.role + " is empty";
    }
    if (has_duplicate(item.providers)) {
        return "routes." + item.role + " names a provider twice";
    }
    for (auto const& name : item.providers) {
        if (!contains_name(declared, name)) {
            return "routes." + item.role + " names the unknown provider " + name;
        }
    }
    return {};
}

constexpr std::string route_problem(std::span<role_route const> const routes, std::span<std::string const> const declared) {
    for (auto const& item : routes) {
        if (auto problem = list_problem(item, declared); !problem.empty()) {
            return problem;
        }
    }
    return {};
}

constexpr std::vector<std::string> order_for(std::span<role_route const> const routes, std::string_view const role,
                                             std::span<std::string const> const declared) {
    for (auto const wanted : {role, fallback_role}) {
        auto const found = std::ranges::find_if(routes, [wanted](role_route const& item) { return item.role == wanted; });
        if (found != routes.end()) {
            return found->providers;
        }
    }
    return std::vector<std::string>{declared.begin(), declared.end()};
}

constexpr breaker_settings quick{.failures = 2, .cooldown_ms = 10};

constexpr bool opens_after_the_threshold_and_probes_once() {
    breaker book;
    on_failure(book, quick, 0);
    auto const still_closed = available(book, quick, 0);
    on_failure(book, quick, 0);
    auto const skipped = !try_acquire(book, quick, quick.cooldown_ms - 1);
    auto const first_probe = try_acquire(book, quick, quick.cooldown_ms);
    auto const second_probe = try_acquire(book, quick, quick.cooldown_ms);
    on_success(book);
    return still_closed && skipped && first_probe && !second_probe && state_of(book, quick, quick.cooldown_ms) == circuit::closed;
}

constexpr bool a_failed_probe_reopens_for_a_full_cooldown() {
    breaker book;
    on_failure(book, quick, 0);
    on_failure(book, quick, 0);
    (void)try_acquire(book, quick, quick.cooldown_ms);
    on_failure(book, quick, quick.cooldown_ms);
    return state_of(book, quick, 2 * quick.cooldown_ms - 1) == circuit::open && state_of(book, quick, 2 * quick.cooldown_ms) == circuit::half_open;
}

constexpr bool routes_fall_back_to_other_then_to_every_provider() {
    std::vector<std::string> const declared{"a", "b"};
    std::vector<role_route> const routes{{.role = "opus", .providers = {"b", "a"}}, {.role = "other", .providers = {"a"}}};
    return order_for(routes, "opus", declared) == std::vector<std::string>{"b", "a"} && order_for(routes, "haiku", declared) == std::vector<std::string>{"a"} &&
           order_for({}, "haiku", declared) == declared && route_problem(routes, declared).empty();
}

static_assert(opens_after_the_threshold_and_probes_once());
static_assert(a_failed_probe_reopens_for_a_full_cooldown());
static_assert(routes_fall_back_to_other_then_to_every_provider());
static_assert(!route_problem(std::vector<role_route>{{.role = "gpt", .providers = {"a"}}}, std::vector<std::string>{"a"}).empty());
static_assert(!route_problem(std::vector<role_route>{{.role = "opus", .providers = {"z"}}}, std::vector<std::string>{"a"}).empty());
static_assert(!route_problem(std::vector<role_route>{{.role = "opus", .providers = {"a", "a"}}}, std::vector<std::string>{"a"}).empty());

}  // namespace llmgate::selection

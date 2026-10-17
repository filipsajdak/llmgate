#include <boost/ut.hpp>

import llmgate.selection;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate::selection;

constexpr breaker_settings limits{.failures = 2, .cooldown_ms = 100};
constexpr std::int64_t start = 1000;
constexpr std::int64_t before_cooldown_ends = start + limits.cooldown_ms - 1;
constexpr std::int64_t after_cooldown = start + limits.cooldown_ms;

breaker tripped_at(std::int64_t const when) {
    breaker book;
    for (unsigned count = 0; count < limits.failures; ++count) {
        on_failure(book, limits, when);
    }
    return book;
}

const suite selection_suite = [] {
    "capability_failures_below_the_threshold_leave_the_breaker_closed"_test = [] {
        breaker book;
        on_failure(book, limits, start);

        expect(state_of(book, limits, start) == circuit::closed);
        expect(try_acquire(book, limits, start));
    };

    "capability_the_threshold_opens_the_breaker_until_the_cooldown_ends"_test = [] {
        auto book = tripped_at(start);

        expect(state_of(book, limits, start) == circuit::open);
        expect(!try_acquire(book, limits, before_cooldown_ends));
        expect(state_of(book, limits, after_cooldown) == circuit::half_open);
    };

    "capability_after_the_cooldown_exactly_one_probe_is_let_through"_test = [] {
        auto book = tripped_at(start);

        expect(try_acquire(book, limits, after_cooldown));
        expect(!try_acquire(book, limits, after_cooldown));
        expect(!available(book, limits, after_cooldown));
    };

    "capability_a_successful_probe_closes_the_breaker_and_forgets_the_failures"_test = [] {
        auto book = tripped_at(start);
        (void)try_acquire(book, limits, after_cooldown);

        on_success(book);

        expect(state_of(book, limits, after_cooldown) == circuit::closed);
        on_failure(book, limits, after_cooldown);
        expect(state_of(book, limits, after_cooldown) == circuit::closed);
    };

    "capability_a_failed_probe_reopens_the_breaker_for_a_full_cooldown"_test = [] {
        auto book = tripped_at(start);
        (void)try_acquire(book, limits, after_cooldown);

        on_failure(book, limits, after_cooldown);

        expect(state_of(book, limits, after_cooldown + limits.cooldown_ms - 1) == circuit::open);
        expect(state_of(book, limits, after_cooldown + limits.cooldown_ms) == circuit::half_open);
    };

    "limit_a_neutral_outcome_frees_the_probe_without_closing_the_breaker"_test = [] {
        auto book = tripped_at(start);
        (void)try_acquire(book, limits, after_cooldown);

        on_release(book);

        expect(try_acquire(book, limits, after_cooldown));
        expect(book.tripped);
    };

    "limit_a_threshold_of_zero_behaves_as_one"_test = [] {
        breaker book;

        on_failure(book, {.failures = 0, .cooldown_ms = limits.cooldown_ms}, start);

        expect(book.tripped);
    };

    "limit_an_enormous_failure_count_saturates"_test = [] {
        breaker book{.failures = std::numeric_limits<unsigned>::max()};

        on_failure(book, limits, start);

        expect(book.failures == std::numeric_limits<unsigned>::max());
    };

    "capability_a_role_uses_its_own_list_then_other_then_every_provider"_test = [] {
        std::vector<std::string> const declared{"a", "b", "c"};
        std::vector<role_route> const routes{{.role = "opus", .providers = {"c", "a"}}, {.role = "other", .providers = {"b"}}};

        expect(order_for(routes, "opus", declared) == std::vector<std::string>{"c", "a"});
        expect(order_for(routes, "haiku", declared) == std::vector<std::string>{"b"});
        expect(order_for({}, "opus", declared) == declared);
    };

    "limit_a_route_table_is_checked_for_roles_names_and_duplicates"_test = [] {
        std::vector<std::string> const declared{"a", "b"};

        expect(route_problem(std::vector<role_route>{{.role = "opus", .providers = {"a", "b"}}}, declared).empty());
        expect(route_problem(std::vector<role_route>{{.role = "gpt", .providers = {"a"}}}, declared).contains("not a role"));
        expect(route_problem(std::vector<role_route>{{.role = "opus", .providers = {}}}, declared).contains("empty"));
        expect(route_problem(std::vector<role_route>{{.role = "opus", .providers = {"a", "a"}}}, declared).contains("twice"));
        expect(route_problem(std::vector<role_route>{{.role = "opus", .providers = {"z"}}}, declared).contains("unknown provider z"));
    };

    "capability_circuit_states_have_names"_test = [] {
        expect(circuit_name(circuit::closed) == "closed" && circuit_name(circuit::open) == "open" && circuit_name(circuit::half_open) == "half_open");
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}

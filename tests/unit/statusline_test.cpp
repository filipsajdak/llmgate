#include <boost/ut.hpp>

import llmgate.statusline;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate::statusline;

const suite statusline_suite = [] {
    "capability_the_bar_fills_in_ten_steps_and_clamps"_test = [] {
        expect(bar(0) == "----------");
        expect(bar(30) == "###-------");
        expect(bar(99) == "#########-");
        expect(bar(100) == "##########");
        expect(bar(5000) == "##########");
        expect(bar(-3) == "----------");
    };

    "capability_the_branch_comes_from_a_ref_or_a_detached_commit"_test = [] {
        expect(branch_from_head("ref: refs/heads/main\n") == "main");
        expect(branch_from_head("ref: refs/heads/feat/s4\r\n") == "feat/s4");
        expect(branch_from_head("abcdef0123456789\n") == "abcdef0");
        expect(branch_from_head("").empty());
    };

    "capability_both_lines_show_model_directory_branch_context_and_gateway"_test = [] {
        auto const text = render({.model = "Sonnet", .directory = "/home/me/work/app", .branch = "feat", .context_percent = 85,
                                  .gateway_up = true, .requests = 12, .repairs = 0});

        expect(text == "[llmgate] Sonnet | app | feat\nctx [########--] 85% high | gateway ok | 12 req\n");
    };

    "limit_a_gateway_that_is_down_is_called_out_and_has_no_counters"_test = [] {
        auto const text = render({.model = "Haiku", .directory = "/x", .gateway_up = false, .requests = 9});

        expect(text == "[llmgate] Haiku | x\nctx [----------] 0% | gateway DOWN\n");
    };

    "capability_a_third_line_shows_todays_tokens_with_or_without_a_budget"_test = [] {
        auto const plain = render({.model = "O", .directory = "/x", .gateway_up = true, .usage_known = true, .today_tokens = 1500});
        auto const budgeted = render({.model = "O", .directory = "/x", .gateway_up = true, .usage_known = true, .today_tokens = 410000,
                                      .budget_limit = 1000000, .budget_percent = 41, .budget_window = "daily", .budget_state = "ok"});

        expect(plain.ends_with("\nusage today 1k tokens\n"));
        expect(budgeted.ends_with("\nusage today 410k/1.0M daily [####------] 41%\n"));
    };

    "limit_warning_and_exhausted_budgets_are_called_out_and_no_usage_means_no_third_line"_test = [] {
        auto const high = usage_line({.today_tokens = 900, .budget_limit = 1000, .budget_percent = 90, .budget_window = "monthly", .budget_state = "warning"});
        auto const full = usage_line({.today_tokens = 1100, .budget_limit = 1000, .budget_percent = 110, .budget_window = "daily", .budget_state = "exhausted"});

        expect(high.ends_with("90% high"));
        expect(full.ends_with("110% EXHAUSTED"));
        expect(std::ranges::count(render({.model = "O"}), '\n') == 2);
    };

    "limit_out_of_range_percentages_print_as_zero_or_the_cap_never_as_a_wrapped_number"_test = [] {
        expect(render({.context_percent = -5}).contains("ctx [----------] 0%"));
        expect(usage_line({.budget_limit = 1000, .budget_percent = -5, .budget_window = "daily", .budget_state = "ok"}).ends_with("[----------] 0%"));
        expect(usage_line({.budget_limit = 1000, .budget_percent = 0, .budget_window = "daily", .budget_state = "ok"}).ends_with("[----------] 0%"));
        expect(usage_line({.budget_limit = 1000, .budget_percent = 1500, .budget_window = "daily", .budget_state = "ok"}).ends_with("[##########] 999%"));
    };

    "limit_an_empty_budget_state_adds_no_suffix_and_empty_usage_reads_zero_tokens"_test = [] {
        expect(usage_line({.budget_limit = 1000, .budget_percent = 10, .budget_window = "daily"}).ends_with("] 10%"));
        expect(render({.usage_known = true}).ends_with("usage today 0 tokens\n"));
    };

    "limit_the_warning_threshold_is_eighty_percent_inclusive"_test = [] {
        expect(level_of(79) == "ok");
        expect(level_of(0) == "ok");
        expect(level_of(80) == "high");
        expect(level_of(99) == "high");
        expect(level_of(100) == "full");
        expect(render({.context_percent = 80}).contains("80% high"));
        expect(!render({.context_percent = 79}).contains("high"));
    };

    "limit_repeated_trailing_slashes_and_carriage_returns_are_all_removed"_test = [] {
        expect(base_name("/a/proj//") == "proj");
        expect(base_name("//") == "/");
        expect(branch_from_head("ref: refs/heads/x\r\r\n") == "x");
        expect(branch_from_head("abcdef\n").empty());
        expect(branch_from_head("abcdefg\n") == "abcdefg");
    };

    "limit_a_missing_model_is_shown_as_unknown_instead_of_an_empty_slot"_test = [] {
        expect(render({}).starts_with("[llmgate] model? | "));
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}

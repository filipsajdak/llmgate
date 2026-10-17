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

    "limit_a_missing_model_is_shown_as_unknown_instead_of_an_empty_slot"_test = [] {
        expect(render({}).starts_with("[llmgate] model? | "));
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}

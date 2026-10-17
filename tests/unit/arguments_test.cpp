#include <boost/ut.hpp>

import llmgate.arguments;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate::arguments;

std::vector<std::string> words(std::initializer_list<std::string_view> const list) {
    return std::vector<std::string>(list.begin(), list.end());
}

const suite arguments_suite = [] {
    "capability_no_arguments_means_serve_with_the_default_config"_test = [] {
        auto const parsed = parse(words({}));

        expect(parsed->which == command::serve);
        expect(parsed->config_path == "~/.config/llmgate/config.jsonc");
    };

    "capability_everything_after_claude_goes_to_claude_untouched"_test = [] {
        auto const parsed = parse(words({"--config", "c.jsonc", "claude", "--config", "inner", "--version", "-p", "hi"}));

        expect(parsed->which == command::claude);
        expect(parsed->config_path == "c.jsonc");
        expect(parsed->passthrough == words({"--config", "inner", "--version", "-p", "hi"}));
        expect(!parsed->show_version);
    };

    "capability_setup_and_env_flags_are_read"_test = [] {
        auto const setup = parse(words({"setup", "--service", "--dry-run", "--vscode", "/tmp/s.json"}));
        auto const env = parse(words({"env", "--json"}));
        auto const snippet = parse(words({"env", "--vscode"}));

        expect(setup->service && setup->dry_run && setup->as_vscode && setup->vscode_file == "/tmp/s.json");
        expect(env->as_json);
        expect(snippet->as_vscode && snippet->vscode_file.empty());
    };

    "limit_unknown_words_a_second_command_and_a_dangling_config_are_refused"_test = [] {
        expect(parse(words({"bogus"})).error() == "unknown argument: bogus");
        expect(!parse(words({"setup", "serve"})).has_value());
        expect(!parse(words({"--config"})).has_value());
    };

    "capability_usage_takes_a_day_count_and_defaults_to_a_week"_test = [] {
        expect(parse(words({"usage"}))->days == 7_u);
        expect(parse(words({"usage", "--days", "30", "--json"}))->days == 30_u);
        expect(parse(words({"usage", "--days", "30", "--json"}))->as_json);
    };

    "limit_a_bad_day_count_is_refused"_test = [] {
        for (auto const bad : {"0", "-3", "abc", "7x", "3661", ""}) {
            expect(!parse(words({"usage", "--days", bad})).has_value()) << bad;
        }
        expect(!parse(words({"usage", "--days"})).has_value());
    };

    "capability_the_listen_address_splits_into_host_and_port"_test = [] {
        auto const address = split_listen("127.0.0.1:9480");

        expect(address->host == "127.0.0.1");
        expect(address->port == 9480);
        expect(base_url(*address) == "http://127.0.0.1:9480/anthropic");
    };

    "limit_malformed_listen_addresses_are_refused"_test = [] {
        for (std::string_view const text : {"", "9480", ":9480", "host:", "host:port", "h:70000", "h:12x"}) {
            expect(!split_listen(text).has_value()) << text;
        }
    };

    "capability_claudes_status_line_input_becomes_a_snapshot"_test = [] {
        auto const now = statusline_input(R"({"model":{"display_name":"Opus"},"workspace":{"current_dir":"/w/p"},"context_window":{"used_percentage":41.9}})");

        expect(now.model == "Opus");
        expect(now.directory == "/w/p");
        expect(now.context_percent == 41);
    };

    "limit_input_of_the_wrong_shape_gives_an_empty_snapshot_without_failing"_test = [] {
        expect(statusline_input("not json").model.empty());
        expect(statusline_input(R"({"model":7,"context_window":"x"})").context_percent == 0);
        expect(statusline_input(R"({"cwd":"/c"})").directory == "/c");
    };

    "capability_gateway_counters_are_summed_from_the_stats_body"_test = [] {
        auto const [requests, repairs] = counters_from(R"({"requests":12,"repairs":{"a":2,"b":1,"c":0}})");

        expect(requests == 12_u);
        expect(repairs == 3_u);
        expect(counters_from("garbage") == std::pair<std::uint64_t, std::uint64_t>{0, 0});
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}

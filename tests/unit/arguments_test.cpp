#include <meta>

#include <boost/ut.hpp>

import llmgate.arguments;
import llmgate.members;
import llmgate.reflect;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate::arguments;

std::vector<std::string> words(std::initializer_list<std::string_view> const list) {
    return std::vector<std::string>(list.begin(), list.end());
}

const suite arguments_suite = [] {
    "capability_doctor_and_quota_are_commands_and_offline_is_a_flag"_test = [] {
        auto const checked = parse(words({"doctor", "--offline", "--json"}));
        auto const quota = parse(words({"quota", "--json"}));

        expect(checked->which == command::doctor && checked->offline && checked->as_json);
        expect(quota->which == command::quota && !quota->offline && quota->as_json);
        expect(!parse(words({"quota"}))->offline);
    };

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

        expect(setup->service && setup->dry_run && setup->vscode == "/tmp/s.json");
        expect(env->as_json);
        expect(snippet->vscode == "");
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

    "limit_flag_errors_keep_their_exact_texts"_test = [] {
        expect(parse(words({"usage", "--days", "0"})).error() == "--days needs a number from 1 to 3660");
        expect(parse(words({"usage", "--days", "3661"})).error() == "--days needs a number from 1 to 3660");
        expect(parse(words({"usage", "--days"})).error() == "--days needs a number from 1 to 3660");
        expect(parse(words({"--config"})).error() == "unknown argument: --config");
        expect(parse(words({"serve", "serve"})).error() == "unknown argument: serve");
    };

    "capability_the_command_words_come_from_the_enumerators"_test = [] {
        for (auto const word : {"serve", "login", "claude", "env", "setup", "statusline", "status", "stop", "usage", "doctor", "quota"}) {
            expect(command_named(word).has_value()) << word;
        }
        expect(!command_named("bogus").has_value());
    };

    "capability_a_vscode_flag_before_a_command_leaves_the_command_alone"_test = [] {
        auto const parsed = parse(words({"--vscode", "env"}));

        expect(parsed->vscode == "" && parsed->which == command::env);
        expect(parse(words({"--check-config", "--version"}))->check_config);
    };

    "capability_vscode_is_absent_unless_asked_and_does_not_swallow_a_command"_test = [] {
        expect(!parse(words({"env"}))->vscode.has_value());
        expect(parse(words({"setup", "--vscode", "serve"})).error() == "unknown argument: serve");
        expect(parse(words({"env", "--vscode", "--json"}))->vscode == "" && parse(words({"env", "--vscode", "--json"}))->as_json);
    };

    "capability_the_usage_text_names_every_command_and_every_flag"_test = [] {
        for (auto const which : llmgate::reflect::enum_values<command>) {
            expect(usage_text.contains(llmgate::reflect::enum_name(which))) << llmgate::reflect::enum_name(which);
        }
        template for (constexpr auto member : std::define_static_array(std::meta::nonstatic_data_members_of(^^llmgate::arguments::options, std::meta::access_context::current()))) {
            constexpr auto marker = llmgate::reflect::annotation_of<flag>(member).value_or(flag{});
            if constexpr (marker.word[0] != '\0') {
                expect(usage_text.contains(std::string_view{marker.word})) << marker.word;
            }
        }
    };

    "capability_the_usage_text_is_pinned"_test = [] {
        expect(usage_text ==
               "usage: llmgate [--config PATH] [serve | login | claude ARGS... | env [--json] [--vscode [FILE]] | setup [--vscode [FILE]] [--service] [--dry-run] | "
               "statusline | status | stop | usage [--json] [--days N] | doctor [--json] [--offline] | quota [--json]] | --check-config | --version")
            << usage_text;
    };

    "capability_only_statusline_stop_and_doctor_tolerate_a_missing_config"_test = [] {
        for (auto const which : llmgate::reflect::enum_values<command>) {
            auto const expected = which == command::statusline || which == command::stop || which == command::doctor;
            expect(command_marked<tolerant>(which) == expected) << llmgate::reflect::enum_name(which);
        }
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}

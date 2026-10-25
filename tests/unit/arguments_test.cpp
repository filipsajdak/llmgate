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
    "capability_login_takes_json_detach_and_progress_and_the_usage_text_says_so"_test = [] {
        auto const detached = parse(words({"login", "--provider", "work", "--json", "--detach"}));
        auto const progress = parse(words({"login", "--provider", "work", "--json", "--progress"}));

        expect(detached.has_value() && detached->detach && detached->as_json && !detached->progress);
        expect(progress.has_value() && progress->progress && !progress->detach);
        expect(usage_text.contains("login [--json] [--detach] [--progress] [--provider NAME]"));
    };

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

    "capability_use_takes_a_role_and_a_target_and_an_optional_session"_test = [] {
        auto const parsed = parse(words({"use", "opus", "claude-x@copilot", "--session", "s1"}));
        auto const plain = parse(words({"route", "sonnet"}));

        expect(parsed->which == command::use && parsed->passthrough == words({"opus", "claude-x@copilot"}));
        expect(parsed->session == "s1");
        expect(plain->which == command::route && plain->passthrough == words({"sonnet"}) && plain->session.empty());
        expect(parse(words({"targets"}))->which == command::targets);
    };

    "capability_models_lists_a_providers_models_and_takes_the_provider_flag"_test = [] {
        auto const parsed = parse(words({"models", "--provider", "codex", "--json"}));

        expect(parsed.has_value() && parsed->which == command::models && parsed->provider == "codex" && parsed->as_json);
    };

    "capability_the_statusline_input_carries_the_session_id"_test = [] {
        expect(statusline_input(R"({"session_id":"abc-1","model":{"id":"m"}})").session_id == "abc-1");
        expect(statusline_input("{}").session_id.empty());
    };

    "capability_the_served_view_fills_the_provider_part_of_the_snapshot"_test = [] {
        auto const now = served_into({.model = "S"}, R"({"provider":"copilot","upstream":"m","source":"config","fallback":true,"switched":false})");

        expect(now.provider == "copilot" && now.upstream_model == "m" && now.fallback && !now.switched && now.model == "S");
        expect(served_into({.model = "S"}, "{}").provider.empty());
        expect(served_into({.model = "S"}, "garbage").provider.empty());
    };

    "capability_login_can_name_the_provider_to_sign_in_to"_test = [] {
        expect(parse(words({"login", "--provider", "codex"}))->provider == "codex" && parse(words({"login"}))->provider.empty());
        expect(!parse(words({"login", "--provider"})).has_value());
    };

    "capability_profile_is_a_global_flag_before_the_command"_test = [] {
        auto const parsed = parse(words({"--profile", "employer", "claude", "-p", "hi"}));

        expect(parsed->profile == "employer" && parsed->which == command::claude && parsed->passthrough == words({"-p", "hi"}));
        expect(parse(words({"claude"}))->profile.empty());
        expect(!parse(words({"--profile"})).has_value());
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
        expect(statusline_input(R"({"cwd":"/c","workspace":{"current_dir":"/w"}})").directory == "/w");
        expect(statusline_input(R"({"model":{"id":"claude-x"}})").model == "claude-x");
        expect(statusline_input(R"({"model":{"display_name":"Opus","id":"claude-x"}})").model == "Opus");
        expect(statusline_input(R"({"context_window":{"used_percentage":-5}})").context_percent == 0);
        expect(statusline_input(R"({"context_window":{"used_percentage":99999}})").context_percent == 1000);
    };

    "capability_gateway_counters_are_summed_from_the_stats_body"_test = [] {
        auto const [requests, repairs] = counters_from(R"({"requests":12,"repairs":{"a":2,"b":1,"c":0}})");

        expect(requests == 12_u);
        expect(repairs == 3_u);
        expect(counters_from("garbage") == std::pair<std::uint64_t, std::uint64_t>{0, 0});
        expect(counters_from(R"({"requests":"x","repairs":[1,2]})") == std::pair<std::uint64_t, std::uint64_t>{0, 0});
        expect(counters_from(R"({"requests":4,"repairs":{"a":-3,"b":"x","c":2.5,"d":5}})") == std::pair<std::uint64_t, std::uint64_t>{4, 5});
        expect(counters_from(R"({"requests":4})") == std::pair<std::uint64_t, std::uint64_t>{4, 0});
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
               "usage: llmgate [--config PATH] [--profile NAME] [serve | login [--json] [--detach] [--progress] [--provider NAME] | claude ARGS... | env [--json] [--vscode [FILE]] | setup [--vscode [FILE]] [--service] [--dry-run] | "
               "statusline | status | stop | usage [--json] [--days N] | doctor [--json] [--offline] | quota [--json] [--provider NAME] | use [--session ID] ARGS... | targets | route [--session ID] ARGS... | models [--json] [--provider NAME] [--yes] ARGS... | reload | provider [--yes] [--preset NAME] [--labels A,B] [--base-url URL] [--token-env VAR] [--auth KIND] [--secret SRC] [--ghe-host HOST] [--offer A,B] [--offer-from FILE] [--offer-filter TEXT] ARGS... | routes [--yes] ARGS... | config [--yes] ARGS... | secret ARGS... | recipe [--json] [--yes] ARGS... | agents [--yes] [--description TEXT] [--prompt TEXT] ARGS... | rules [--yes] ARGS... | check [--json] [--provider NAME] [--quick]] | --check-config | --version | --print-example | --print-schema | --print-config-table")
            << usage_text;
    };

    "capability_only_statusline_stop_doctor_and_secret_tolerate_a_missing_config"_test = [] {
        for (auto const which : llmgate::reflect::enum_values<command>) {
            auto const expected = which == command::statusline || which == command::stop || which == command::doctor || which == command::secret;
            expect(command_marked<tolerant>(which) == expected) << llmgate::reflect::enum_name(which);
        }
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}

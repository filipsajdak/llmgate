#include <boost/ut.hpp>

import llmgate.claude;
import llmgate.json;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate;

claude::environment_list sample(std::string_view const key = "") {
    return claude::environment({}, "http://127.0.0.1:9480/anthropic", key, "/home/me/.claude-llmgate");
}

std::string value_of(claude::environment_list const& list, std::string_view const name) {
    auto const found = std::ranges::find(list, name, &claude::environment_variable::name);
    return found == list.end() ? "<missing>" : found->value;
}

json::value parse_value(std::string_view const text) {
    return std::move(*json::parse(text));
}

const suite claude_suite = [] {
    "capability_the_environment_points_claude_at_the_gateway_with_the_configured_models"_test = [] {
        auto const list = sample();

        expect(value_of(list, "ANTHROPIC_BASE_URL") == "http://127.0.0.1:9480/anthropic");
        expect(value_of(list, "ANTHROPIC_MODEL") == "opusplan");
        expect(value_of(list, "ANTHROPIC_DEFAULT_SONNET_MODEL") == "claude-sonnet-5-5");
        expect(value_of(list, "ANTHROPIC_DEFAULT_FABLE_MODEL") == "claude-fable-5-1");
        expect(value_of(list, "CLAUDE_CONFIG_DIR") == "/home/me/.claude-llmgate");
        expect(value_of(list, "CLAUDE_CODE_AUTO_MODE_SERVER") == "0");
        expect(value_of(list, "CLAUDE_CODE_ENABLE_GATEWAY_MODEL_DISCOVERY") == "1" && value_of(list, "CLAUDE_CODE_GATEWAY_HINT_HEADERS") == "1");
    };

    "capability_the_context_window_feeds_both_limits"_test = [] {
        auto const list = claude::environment({.context_tokens = 123456}, "u", "", "/p");

        expect(value_of(list, "CLAUDE_CODE_MAX_CONTEXT_TOKENS") == "123456");
        expect(value_of(list, "CLAUDE_CODE_AUTO_COMPACT_WINDOW") == "123456");
    };

    "limit_telemetry_is_off_unless_the_config_turns_it_on"_test = [] {
        expect(value_of(sample(), "DISABLE_TELEMETRY") == "1");
        expect(value_of(claude::environment({.telemetry = true}, "u", "", "/p"), "DISABLE_TELEMETRY") == "<missing>");
    };

    "capability_the_client_key_is_used_when_set_and_a_dummy_otherwise"_test = [] {
        expect(value_of(sample(), "ANTHROPIC_AUTH_TOKEN") == "none");
        expect(value_of(sample("local-secret"), "ANTHROPIC_AUTH_TOKEN") == "local-secret");
    };

    "capability_login_credential_mode_sends_no_gateway_token_so_claude_keeps_its_own_login"_test = [] {
        auto const list = claude::environment({.credential = claude::credential_mode::login}, "u", "local-secret", "/p");

        expect(value_of(list, "ANTHROPIC_AUTH_TOKEN") == "<missing>");
        expect(value_of(list, "ANTHROPIC_BASE_URL") == "u");
    };

    "limit_shell_exports_quote_values_so_they_cannot_inject_commands"_test = [] {
        claude::environment_list list;
        list.emplace_back("A", "x'; rm -rf ~; echo '");

        auto const text = claude::shell_exports(list);

        expect(text == "export A='x'\\''; rm -rf ~; echo '\\'''\n");
    };

    "capability_the_vscode_snippet_lists_every_variable_and_skips_the_login_prompt"_test = [] {
        auto const text = json::write(claude::vscode_snippet(sample()));

        expect(text.contains(R"({"name":"ANTHROPIC_BASE_URL","value":"http://127.0.0.1:9480/anthropic"})"));
        expect(text.ends_with(R"("claudeCode.disableLoginPrompt":true})"));
    };

    "capability_a_new_profile_gets_denials_effort_and_the_status_line"_test = [] {
        auto const merged = claude::merge_settings(json::value{}, "medium", "/bin/llmgate statusline");
        auto const text = json::write(merged);

        expect(text.contains("Read(**/.env)"));
        expect(text.contains("Read(~/.config/llmgate/**)"));
        expect(text.contains(R"("effortLevel":"medium")"));
        expect(text.contains(R"("command":"/bin/llmgate statusline")"));
    };

    "limit_merging_keeps_the_users_own_settings_and_choices"_test = [] {
        auto const existing = parse_value(R"x({"effortLevel":"high","statusLine":{"type":"command","command":"mine"},"permissions":{"deny":["Bash(rm:*)"],"allow":["Read(x)"]},"theme":"dark"})x");

        auto const text = json::write(claude::merge_settings(existing, "medium", "/bin/llmgate statusline"));

        expect(text.contains(R"x("effortLevel":"high")x"));
        expect(text.contains(R"x("command":"mine")x"));
        expect(text.contains(R"x("Bash(rm:*)")x"));
        expect(text.contains(R"x("allow":["Read(x)"])x"));
        expect(text.contains(R"x("theme":"dark")x"));
        expect(text.contains("Read(**/.env)"));
    };

    "capability_merging_twice_changes_nothing"_test = [] {
        auto const once = claude::merge_settings(json::value{}, "medium", "s");
        auto const twice = claude::merge_settings(once, "medium", "s");

        expect(json::write(once) == json::write(twice));
    };

    "limit_settings_of_the_wrong_shape_are_replaced_not_crashed_on"_test = [] {
        auto const text = json::write(claude::merge_settings(parse_value(R"x({"permissions":7})x"), "medium", "s"));

        expect(text.contains("Read(**/.env)"));
        expect(json::write(claude::merge_settings(parse_value("[1,2]"), "medium", "s")).contains("effortLevel"));
    };

    "capability_vscode_merging_replaces_only_our_variables_and_keeps_the_rest"_test = [] {
        auto const existing = parse_value(R"x({"editor.fontSize":14,"claudeCode.environmentVariables":[{"name":"MINE","value":"1"},{"name":"ANTHROPIC_MODEL","value":"old"}]})x");

        auto const text = json::write(claude::merge_vscode(existing, sample()));

        expect(text.contains(R"x({"name":"MINE","value":"1"})x"));
        expect(text.contains(R"x({"name":"ANTHROPIC_MODEL","value":"opusplan"})x"));
        expect(!text.contains(R"x("value":"old")x"));
        expect(text.contains(R"x("editor.fontSize":14)x"));
        expect(text.contains(R"x("claudeCode.disableLoginPrompt":true)x"));
    };

    "capability_vscode_merging_twice_changes_nothing"_test = [] {
        auto const once = claude::merge_vscode(json::value{}, sample());
        auto const twice = claude::merge_vscode(once, sample());

        expect(json::write(once) == json::write(twice));
    };

    "capability_service_files_name_the_binary_the_config_and_the_log"_test = [] {
        auto const plist = claude::launchd_plist("dev.llmgate", "/bin/llmgate", "/c.jsonc", "/l.log");
        auto const unit = claude::systemd_unit("/bin/llmgate", "/c.jsonc");

        expect(plist.contains("<string>dev.llmgate</string>") && plist.contains("<string>/c.jsonc</string>") && plist.contains("<string>/l.log</string>"));
        expect(unit.contains(R"(ExecStart="/bin/llmgate" --config "/c.jsonc" serve)") && unit.contains("Restart=on-failure"));
    };

    "defect_service_files_escape_paths_that_would_break_the_file"_test = [] {
        auto const plist = claude::launchd_plist("dev.llmgate", "/a&b/<llmgate>", "/c.jsonc", "/l.log");
        auto const unit = claude::systemd_unit("/my apps/50%/\"x\"", "/$HOME/c.jsonc");

        expect(plist.contains("<string>/a&amp;b/&lt;llmgate&gt;</string>"));
        expect(!plist.contains("/a&b/"));
        expect(unit.contains(R"(ExecStart="/my apps/50%%/\"x\"" --config "/$$HOME/c.jsonc" serve)"));
    };

    "capability_shell_quoting_keeps_plain_text_whole_and_survives_embedded_quotes"_test = [] {
        expect(claude::shell_quote("") == "''");
        expect(claude::shell_quote("plain") == "'plain'");
        expect(claude::shell_quote("it's") == R"('it'\''s')");
        expect(claude::shell_quote("'") == R"(''\''')");
        expect(claude::shell_quote("a'b'c") == R"('a'\''b'\''c')");
    };

    "capability_a_named_profile_is_its_own_directory_next_to_the_default"_test = [] {
        expect(claude::profile_directory("employer") == "~/.claude-llmgate-employer");
        expect(claude::profile_directory("a_b-2") == "~/.claude-llmgate-a_b-2");
    };

    "limit_a_profile_name_cannot_escape_the_home_directory_or_be_empty"_test = [] {
        for (std::string_view const bad : {"", "../x", "a/b", "a b", ".", "-x", "x;rm"}) {
            expect(claude::profile_directory(bad).empty()) << bad;
        }
    };

    "capability_the_gateway_slash_command_runs_llmgate_with_the_session_id_and_may_only_run_llmgate"_test = [] {
        auto const file = claude::gateway_command_file("/opt/bin/llmgate", "/home/me/.config/llmgate/config.jsonc");

        expect(file.contains("allowed-tools: Bash(/opt/bin/llmgate:*)"));
        expect(file.contains("!`/opt/bin/llmgate --config /home/me/.config/llmgate/config.jsonc $ARGUMENTS --session ${CLAUDE_SESSION_ID}`"));
        expect(file.starts_with("---\n"));
    };

    "limit_a_binary_path_with_a_space_is_quoted_in_the_command_and_the_permission"_test = [] {
        auto const file = claude::gateway_command_file("/my apps/llmgate", "/c.jsonc");

        expect(file.contains("Bash('/my apps/llmgate':*)") && file.contains("!`'/my apps/llmgate' --config /c.jsonc"));
    };

    "capability_the_default_environment_is_exactly_the_documented_variables_in_order"_test = [] {
        constexpr std::string_view expected = R"(export CLAUDE_CONFIG_DIR='/home/me/.claude-llmgate'
export ANTHROPIC_BASE_URL='http://127.0.0.1:9480/anthropic'
export ANTHROPIC_AUTH_TOKEN='none'
export ANTHROPIC_MODEL='opusplan'
export ANTHROPIC_DEFAULT_OPUS_MODEL='claude-opus-5-5'
export ANTHROPIC_DEFAULT_SONNET_MODEL='claude-sonnet-5-5'
export ANTHROPIC_DEFAULT_HAIKU_MODEL='claude-haiku-5-5'
export ANTHROPIC_DEFAULT_FABLE_MODEL='claude-fable-5-1'
export CLAUDE_CODE_DISABLE_UNKNOWN_MODEL_WINDOW_ENFORCEMENT='1'
export CLAUDE_CODE_MAX_CONTEXT_TOKENS='180000'
export CLAUDE_CODE_AUTO_COMPACT_WINDOW='180000'
export CLAUDE_CODE_AUTO_MODE_SERVER='0'
export CLAUDE_CODE_ATTRIBUTION_HEADER='0'
export CLAUDE_CODE_ENABLE_GATEWAY_MODEL_DISCOVERY='1'
export CLAUDE_CODE_GATEWAY_HINT_HEADERS='1'
export CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC='1'
export DISABLE_TELEMETRY='1'
export DISABLE_ERROR_REPORTING='1'
)";

        expect(claude::shell_exports(sample()) == expected);
    };

    "capability_the_profile_denies_exactly_the_secret_paths_it_documents"_test = [] {
        constexpr std::string_view expected =
            R"x("deny":["Read(**/.env)","Read(**/.env.*)","Read(./secrets/**)","Read(./config/credentials.json)","Read(~/.config/llmgate/**)","Read(~/.claude/**)"])x";

        auto const text = json::write(claude::merge_settings(json::value{}, "medium", "llmgate statusline"));

        expect(text.contains(expected)) << text;
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}

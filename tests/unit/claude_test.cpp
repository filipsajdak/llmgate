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
    auto const found = std::ranges::find(list, name, &claude::environment_list::value_type::first);
    return found == list.end() ? "<missing>" : found->second;
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
        expect(value_of(list, "CLAUDE_CONFIG_DIR") == "/home/me/.claude-llmgate");
        expect(value_of(list, "CLAUDE_CODE_AUTO_MODE_SERVER") == "0");
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
        expect(value_of(sample(), "ANTHROPIC_API_KEY") == "none");
        expect(value_of(sample("local-secret"), "ANTHROPIC_API_KEY") == "local-secret");
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
        expect(unit.contains("ExecStart=/bin/llmgate --config /c.jsonc serve") && unit.contains("Restart=on-failure"));
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}

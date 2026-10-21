#include <unistd.h>

#include <boost/ut.hpp>

import llmgate.arguments;
import llmgate.commands;
import llmgate.control;
import llmgate.gateway;
import llmgate.process;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate;

constexpr std::string_view key_variable = "LLMGATE_COMMANDS_TEST_KEY";

int runs = 0;
int answer = 0;

int recording_runner(std::filesystem::path const&, std::vector<std::string> const&) {
    ++runs;
    return answer;
}

struct sandbox {
    std::filesystem::path root = std::filesystem::temp_directory_path() / std::format("llmgate-commands-{}", ::getpid());

    sandbox() {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "home");
        ::setenv("HOME", (root / "home").c_str(), 1);
        ::setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);
        ::setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
        ::unsetenv(key_variable.data());
        runs = 0;
        answer = 0;
    }

    ~sandbox() { std::filesystem::remove_all(root); }

    commands::context context() const {
        commands::context where{.options = {}, .settings = {}, .binary = "/usr/local/bin/llmgate", .run_program = &recording_runner};
        where.settings.listen = "127.0.0.1:19480";
        where.settings.claude.profile_dir = (root / "profile").string();
        where.options.config_path = (root / "llmgate.jsonc").string();
        return where;
    }

    bool unit_written() const {
        return std::filesystem::exists(root / "home" / "Library" / "LaunchAgents" / "dev.llmgate.plist") ||
               std::filesystem::exists(root / "config" / "systemd" / "user" / "llmgate.service");
    }
};

gateway::provider_config with_token_file(std::string file) {
    gateway::provider_config provider{.base_url = "https://example.invalid"};
    provider.copilot = gateway::copilot_config{.token_file = std::move(file)};
    return provider;
}

const suite commands_suite = [] {
    "capability_setup_with_service_writes_the_unit_and_loads_it_through_the_runner"_test = [] {
        sandbox box;
        auto where = box.context();
        where.options.service = true;

        expect(commands::run_setup(where) == 0_i);

        expect(box.unit_written());
        expect(runs >= 1_i);
        expect(std::filesystem::exists(box.root / "profile" / "settings.json"));
        expect(std::filesystem::exists(box.root / "profile" / "commands" / "gateway.md"));
    };

    "limit_setup_reports_a_service_that_cannot_be_loaded_and_exits_non_zero"_test = [] {
        sandbox box;
        auto where = box.context();
        where.options.service = true;
        answer = 1;

        expect(commands::run_setup(where) == 1_i);
    };

    "capability_setup_dry_run_with_service_writes_no_unit_and_runs_nothing"_test = [] {
        sandbox box;
        auto where = box.context();
        where.options.service = true;
        where.options.dry_run = true;

        expect(commands::run_setup(where) == 0_i);

        expect(!box.unit_written());
        expect(runs == 0_i);
        expect(!std::filesystem::exists(box.root / "profile" / "settings.json"));
    };

    "capability_setup_without_service_never_touches_the_runner"_test = [] {
        sandbox box;

        expect(commands::run_setup(box.context()) == 0_i);

        expect(runs == 0_i);
        expect(!box.unit_written());
    };

    "capability_setup_merges_into_a_vscode_settings_file"_test = [] {
        sandbox box;
        auto where = box.context();
        auto const file = box.root / "vscode.json";
        std::filesystem::create_directories(box.root);
        std::ofstream{file} << R"({"editor.fontSize": 14})";
        where.options.vscode = file.string();

        expect(commands::run_setup(where) == 0_i);

        std::ifstream in{file};
        std::string const text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        expect(text.contains("editor.fontSize") && text.contains("ANTHROPIC_BASE_URL"));
    };

    "limit_setup_refuses_a_vscode_file_that_is_not_plain_json_and_skips_the_service"_test = [] {
        sandbox box;
        auto where = box.context();
        auto const file = box.root / "vscode.json";
        std::ofstream{file} << "{ // comment";
        where.options.vscode = file.string();
        where.options.service = true;

        expect(commands::run_setup(where) == 1_i);

        expect(runs == 0_i);
        expect(!box.unit_written());
    };

    "limit_setup_fails_when_the_listen_address_is_malformed"_test = [] {
        sandbox box;
        auto where = box.context();
        where.settings.listen = "nonsense";

        expect(commands::run_setup(where) == 1_i);
    };

    "capability_a_loopback_policy_needs_no_client_key_and_allows_the_listen_host"_test = [] {
        sandbox box;
        gateway::config settings;
        settings.allowed_hosts.emplace_back("gateway.example");

        auto const policy = commands::make_policy(settings, "127.0.0.1", 9480);

        expect(policy.has_value());
        expect(policy->client_key.empty());
        expect(policy->identity_scope == "127.0.0.1:9480");
        expect(policy->identity_key.size() == 64_u);
        expect(std::ranges::contains(policy->allowed_hosts, std::string{"gateway.example"}));
    };

    "limit_a_client_key_variable_that_is_not_set_is_refused"_test = [] {
        sandbox box;
        gateway::config settings;
        settings.client_key_env = key_variable;

        auto const policy = commands::make_policy(settings, "127.0.0.1", 9480);

        expect(!policy.has_value());
        expect(policy.error().contains(key_variable));
    };

    "limit_a_non_loopback_host_without_a_client_key_is_refused"_test = [] {
        sandbox box;

        auto const policy = commands::make_policy(gateway::config{}, "0.0.0.0", 9480);

        expect(!policy.has_value());
        expect(policy.error().contains("not a loopback address"));
    };

    "capability_a_client_key_from_the_environment_is_carried_into_the_policy"_test = [] {
        sandbox box;
        gateway::config settings;
        settings.client_key_env = key_variable;
        ::setenv(key_variable.data(), "secret-key", 1);

        auto const policy = commands::make_policy(settings, "0.0.0.0", 9480);
        ::unsetenv(key_variable.data());

        expect(policy.has_value());
        expect(policy->client_key == "secret-key");
    };

    "limit_a_forwarding_provider_is_refused_with_a_client_key"_test = [] {
        sandbox box;
        gateway::config settings;
        settings.providers["anthropic"] = {.base_url = "https://api.anthropic.com", .forward_credentials = true};
        settings.client_key_env = key_variable;
        ::setenv(key_variable.data(), "secret-key", 1);

        auto const policy = commands::make_policy(settings, "127.0.0.1", 9480);
        ::unsetenv(key_variable.data());

        expect(!policy.has_value());
        expect(policy.error().contains("anthropic") && policy.error().contains("client_key_env"));
    };

    "limit_a_forwarding_provider_is_refused_on_a_non_loopback_listen"_test = [] {
        sandbox box;
        gateway::config settings;
        settings.providers["anthropic"] = {.base_url = "https://api.anthropic.com", .forward_credentials = true};
        settings.client_key_env = key_variable;
        ::setenv(key_variable.data(), "secret-key", 1);

        auto const policy = commands::make_policy(settings, "0.0.0.0", 9480);
        ::unsetenv(key_variable.data());

        expect(!policy.has_value());
        expect(policy.error().contains("anthropic"));
    };

    "capability_a_forwarding_provider_is_allowed_on_loopback_without_a_key"_test = [] {
        sandbox box;
        gateway::config settings;
        settings.providers["anthropic"] = {.base_url = "https://api.anthropic.com", .forward_credentials = true};

        auto const policy = commands::make_policy(settings, "127.0.0.1", 9480);

        expect(policy.has_value());
    };

    "limit_an_identity_key_that_cannot_be_stored_fails_the_policy"_test = [] {
        sandbox box;
        std::filesystem::create_directories(box.root / "state");
        std::ofstream{box.root / "state" / "llmgate"} << "a file where the state directory should be";

        auto const policy = commands::make_policy(gateway::config{}, "127.0.0.1", 9480);

        expect(!policy.has_value());
    };

    "capability_use_words_become_a_control_command_for_a_role_or_a_clear"_test = [] {
        control::overrides pins;
        std::vector<control::target> const known{{.id = "claude-x@copilot", .provider = "copilot", .upstream = "claude-x"}};

        auto const use = commands::control_body({"opus", "claude-x@copilot"}, "s1");
        auto const clear = commands::control_body({"opus", "clear"}, "");
        auto const everything = commands::control_body({"clear"}, "");

        expect(use.has_value() && control::apply(pins, *use, known).empty());
        expect(pins.session.at("s1").at("opus").provider == "copilot" && pins.global.empty());
        pins.global["opus"] = {.provider = "copilot", .upstream = "claude-x"};
        pins.global["haiku"] = {.provider = "copilot", .upstream = "claude-x"};
        expect(clear.has_value() && control::apply(pins, *clear, known).empty());
        expect(!pins.global.contains("opus") && pins.global.contains("haiku"));
        expect(everything.has_value() && control::apply(pins, *everything, known).empty() && pins.global.empty());
    };

    "capability_overrides_are_saved_privately_and_read_back_by_the_next_gateway"_test = [] {
        sandbox box;
        control::overrides pins;
        pins.global["opus"] = {.provider = "copilot", .upstream = "claude-opus-5.5"};

        commands::overrides_sink()(pins);
        auto const loaded = commands::read_overrides();

        expect(loaded.global == pins.global);
        expect((std::filesystem::status(commands::overrides_file()).permissions() & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) == std::filesystem::perms::none);
    };

    "limit_a_missing_or_damaged_overrides_file_reads_as_no_overrides"_test = [] {
        sandbox box;
        expect(commands::read_overrides().global.empty());
        std::filesystem::create_directories(commands::overrides_file().parent_path());
        std::ofstream{commands::overrides_file()} << "{ not json";

        expect(commands::read_overrides().global.empty());
    };

    "capability_the_capture_sink_appends_lines_privately_and_is_off_without_a_path"_test = [] {
        sandbox box;
        auto const path = box.root / "state" / "capture.jsonl";

        auto const sink = commands::capture_sink_bytes(path.string(), 1000);
        sink("one\n");
        sink("two\n");

        expect(!commands::capture_sink_bytes("", 1000));
        std::ifstream in{path};
        std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        expect(text == "one\ntwo\n");
        expect((std::filesystem::status(path).permissions() & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) == std::filesystem::perms::none);
    };

    "limit_the_capture_sink_stops_at_its_limit_and_never_writes_half_a_line"_test = [] {
        sandbox box;
        auto const path = box.root / "capture.jsonl";

        auto const sink = commands::capture_sink_bytes(path.string(), 30);
        for (auto const& line : {std::string(19, 'a') + "\n", std::string(19, 'b') + "\n", std::string(5, 'c') + "\n"}) {
            sink(line);
        }

        expect(std::filesystem::file_size(path) == 20_u);
    };

    "limit_an_existing_capture_file_counts_toward_the_limit"_test = [] {
        sandbox box;
        auto const path = box.root / "capture.jsonl";
        std::filesystem::create_directories(path.parent_path());
        std::ofstream{path} << std::string(25, 'x');

        auto const sink = commands::capture_sink_bytes(path.string(), 30);
        sink(std::string(19, 'a') + "\n");

        expect(std::filesystem::file_size(path) == 25_u);
    };

    "limit_use_refuses_the_wrong_number_of_words"_test = [] {
        expect(!commands::control_body({}, "").has_value());
        expect(!commands::control_body({"opus"}, "").has_value());
        expect(!commands::control_body({"opus", "a", "b"}, "").has_value());
    };

    "capability_the_login_target_is_the_first_provider_with_a_token_file"_test = [] {
        gateway::config settings;
        settings.providers.emplace("a-plain", gateway::provider_config{.base_url = "https://example.invalid"});
        settings.providers.emplace("b-copilot", with_token_file("/tmp/token-b.json"));
        settings.providers.emplace("c-copilot", with_token_file("/tmp/token-c.json"));

        auto const target = commands::copilot_login_target(settings);

        expect(target.has_value() && target->token_file == "/tmp/token-b.json");
    };

    "capability_login_without_a_provider_name_keeps_choosing_copilot_first_then_a_chatgpt_provider"_test = [] {
        gateway::config both;
        both.providers.emplace("a-chatgpt", gateway::provider_config{.base_url = "https://x.invalid", .chatgpt = gateway::chatgpt_config{.token_file = "/tmp/c.json"}});
        both.providers.emplace("b-copilot", with_token_file("/tmp/token-b.json"));
        gateway::config only_chatgpt;
        only_chatgpt.providers.emplace("a-chatgpt", gateway::provider_config{.base_url = "https://x.invalid", .chatgpt = gateway::chatgpt_config{.token_file = "/tmp/c.json"}});

        auto const first = commands::login_choice(both, "");
        auto const second = commands::login_choice(only_chatgpt, "");

        expect(first.has_value() && std::holds_alternative<gateway::copilot_config>(*first));
        expect(second.has_value() && std::holds_alternative<gateway::chatgpt_config>(*second));
    };

    "capability_login_with_a_provider_name_picks_that_providers_sign_in"_test = [] {
        gateway::config settings;
        settings.providers.emplace("a-chatgpt", gateway::provider_config{.base_url = "https://x.invalid", .chatgpt = gateway::chatgpt_config{.token_file = "/tmp/c.json"}});
        settings.providers.emplace("b-copilot", with_token_file("/tmp/token-b.json"));

        auto const chatgpt = commands::login_choice(settings, "a-chatgpt");
        auto const copilot = commands::login_choice(settings, "b-copilot");

        expect(chatgpt.has_value() && std::get<gateway::chatgpt_config>(*chatgpt).token_file == "/tmp/c.json");
        expect(copilot.has_value() && std::get<gateway::copilot_config>(*copilot).token_file == "/tmp/token-b.json");
    };

    "limit_login_names_the_problem_for_an_unknown_provider_one_without_a_sign_in_or_nothing_configured"_test = [] {
        gateway::config settings;
        settings.providers.emplace("plain", gateway::provider_config{.base_url = "https://x.invalid"});

        expect(commands::login_choice(settings, "ghost").error().contains("ghost"));
        expect(commands::login_choice(settings, "plain").error().contains("plain") && commands::login_choice(settings, "plain").error().contains("sign-in"));
        expect(commands::login_choice(settings, "").error().contains("chatgpt.token_file"));
        expect(!commands::login_choice(gateway::config{}, "").has_value());
    };

    "limit_no_token_file_means_no_login_target"_test = [] {
        gateway::config settings;
        settings.providers.emplace("a-plain", gateway::provider_config{.base_url = "https://example.invalid"});
        settings.providers.emplace("b-empty", with_token_file(""));

        expect(!commands::copilot_login_target(settings).has_value());
        expect(!commands::copilot_login_target(gateway::config{}).has_value());
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}

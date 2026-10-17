#include <unistd.h>

#include <boost/ut.hpp>

import llmgate.arguments;
import llmgate.commands;
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
        where.options.as_vscode = true;
        where.options.vscode_file = file.string();

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
        where.options.as_vscode = true;
        where.options.vscode_file = file.string();
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

    "limit_an_identity_key_that_cannot_be_stored_fails_the_policy"_test = [] {
        sandbox box;
        std::filesystem::create_directories(box.root / "state");
        std::ofstream{box.root / "state" / "llmgate"} << "a file where the state directory should be";

        auto const policy = commands::make_policy(gateway::config{}, "127.0.0.1", 9480);

        expect(!policy.has_value());
    };

    "capability_the_login_target_is_the_first_provider_with_a_token_file"_test = [] {
        gateway::config settings;
        settings.providers.emplace("a-plain", gateway::provider_config{.base_url = "https://example.invalid"});
        settings.providers.emplace("b-copilot", with_token_file("/tmp/token-b.json"));
        settings.providers.emplace("c-copilot", with_token_file("/tmp/token-c.json"));

        auto const target = commands::copilot_login_target(settings);

        expect(target.has_value() && target->token_file == "/tmp/token-b.json");
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

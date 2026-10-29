#include <boost/ut.hpp>

import llmgate.doctor;
import llmgate.json;
import llmgate.quota;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate;
using namespace llmgate::doctor;

constexpr unsigned private_mode = 0600;
constexpr unsigned readable_mode = 0644;

facts healthy() {
    return {.listen = "127.0.0.1:9480",
            .identity = {.path = "/s/identity", .exists = true, .mode = private_mode},
            .profile_dir = "/p",
            .claude_found = true,
            .copilot_configured = true,
            .token = {.path = "/c/token.json", .exists = true, .mode = private_mode},
            .token_valid = true,
            .ledger_configured = true,
            .ledger = ledger_state::writable,
            .ledger_file = {.path = "/s/usage.jsonl", .exists = true, .mode = private_mode},
            .daily_tokens = 1000,
            .monthly_tokens = 20000,
            .warn_percent = 80};
}

finding named(std::vector<finding> const& found, std::string_view const check) {
    auto const at = std::ranges::find(found, check, &finding::check);
    return at == found.end() ? finding{.check = "missing"} : *at;
}

status state_of(facts const& seen, std::string_view const check) {
    return named(diagnose(seen), check).status;
}

quota::summary summary_with(int const percent) {
    return {.plan = "individual", .sku = "free", .reset_date = "2026-11-01",
            .quotas = {{.name = "chat", .has_quota = true, .entitlement = 200, .remaining = 4, .percent_remaining = percent}}};
}

const suite doctor_suite = [] {
    "capability_a_forced_subagent_model_is_a_warning_because_it_overrides_agent_types_and_spawn_rules"_test = [] {
        auto seen = healthy();
        auto const quiet = diagnose(seen);
        seen.subagent_model_forced = true;
        auto const loud = diagnose(seen);

        expect(!std::ranges::any_of(quiet, [](finding const& item) { return item.check == "subagents"; }));
        auto const found = std::ranges::find(loud, std::string{"subagents"}, &finding::check);
        expect(found != loud.end() && found->status == status::warn);
        expect(found->detail.contains("CLAUDE_CODE_SUBAGENT_MODEL_FORCE") && found->remedy.contains("unset"));
        expect(exit_code(loud) == 0);
    };

    "capability_an_agent_or_rule_whose_target_no_provider_offers_is_a_warning_naming_it_and_the_fix"_test = [] {
        auto seen = healthy();
        seen.dangling_targets = {"agent council-copilot targets claude-x@copilot", "rule Explore targets claude-y@codex"};

        auto const findings = diagnose(seen);
        auto const found = std::ranges::find(findings, std::string{"agent-targets"}, &finding::check);

        expect(found != findings.end() && found->status == status::warn);
        expect(found->detail.contains("council-copilot") && found->detail.contains("Explore") && found->remedy.contains("agents set"));
        expect(!std::ranges::any_of(diagnose(healthy()), [](finding const& item) { return item.check == "agent-targets"; }));
        expect(exit_code(diagnose(seen)) == 0);
    };

    "capability_an_offered_non_claude_model_without_a_known_window_is_a_warning_naming_it_and_the_fix"_test = [] {
        auto seen = healthy();
        seen.unknown_limits = {"copilot/gpt-6-luna", "local/qwen-4"};

        auto const findings = diagnose(seen);
        auto const found = std::ranges::find(findings, std::string{"model-limits"}, &finding::check);

        expect(found != findings.end() && found->status == status::warn);
        expect(found->detail.contains("copilot/gpt-6-luna") && found->detail.contains("200000") && found->remedy.contains("models offer"));
        expect(!std::ranges::any_of(diagnose(healthy()), [](finding const& item) { return item.check == "model-limits"; }));
        expect(exit_code(findings) == 0);
    };

    "capability_the_org_check_passes_warns_on_overridden_keys_fails_on_a_bad_pack_and_is_absent_without_one"_test = [] {
        auto const status_of = [](doctor::facts const& seen) -> std::optional<doctor::status> {
            auto const findings = doctor::diagnose(seen);
            auto const found = std::ranges::find(findings, std::string{"org"}, &doctor::finding::check);
            return found == findings.end() ? std::nullopt : std::optional<doctor::status>{found->status};
        };
        doctor::facts good;
        good.org = {.present = true, .name = "acme", .version = "1.0.0", .source = "git@host:acme/pack.git"};
        doctor::facts shadowed = good;
        shadowed.org.overridden = {"network"};
        doctor::facts broken;
        broken.org = {.problem = "the installed organisation pack is invalid: x"};
        auto const warned = doctor::diagnose(shadowed);
        auto const warning = std::ranges::find(warned, std::string{"org"}, &doctor::finding::check);

        expect(!status_of({}).has_value() && status_of(good) == doctor::status::pass && status_of(shadowed) == doctor::status::warn && status_of(broken) == doctor::status::fail);
        expect(warning->detail.contains("acme 1.0.0") && warning->detail.contains("network") && warning->remedy.contains("administrator"));
    };

    "capability_the_aws_check_names_where_credentials_come_from_fails_with_a_remedy_and_is_absent_without_a_signing_provider"_test = [] {
        auto const found = [](doctor::facts const& seen) {
            auto const findings = doctor::diagnose(seen);
            return std::ranges::find(findings, std::string{"aws"}, &doctor::finding::check) != findings.end();
        };
        doctor::facts good;
        good.aws = {{.provider = "bedrock", .source = "process"}};
        doctor::facts broken;
        broken.aws = {{.provider = "bedrock", .problem = "no AWS region: set region on the provider or AWS_REGION"}};
        auto const passed_findings = doctor::diagnose(good);
        auto const failed_findings = doctor::diagnose(broken);
        auto const passed = std::ranges::find(passed_findings, std::string{"aws"}, &doctor::finding::check);
        auto const failed = std::ranges::find(failed_findings, std::string{"aws"}, &doctor::finding::check);

        expect(!found({}) && found(good) && found(broken));
        expect(passed->status == doctor::status::pass && passed->detail.contains("credentials from process"));
        expect(failed->status == doctor::status::fail && failed->detail.contains("provider bedrock") && failed->remedy.contains("credential_process"));
    };

    "capability_the_network_check_fails_on_a_problem_warns_on_a_silent_proxy_and_is_absent_without_one"_test = [] {
        auto const named = [](facts const& seen) {
            auto const findings = diagnose(seen);
            return std::ranges::find(findings, std::string{"network"}, &finding::check) != findings.end();
        };
        auto broken = healthy();
        broken.network.problem = "network.ca_file cannot be read";
        auto silent = healthy();
        silent.network = {.proxy = "proxy.corp:3128", .proxy_answers = false};
        auto answering = healthy();
        answering.network = {.proxy = "proxy.corp:3128"};

        auto const broken_findings = diagnose(broken);
        auto const failed = std::ranges::find(broken_findings, std::string{"network"}, &finding::check);
        auto const silent_findings = diagnose(silent);
        auto const warned = std::ranges::find(silent_findings, std::string{"network"}, &finding::check);
        auto const answering_findings = diagnose(answering);
        auto const passed = std::ranges::find(answering_findings, std::string{"network"}, &finding::check);

        expect(failed->status == status::fail && failed->detail.contains("ca_file") && failed->remedy.contains("network.ca_file") && exit_code(broken_findings) != 0);
        expect(warned->status == status::warn && warned->detail.contains("proxy.corp:3128") && exit_code(silent_findings) == 0);
        expect(passed->status == status::pass && passed->detail.contains("through the proxy proxy.corp:3128"));
        expect(!named(healthy()));
    };

    "capability_a_healthy_setup_passes_every_check_and_exits_zero"_test = [] {
        auto const found = diagnose(healthy());

        expect(exit_code(found) == 0);
        expect(count_of(found, status::pass) == found.size());
    };

    "capability_a_chatgpt_sign_in_replaces_the_static_credential_check_and_passes_when_saved_privately_with_plan_scope"_test = [] {
        auto seen = healthy();
        seen.copilot_configured = false;
        seen.chatgpt = {.configured = true, .provider = "codex", .token = {.path = "/c/chatgpt.json", .exists = true, .mode = 0600}, .valid = true, .plan_scope = true};

        auto const found = diagnose(seen);

        expect(state_of(seen, "chatgpt-login") == status::pass);
        expect(std::ranges::none_of(found, [](finding const& item) { return item.check == "credential"; }));
        expect(exit_code(found) == 0);
    };

    "limit_a_missing_unreadable_open_or_planless_chatgpt_sign_in_fails_with_the_command_to_fix_it"_test = [] {
        auto seen = healthy();
        seen.copilot_configured = false;
        seen.chatgpt = {.configured = true, .provider = "codex", .token = {.path = "/c/chatgpt.json", .exists = false}, .valid = false, .plan_scope = false};
        expect(state_of(seen, "chatgpt-login") == status::fail && named(diagnose(seen), "chatgpt-login").remedy.contains("llmgate login --provider codex"));

        seen.chatgpt.token = {.path = "/c/chatgpt.json", .exists = true, .mode = 0600};
        expect(state_of(seen, "chatgpt-login") == status::fail);

        seen.chatgpt.valid = true;
        seen.chatgpt.token.mode = 0644;
        expect(state_of(seen, "chatgpt-login") == status::fail && named(diagnose(seen), "chatgpt-login").remedy.contains("chmod 600"));

        seen.chatgpt.token.mode = 0600;
        seen.chatgpt.plan_scope = false;
        expect(state_of(seen, "chatgpt-login") == status::fail && named(diagnose(seen), "chatgpt-login").detail.contains("plan use"));
    };

    "capability_copilot_and_chatgpt_can_both_be_configured_and_are_both_reported"_test = [] {
        auto seen = healthy();
        seen.chatgpt = {.configured = true, .provider = "codex", .token = {.path = "/c/chatgpt.json", .exists = true, .mode = 0600}, .valid = true, .plan_scope = true};

        expect(state_of(seen, "copilot-login") == status::pass && state_of(seen, "chatgpt-login") == status::pass);
    };

    "capability_a_broken_configuration_is_the_only_config_dependent_failure_reported"_test = [] {
        auto seen = healthy();
        seen.config_error = "cannot read ~/.config/llmgate/config.jsonc";

        auto const found = diagnose(seen);

        expect(found.front().check == "config" && found.front().status == status::fail);
        expect(exit_code(found) == 1);
        expect(named(found, "listen").check == "missing");
    };

    "capability_an_open_listen_address_needs_a_client_key"_test = [] {
        auto seen = healthy();
        seen.listen = "0.0.0.0:9480";

        expect(state_of(seen, "listen") == status::fail);
        seen.client_key_variable = "LLMGATE_KEY";
        seen.client_key_set = true;
        expect(state_of(seen, "listen") == status::pass);
        seen.client_key_set = false;
        expect(state_of(seen, "listen") == status::fail);
    };

    "limit_port_zero_is_a_warning_because_launches_cannot_share_a_gateway"_test = [] {
        auto seen = healthy();
        seen.listen = "127.0.0.1:0";

        expect(state_of(seen, "listen") == status::warn);
        expect(exit_code(diagnose(seen)) == 0);
    };

    "capability_a_stranger_on_the_port_fails_and_our_own_gateway_passes"_test = [] {
        auto seen = healthy();
        seen.port = port_state::foreign;
        expect(state_of(seen, "port") == status::fail);
        seen.port = port_state::ours;
        expect(state_of(seen, "port") == status::pass);
        seen.port = port_state::free;
        expect(state_of(seen, "port") == status::pass);
    };

    "limit_secret_files_readable_by_others_fail_with_the_chmod_remedy"_test = [] {
        auto seen = healthy();
        seen.identity.mode = readable_mode;
        seen.token.mode = readable_mode;

        auto const found = diagnose(seen);

        expect(named(found, "identity").status == status::fail && named(found, "identity").remedy == "chmod 600 /s/identity");
        expect(named(found, "copilot-login").status == status::fail && named(found, "copilot-login").remedy == "chmod 600 /c/token.json");
        expect(named(found, "identity").detail.contains("644"));
    };

    "capability_a_missing_copilot_login_points_at_llmgate_login"_test = [] {
        auto seen = healthy();
        seen.token.exists = false;

        expect(named(diagnose(seen), "copilot-login").remedy == "run llmgate login");
    };

    "capability_the_token_exchange_outcomes_map_to_pass_warn_and_fail"_test = [] {
        auto seen = healthy();
        seen.exchange = exchange_state::accepted;
        expect(state_of(seen, "copilot-exchange") == status::pass);
        seen.exchange = exchange_state::login_required;
        expect(state_of(seen, "copilot-exchange") == status::fail);
        seen.exchange = exchange_state::unreachable;
        expect(state_of(seen, "copilot-exchange") == status::warn);
        seen.exchange = exchange_state::rejected;
        expect(state_of(seen, "copilot-exchange") == status::warn);
        seen.exchange = exchange_state::skipped;
        expect(named(diagnose(seen), "copilot-exchange").check == "missing");
    };

    "capability_a_static_token_variable_must_be_named_and_set"_test = [] {
        auto seen = healthy();
        seen.copilot_configured = false;
        expect(state_of(seen, "credential") == status::warn);
        seen.static_token_variable = "UPSTREAM_TOKEN";
        expect(state_of(seen, "credential") == status::fail);
        seen.static_token_set = true;
        expect(state_of(seen, "credential") == status::pass);
    };

    "capability_every_profile_state_has_its_own_verdict"_test = [] {
        auto seen = healthy();
        seen.settings = settings_state::no_profile;
        expect(state_of(seen, "profile") == status::warn);
        seen.settings = settings_state::no_settings;
        expect(state_of(seen, "profile") == status::warn);
        seen.settings = settings_state::outdated;
        expect(state_of(seen, "profile") == status::warn);
        seen.settings = settings_state::invalid;
        expect(state_of(seen, "profile") == status::fail);
        seen.settings = settings_state::current;
        expect(state_of(seen, "profile") == status::pass);
    };

    "limit_a_missing_claude_command_only_warns"_test = [] {
        auto seen = healthy();
        seen.claude_found = false;

        expect(state_of(seen, "claude") == status::warn);
        expect(exit_code(diagnose(seen)) == 0);
    };

    "capability_the_ledger_must_be_writable_and_private"_test = [] {
        auto seen = healthy();
        seen.ledger = ledger_state::not_writable;
        expect(state_of(seen, "ledger") == status::fail);
        seen.ledger = ledger_state::writable;
        seen.ledger_file.mode = readable_mode;
        expect(state_of(seen, "ledger") == status::warn);
        seen.ledger_configured = false;
        expect(state_of(seen, "ledger") == status::pass);
    };

    "capability_budget_sanity_flags_a_warning_threshold_that_never_helps_and_inverted_limits"_test = [] {
        auto seen = healthy();
        seen.warn_percent = 0;
        expect(state_of(seen, "budget") == status::warn);
        seen.warn_percent = 100;
        expect(state_of(seen, "budget") == status::warn);
        seen.warn_percent = 80;
        seen.daily_tokens = 90000;
        expect(state_of(seen, "budget") == status::warn);
        seen.daily_tokens = 0;
        seen.monthly_tokens = 0;
        expect(state_of(seen, "budget") == status::pass);
    };

    "capability_a_low_copilot_quota_warns_with_the_reset_date_and_never_fails"_test = [] {
        auto seen = healthy();
        seen.quota = quota_state::read;
        seen.quota_summary = summary_with(2);

        auto const found = diagnose(seen);

        expect(named(found, "quota").status == status::warn);
        expect(named(found, "quota").detail.contains("chat 2% left") && named(found, "quota").detail.contains("2026-11-01"));
        expect(exit_code(found) == 0);
        seen.quota_summary = summary_with(70);
        expect(state_of(seen, "quota") == status::pass);
    };

    "capability_an_unavailable_quota_endpoint_degrades_to_a_warning"_test = [] {
        auto seen = healthy();
        seen.quota = quota_state::unavailable;

        expect(state_of(seen, "quota") == status::warn);
        seen.quota = quota_state::skipped;
        expect(named(diagnose(seen), "quota").check == "missing");
    };

    "capability_a_mapped_model_the_provider_disables_or_no_longer_lists_is_a_warning_with_the_fix"_test = [] {
        auto seen = healthy();
        seen.models = {{.provider = "copilot", .alias = "claude-sonnet-9", .upstream = "claude-sonnet-9.1", .state = model_state::disabled},
                       {.provider = "copilot", .alias = "claude-haiku-9", .upstream = "claude-haiku-9", .state = model_state::missing},
                       {.provider = "copilot", .alias = "claude-opus-9", .upstream = "claude-opus-9", .state = model_state::listed}};

        auto const found = diagnose(seen);
        auto const drift = std::ranges::count_if(found, [](finding const& item) { return item.check == "models" && item.status == status::warn; });

        expect(drift == 2_i);
        auto const first = named(found, "models");
        expect(first.detail.contains("claude-sonnet-9.1") && first.detail.contains("disabled") && first.remedy.contains("llmgate models --provider copilot"));
    };

    "capability_models_that_are_all_listed_and_enabled_pass_and_an_unchecked_list_is_silent"_test = [] {
        auto seen = healthy();
        expect(named(diagnose(seen), "models").check == "missing");
        seen.models = {{.provider = "copilot", .alias = "a", .upstream = "b", .state = model_state::listed}};

        expect(state_of(seen, "models") == status::pass);
        seen.models_error = "the provider did not answer";
        expect(state_of(seen, "models") == status::warn);
    };

    "limit_control_bytes_in_externally_supplied_text_never_reach_the_report"_test = [] {
        auto seen = healthy();
        seen.config_error = "bad \x1b[31m input\x07\n";

        auto const found = diagnose(seen);

        expect(render_text(found).find('\x1b') == std::string::npos);
        expect(render_text(found).find('\x07') == std::string::npos);
        expect(render_json(found).find('\x1b') == std::string::npos);
    };

    "capability_the_text_report_lists_status_check_and_fix_then_a_tally"_test = [] {
        auto seen = healthy();
        seen.identity.mode = readable_mode;

        auto const drawn = render_text(diagnose(seen));

        expect(drawn.contains("FAIL  identity:"));
        expect(drawn.contains("      fix: chmod 600 /s/identity"));
        expect(drawn.contains("PASS  config: the configuration parses"));
        expect(drawn.ends_with("1 fail\n"));
    };

    "capability_the_json_report_carries_ok_and_every_finding"_test = [] {
        auto seen = healthy();
        seen.token.exists = false;

        auto const found = diagnose(seen);
        auto const document = json::parse(render_json(found), {.jsonc = false});

        expect(document.has_value());
        auto const* ok = json::at(*document, {"ok"});
        expect(ok != nullptr && std::get<bool>(ok->data) == false);
        auto const* listed = json::at(*document, {"findings"});
        expect(listed != nullptr && std::get<json::array>(listed->data).size() == found.size());
    };

    "capability_the_json_report_keeps_its_bytes_and_key_order"_test = [] {
        constexpr std::string_view golden = R"json({
  "ok": false,
  "findings": [
    {
      "check": "config",
      "status": "fail",
      "detail": "bad?",
      "remedy": "fix the file, then check it with llmgate --check-config"
    },
    {
      "check": "identity",
      "status": "pass",
      "detail": "the identity file is private (0600)",
      "remedy": ""
    },
    {
      "check": "claude",
      "status": "pass",
      "detail": "the claude command is on PATH",
      "remedy": ""
    }
  ]
}
)json";
        auto seen = healthy();
        seen.config_error = "bad\x01";

        expect(render_json(diagnose(seen)) == golden);
        expect(render_json({}) == "{\n  \"ok\": true,\n  \"findings\": []\n}\n");
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}

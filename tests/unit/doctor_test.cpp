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
    "capability_a_healthy_setup_passes_every_check_and_exits_zero"_test = [] {
        auto const found = diagnose(healthy());

        expect(exit_code(found) == 0);
        expect(count_of(found, status::pass) == found.size());
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

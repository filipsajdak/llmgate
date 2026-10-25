export module llmgate.doctor;

import std;
import llmgate.arguments;
import llmgate.json;
import llmgate.quota;
import llmgate.reflect;
import llmgate.routing;
import llmgate.text;

export namespace llmgate::doctor {

enum class status { pass, warn, fail };
enum class check { config, identity, claude, listen, port, profile, copilot_login, copilot_exchange, chatgpt_login, credential, models, ledger, budget, quota, subagents, agent_targets, model_limits };
static_assert(reflect::unique_values<check>());

enum class port_state { free, ours, foreign };
enum class settings_state { no_profile, no_settings, outdated, current, invalid };
enum class ledger_state { disabled, writable, not_writable };
enum class exchange_state { skipped, accepted, login_required, unreachable, rejected };
enum class quota_state { skipped, unavailable, read };
enum class model_state { listed, missing, disabled };
static_assert(reflect::unique_values<status>() && reflect::unique_values<port_state>() && reflect::unique_values<settings_state>() &&
              reflect::unique_values<ledger_state>() && reflect::unique_values<exchange_state>() && reflect::unique_values<quota_state>() && reflect::unique_values<model_state>());

constexpr std::size_t longest_text = 300;
constexpr unsigned others_mask = 077;
constexpr unsigned octal_base = 8;
constexpr unsigned long_budget_percent = 100;

struct finding {
    std::string check{};
    doctor::status status = doctor::status::pass;
    std::string detail{};
    std::string remedy{};
};

struct file_fact {
    std::string path{};
    bool exists = false;
    unsigned mode = 0;
};

struct chatgpt_fact {
    bool configured = false;
    std::string provider{};
    file_fact token{};
    bool valid = false;
    bool plan_scope = false;
};

struct model_fact {
    std::string provider{};
    std::string alias{};
    std::string upstream{};
    model_state state = model_state::listed;
};

struct facts {
    std::string config_error{};
    std::string listen{};
    std::string client_key_variable{};
    bool client_key_set = false;
    std::optional<port_state> port{};
    file_fact identity{};
    std::string profile_dir{};
    settings_state settings = settings_state::current;
    bool claude_found = false;
    bool copilot_configured = false;
    file_fact token{};
    bool token_valid = false;
    chatgpt_fact chatgpt{};
    exchange_state exchange = exchange_state::skipped;
    std::string static_token_variable{};
    bool static_token_set = false;
    bool ledger_configured = false;
    ledger_state ledger = ledger_state::disabled;
    file_fact ledger_file{};
    std::uint64_t daily_tokens = 0;
    std::uint64_t monthly_tokens = 0;
    unsigned warn_percent = 0;
    quota_state quota = quota_state::skipped;
    std::optional<quota::summary> quota_summary{};
    std::vector<model_fact> models{};
    std::string models_error{};
    bool subagent_model_forced = false;
    std::vector<std::string> dangling_targets{};
    std::vector<std::string> unknown_limits{};
};

}  // namespace llmgate::doctor

namespace llmgate::doctor::detail {

constexpr std::string clean(std::string_view const raw) {
    std::string out;
    for (auto const c : raw.substr(0, longest_text)) {
        out += (static_cast<unsigned char>(c) < ' ' || c == '\x7f') ? '?' : c;
    }
    return out;
}

constexpr std::string name_of(check const item) {
    auto name = reflect::enum_string(item);
    std::ranges::replace(name, '_', '-');
    return name;
}

constexpr finding make(check const item, status const state, std::string_view const detail, std::string_view const remedy = {}) {
    return {.check = name_of(item), .status = state, .detail = clean(detail), .remedy = clean(remedy)};
}

constexpr std::string octal(unsigned const mode) {
    std::array<char, 16> digits{};
    return std::string{digits.data(), std::to_chars(digits.data(), digits.data() + digits.size(), mode & 0777U, static_cast<int>(octal_base)).ptr};
}

constexpr bool too_open(file_fact const& file) {
    return file.exists && (file.mode & others_mask) != 0;
}

constexpr bool loopback_listen(arguments::gateway_address const& address) {
    return routing::is_loopback_host(address.host);
}

constexpr finding config_finding(facts const& seen) {
    if (!seen.config_error.empty()) {
        return make(check::config, status::fail, seen.config_error, "fix the file, then check it with llmgate --check-config");
    }
    return make(check::config, status::pass, "the configuration parses");
}

constexpr std::optional<finding> key_problem(facts const& seen, arguments::gateway_address const& address) {
    if (!seen.client_key_variable.empty() && !seen.client_key_set) {
        return make(check::listen, status::fail, "client_key_env names " + seen.client_key_variable + ", which is not set", "export " + seen.client_key_variable + " before starting the gateway");
    }
    if (!loopback_listen(address) && seen.client_key_variable.empty()) {
        return make(check::listen, status::fail, address.host + " is not a loopback address and no client key is configured", "listen on 127.0.0.1, or set client_key_env");
    }
    return std::nullopt;
}

constexpr finding listen_finding(facts const& seen) {
    auto const address = arguments::split_listen(seen.listen);
    if (!address) {
        return make(check::listen, status::fail, "listen must look like 127.0.0.1:9480", "set listen in the configuration");
    }
    if (auto const problem = key_problem(seen, *address)) {
        return *problem;
    }
    if (address->port == 0) {
        return make(check::listen, status::warn, "port 0 picks a new port on every start", "set a fixed port so every launch shares one gateway");
    }
    return make(check::listen, status::pass, seen.listen + (loopback_listen(*address) ? " (loopback)" : " (open, client key required)"));
}

constexpr std::optional<finding> port_finding(facts const& seen) {
    if (!seen.port) {
        return std::nullopt;
    }
    switch (*seen.port) {
        case port_state::free:
            return make(check::port, status::pass, "nothing is listening on " + seen.listen + " yet");
        case port_state::ours:
            return make(check::port, status::pass, "this user's llmgate gateway answers on " + seen.listen);
        case port_state::foreign:
            break;
    }
    return make(check::port, status::fail, "something that is not this user's gateway answers on " + seen.listen, "stop it or change listen; nothing was sent to it");
}

constexpr finding identity_finding(facts const& seen) {
    if (!seen.identity.exists) {
        return make(check::identity, status::pass, "the identity key is created on the first start");
    }
    if (too_open(seen.identity)) {
        return make(check::identity, status::fail, "the identity file has mode " + octal(seen.identity.mode) + ", other users can read it", "chmod 600 " + seen.identity.path);
    }
    return make(check::identity, status::pass, "the identity file is private (0600)");
}

constexpr finding profile_finding(facts const& seen) {
    switch (seen.settings) {
        case settings_state::no_profile:
            return make(check::profile, status::warn, "the profile directory " + seen.profile_dir + " does not exist", "run llmgate setup");
        case settings_state::no_settings:
            return make(check::profile, status::warn, "settings.json is missing in " + seen.profile_dir, "run llmgate setup");
        case settings_state::outdated:
            return make(check::profile, status::warn, "settings.json differs from what setup would write", "run llmgate setup (your own settings are kept)");
        case settings_state::invalid:
            return make(check::profile, status::fail, "settings.json in " + seen.profile_dir + " is not plain JSON", "fix or remove it; setup refuses to change it");
        case settings_state::current:
            break;
    }
    return make(check::profile, status::pass, "settings.json is merged in " + seen.profile_dir);
}

constexpr finding claude_finding(facts const& seen) {
    if (!seen.claude_found) {
        return make(check::claude, status::warn, "the claude command is not on PATH", "install Claude Code or set LLMGATE_CLAUDE (VS Code users can ignore this)");
    }
    return make(check::claude, status::pass, "the claude command is on PATH");
}

constexpr finding token_file_finding(facts const& seen) {
    if (!seen.token.exists) {
        return make(check::copilot_login, status::fail, "no Copilot login at " + seen.token.path, "run llmgate login");
    }
    if (!seen.token_valid) {
        return make(check::copilot_login, status::fail, "the token file holds no oauth_token", "run llmgate login");
    }
    if (too_open(seen.token)) {
        return make(check::copilot_login, status::fail, "the token file has mode " + octal(seen.token.mode) + ", other users can read it", "chmod 600 " + seen.token.path);
    }
    return make(check::copilot_login, status::pass, "the Copilot token file is present and private");
}

constexpr finding chatgpt_finding(chatgpt_fact const& seen) {
    std::string const fix = "run llmgate login --provider " + seen.provider;
    if (!seen.token.exists) {
        return make(check::chatgpt_login, status::fail, "no ChatGPT sign-in at " + seen.token.path, fix);
    }
    if (!seen.valid) {
        return make(check::chatgpt_login, status::fail, "the token file holds no usable sign-in", fix);
    }
    if (too_open(seen.token)) {
        return make(check::chatgpt_login, status::fail, "the token file has mode " + octal(seen.token.mode) + ", other users can read it", "chmod 600 " + seen.token.path);
    }
    if (!seen.plan_scope) {
        return make(check::chatgpt_login, status::fail, "the sign-in does not grant ChatGPT plan use", "enable plan use in ChatGPT settings, then " + fix);
    }
    return make(check::chatgpt_login, status::pass, "the ChatGPT sign-in is saved privately and grants plan use");
}

constexpr std::optional<finding> exchange_finding(facts const& seen) {
    switch (seen.exchange) {
        case exchange_state::skipped:
            return std::nullopt;
        case exchange_state::accepted:
            return make(check::copilot_exchange, status::pass, "GitHub accepted the stored login");
        case exchange_state::login_required:
            return make(check::copilot_exchange, status::fail, "GitHub rejected the stored login", "run llmgate login");
        case exchange_state::unreachable:
            return make(check::copilot_exchange, status::warn, "GitHub could not be reached", "check the network, or run doctor with --offline");
        case exchange_state::rejected:
            break;
    }
    return make(check::copilot_exchange, status::warn, "GitHub answered the token exchange with an error", "retry later; run llmgate login if it persists");
}

constexpr std::optional<finding> subagent_finding(facts const& seen) {
    if (!seen.subagent_model_forced) {
        return std::nullopt;
    }
    return make(check::subagents, status::warn, "CLAUDE_CODE_SUBAGENT_MODEL_FORCE is set, so every subagent runs on that one model and the llmgate agent types and spawn rules are ignored", "unset CLAUDE_CODE_SUBAGENT_MODEL_FORCE (also check the env block of the profile's settings.json)");
}

constexpr std::optional<finding> agent_targets_finding(facts const& seen) {
    if (seen.dangling_targets.empty()) {
        return std::nullopt;
    }
    std::string detail = "no provider offers the target of:";
    for (auto const& item : seen.dangling_targets) {
        text::append_all(detail, " ", item, ";");
    }
    return make(check::agent_targets, status::warn, detail, "llmgate agents set NAME TARGET (or agents remove NAME, rules set AGENT TARGET); llmgate targets lists the targets");
}

constexpr std::uint64_t assumed_window_tokens = 200000;

constexpr std::optional<finding> model_limits_finding(facts const& seen) {
    if (seen.unknown_limits.empty()) {
        return std::nullopt;
    }
    std::string detail;
    text::append_all(detail, "no context window is recorded for:");
    for (auto const& item : seen.unknown_limits) {
        text::append_all(detail, " ", item, ";");
    }
    text::append_all(detail, " Claude Code assumes ", text::decimal(assumed_window_tokens), " tokens for a model it does not know and compacts by that");
    return make(check::model_limits, status::warn, detail, "llmgate models offer PROVIDER MODEL... records the limits the provider reports");
}

constexpr finding static_credential_finding(facts const& seen) {
    if (seen.static_token_variable.empty()) {
        return make(check::credential, status::warn, "no upstream credential is configured", "set copilot.token_file, or static_token_env for another upstream");
    }
    if (!seen.static_token_set) {
        return make(check::credential, status::fail, "static_token_env names " + seen.static_token_variable + ", which is not set", "export " + seen.static_token_variable);
    }
    return make(check::credential, status::pass, "the upstream token variable " + seen.static_token_variable + " is set");
}

constexpr finding ledger_finding(facts const& seen) {
    if (!seen.ledger_configured) {
        return make(check::ledger, status::pass, "the usage ledger is off");
    }
    if (seen.ledger == ledger_state::not_writable) {
        return make(check::ledger, status::fail, "the usage ledger " + seen.ledger_file.path + " cannot be written", "fix the permissions of its directory or change usage.ledger_file");
    }
    if (too_open(seen.ledger_file)) {
        return make(check::ledger, status::warn, "the usage ledger has mode " + octal(seen.ledger_file.mode) + ", other users can read it", "chmod 600 " + seen.ledger_file.path);
    }
    return make(check::ledger, status::pass, "the usage ledger is writable");
}

constexpr bool daily_above_monthly(facts const& seen) {
    return seen.daily_tokens != 0 && seen.monthly_tokens != 0 && seen.daily_tokens > seen.monthly_tokens;
}

constexpr std::string_view budget_problem(facts const& seen) {
    if (seen.warn_percent == 0 || seen.warn_percent >= long_budget_percent) {
        return "set warn_percent between 1 and 99 so the warning appears before the limit";
    }
    if (daily_above_monthly(seen)) {
        return "the daily limit is above the monthly one, so the monthly limit always binds first";
    }
    return {};
}

constexpr finding budget_finding(facts const& seen) {
    if (seen.daily_tokens == 0 && seen.monthly_tokens == 0) {
        return make(check::budget, status::pass, "no token budget is set");
    }
    auto const shown = "daily " + text::compact(seen.daily_tokens) + ", monthly " + text::compact(seen.monthly_tokens) + " tokens, warning at " +
                       text::decimal(seen.warn_percent) + "%";
    auto const problem = budget_problem(seen);
    return make(check::budget, problem.empty() ? status::pass : status::warn, shown, problem);
}

constexpr std::string lowest_summary(quota::summary const& found) {
    std::string out;
    for (auto const& item : found.quotas) {
        if (quota::is_low(item)) {
            text::append_all(out, out.empty() ? "" : ", ", item.name, " ", text::decimal(item.percent_remaining), "% left");
        }
    }
    if (!found.reset_date.empty()) {
        text::append_all(out, "; resets ", found.reset_date);
    }
    return out;
}

constexpr std::optional<finding> quota_finding(facts const& seen) {
    if (seen.quota == quota_state::skipped) {
        return std::nullopt;
    }
    if (seen.quota == quota_state::unavailable || !seen.quota_summary) {
        return make(check::quota, status::warn, quota::unavailable_message, "retry later; requests are not affected");
    }
    if (quota::any_low(*seen.quota_summary)) {
        return make(check::quota, status::warn, "Copilot quota is low: " + lowest_summary(*seen.quota_summary), "wait for the reset, route fewer requests to Copilot, or change plan");
    }
    return make(check::quota, status::pass, "Copilot quota is healthy (" + seen.quota_summary->plan + ")");
}

constexpr std::string_view reason_of(model_state const state) {
    return state == model_state::disabled ? "disabled by the account's policy" : "not listed by the provider";
}

constexpr void add_models(std::vector<finding>& out, facts const& seen) {
    if (!seen.models_error.empty()) {
        out.push_back(make(check::models, status::warn, "the model lists could not be read: " + seen.models_error, "run llmgate models"));
        return;
    }
    if (seen.models.empty()) {
        return;
    }
    bool drifted = false;
    for (auto const& model : seen.models) {
        if (model.state == model_state::listed) {
            continue;
        }
        drifted = true;
        std::string detail;
        text::append_all(detail, "provider ", model.provider, " maps ", model.alias, " to ", model.upstream, ", which is ", reason_of(model.state));
        out.push_back(make(check::models, status::warn, detail, "enable it for the account, or map the alias to a model that llmgate models --provider " + model.provider + " lists as usable"));
    }
    if (!drifted) {
        out.push_back(make(check::models, status::pass, "every mapped model is listed and enabled"));
    }
}

constexpr void add(std::vector<finding>& out, std::optional<finding> item) {
    if (item) {
        out.push_back(std::move(*item));
    }
}

constexpr void add_credential(std::vector<finding>& out, facts const& seen) {
    if (seen.chatgpt.configured) {
        out.push_back(chatgpt_finding(seen.chatgpt));
    }
    if (!seen.copilot_configured) {
        if (!seen.chatgpt.configured) {
            out.push_back(static_credential_finding(seen));
        }
        return;
    }
    out.push_back(token_file_finding(seen));
    add(out, exchange_finding(seen));
}

}  // namespace llmgate::doctor::detail

export namespace llmgate::doctor {

constexpr std::vector<finding> diagnose(facts const& seen) {
    std::vector<finding> out;
    out.push_back(detail::config_finding(seen));
    out.push_back(detail::identity_finding(seen));
    out.push_back(detail::claude_finding(seen));
    if (!seen.config_error.empty()) {
        return out;
    }
    out.push_back(detail::listen_finding(seen));
    detail::add(out, detail::port_finding(seen));
    out.push_back(detail::profile_finding(seen));
    detail::add_credential(out, seen);
    detail::add_models(out, seen);
    out.push_back(detail::ledger_finding(seen));
    out.push_back(detail::budget_finding(seen));
    detail::add(out, detail::quota_finding(seen));
    detail::add(out, detail::subagent_finding(seen));
    detail::add(out, detail::agent_targets_finding(seen));
    detail::add(out, detail::model_limits_finding(seen));
    return out;
}

constexpr std::string label_of(status const state) {
    auto label = reflect::enum_string(state);
    std::ranges::transform(label, label.begin(), [](char const c) { return static_cast<char>(c - 'a' + 'A'); });
    return label;
}

constexpr std::size_t count_of(std::span<finding const> const findings, status const state) {
    return static_cast<std::size_t>(std::ranges::count(findings, state, &finding::status));
}

constexpr int exit_code(std::span<finding const> const findings) {
    return count_of(findings, status::fail) == 0 ? 0 : 1;
}

constexpr std::string render_text(std::span<finding const> const findings) {
    std::string out;
    for (auto const& item : findings) {
        text::append_all(out, label_of(item.status), "  ", item.check, ": ", item.detail, "\n");
        if (item.status != status::pass) {
            text::append_all(out, "      fix: ", item.remedy, "\n");
        }
    }
    text::append_all(out, text::decimal(count_of(findings, status::pass)), " pass, ", text::decimal(count_of(findings, status::warn)), " warn, ",
                     text::decimal(count_of(findings, status::fail)), " fail\n");
    return out;
}

struct diagnosis {
    bool ok = true;
    std::vector<finding> findings{};
};

constexpr std::string render_json(std::span<finding const> const findings) {
    return json::write(json::to_value(diagnosis{.ok = exit_code(findings) == 0, .findings = {findings.begin(), findings.end()}}), 2);
}

}  // namespace llmgate::doctor

namespace llmgate::doctor {

constexpr facts healthy_facts() {
    return {.listen = "127.0.0.1:9480",
            .identity = {.path = "/s/identity", .exists = true, .mode = 0600},
            .profile_dir = "/p",
            .claude_found = true,
            .copilot_configured = true,
            .token = {.path = "/c/token.json", .exists = true, .mode = 0600},
            .token_valid = true,
            .ledger_configured = true,
            .ledger = ledger_state::writable,
            .ledger_file = {.path = "/s/usage.jsonl", .exists = true, .mode = 0600},
            .daily_tokens = 1000,
            .monthly_tokens = 20000,
            .warn_percent = 80};
}

constexpr bool ok_with(auto const change) {
    auto seen = healthy_facts();
    change(seen);
    auto const found = diagnose(seen);
    return exit_code(found) == 0;
}

constexpr bool fails_with(auto const change) {
    auto seen = healthy_facts();
    change(seen);
    return exit_code(diagnose(seen)) == 1;
}

static_assert(ok_with([](facts&) {}));
static_assert(fails_with([](facts& seen) { seen.config_error = "bad"; }));
static_assert(fails_with([](facts& seen) { seen.listen = "0.0.0.0:80"; }));
static_assert(ok_with([](facts& seen) { seen.listen = "0.0.0.0:80"; seen.client_key_variable = "K"; seen.client_key_set = true; }));
static_assert(fails_with([](facts& seen) { seen.client_key_variable = "K"; }));
static_assert(fails_with([](facts& seen) { seen.listen = "nonsense"; }));
static_assert(ok_with([](facts& seen) { seen.listen = "127.0.0.1:0"; }));
static_assert(fails_with([](facts& seen) { seen.port = port_state::foreign; }));
static_assert(ok_with([](facts& seen) { seen.port = port_state::ours; }));
static_assert(fails_with([](facts& seen) { seen.identity.mode = 0644; }));
static_assert(fails_with([](facts& seen) { seen.token.exists = false; }));
static_assert(fails_with([](facts& seen) { seen.token.mode = 0640; }));
static_assert(fails_with([](facts& seen) { seen.exchange = exchange_state::login_required; }));
static_assert(ok_with([](facts& seen) { seen.exchange = exchange_state::unreachable; }));
static_assert(fails_with([](facts& seen) { seen.settings = settings_state::invalid; }));
static_assert(ok_with([](facts& seen) { seen.settings = settings_state::outdated; seen.claude_found = false; }));
static_assert(fails_with([](facts& seen) { seen.ledger_configured = true; seen.ledger = ledger_state::not_writable; }));
static_assert(ok_with([](facts& seen) { seen.warn_percent = 100; }));
static_assert(ok_with([](facts& seen) { seen.daily_tokens = 50000; }));
static_assert(diagnose(healthy_facts()).size() == 8 && diagnose(facts{.config_error = "x"}).size() == 3);
static_assert(count_of(diagnose([] { auto seen = healthy_facts(); seen.warn_percent = 100; return seen; }()), status::warn) == 1);
static_assert(render_text(diagnose(healthy_facts())).ends_with("8 pass, 0 warn, 0 fail\n"));
static_assert(render_text(diagnose(facts{.config_error = "a\x1b[31m"})).find('\x1b') == std::string::npos);
static_assert(label_of(status::pass) == "PASS" && label_of(status::warn) == "WARN" && label_of(status::fail) == "FAIL");
static_assert(detail::name_of(check::copilot_login) == "copilot-login" && detail::name_of(check::copilot_exchange) == "copilot-exchange" && detail::name_of(check::quota) == "quota");
static_assert(detail::octal(0644) == "644" && detail::octal(0600) == "600");

}  // namespace llmgate::doctor

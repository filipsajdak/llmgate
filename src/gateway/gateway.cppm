module;

#include <meta>

export module llmgate.gateway;

import std;
import llmgate.anthropic;
import llmgate.async;
import llmgate.capture;
import llmgate.claude;
import llmgate.control;
import llmgate.history;
import llmgate.http;
import llmgate.httpio;
import llmgate.json;
import llmgate.members;
import llmgate.net;
import llmgate.proof;
import llmgate.reflect;
import llmgate.responses_request;
import llmgate.responses_stream;
import llmgate.role;
import llmgate.routing;
import llmgate.selection;
import llmgate.shutdown;
import llmgate.sse;
import llmgate.tls;
import llmgate.usage;

export import llmgate.config;

export namespace llmgate::gateway {

using reflect::member_named;

constexpr std::string effective_messages_path(wire_protocol const protocol, std::string_view const configured) {
    return protocol == wire_protocol::openai_responses && configured == default_messages_path ? std::string{default_responses_path} : std::string{configured};
}

provider_config legacy_provider(config const& settings) {
    provider_config out;
    template for (constexpr auto field : std::define_static_array(std::meta::nonstatic_data_members_of(^^provider_config, std::meta::access_context::current()))) {
        constexpr auto from_upstream = member_named(^^upstream_config, std::meta::identifier_of(field));
        constexpr auto from_config = member_named(^^config, std::meta::identifier_of(field));
        static_assert(from_upstream || from_config, "a provider_config member has no legacy source of the same name");
        if constexpr (from_upstream) {
            static_assert(std::meta::type_of(*from_upstream) == std::meta::type_of(field), "the legacy upstream member has another type");
            out.[:field:] = settings.upstream.[:*from_upstream:];
        } else {
            static_assert(std::meta::type_of(*from_config) == std::meta::type_of(field), "the legacy config member has another type");
            out.[:field:] = settings.[:*from_config:];
        }
    }
    return out;
}

std::optional<preset_defaults> defaults_of(preset_kind const kind) {
    std::optional<preset_defaults> found;
    template for (constexpr auto enumerator : std::define_static_array(std::meta::enumerators_of(^^preset_kind))) {
        if constexpr (reflect::has_annotation<preset_defaults>(enumerator)) {
            if (kind == [:enumerator:]) {
                found = *reflect::annotation_of<preset_defaults>(enumerator);
            }
        }
    }
    return found;
}

provider_config with_preset(provider_config item) {
    if (auto const defaults = defaults_of(item.preset)) {
        if (item.base_url.empty()) {
            item.base_url = defaults->base_url;
        }
        if (item.protocol == wire_protocol::anthropic) {
            item.protocol = defaults->protocol;
        }
    }
    return item;
}

std::map<std::string, provider_config> effective_providers(config const& settings) {
    auto all = settings.providers;
    for (auto& entry : all) {
        entry.second = with_preset(std::move(entry.second));
    }
    if (!settings.upstream.base_url.empty() && !all.contains(std::string{implicit_provider})) {
        all.emplace(std::string{implicit_provider}, legacy_provider(settings));
    }
    return all;
}

std::vector<selection::role_route> role_routes(config const& settings) {
    std::vector<selection::role_route> routes;
    for (auto const& [role, names] : settings.routes) {
        routes.push_back({.role = role, .providers = names});
    }
    return routes;
}

std::vector<std::string> provider_names(std::map<std::string, provider_config> const& providers) {
    std::vector<std::string> names;
    for (auto const& entry : providers) {
        names.push_back(entry.first);
    }
    return names;
}

std::string provider_problem(std::string const& name, provider_config const& item, config const& settings) {
    if (item.preset == preset_kind::openai_chat) {
        return "provider " + name + ": the openai_chat preset is not built yet";
    }
    if (auto const parsed = routing::parse_url(item.base_url); !parsed) {
        return "provider " + name + ": " + parsed.error();
    }
    if (item.forward_credentials && !settings.client_key_env.empty()) {
        return "provider " + name + " forwards the client's credentials, which cannot also carry the gateway key: remove client_key_env";
    }
    if (item.forward_credentials && item.protocol != wire_protocol::anthropic) {
        return "provider " + name + " forwards the client's credentials (forward_credentials), which only an anthropic provider can use";
    }
    return {};
}

std::string config_problem(config const& settings) {
    auto const providers = effective_providers(settings);
    if (providers.empty()) {
        return "configure upstream.base_url or at least one entry in providers";
    }
    for (auto const& [name, item] : providers) {
        if (auto problem = provider_problem(name, item, settings); !problem.empty()) {
            return problem;
        }
    }
    return selection::route_problem(role_routes(settings), provider_names(providers));
}

struct access_policy {
    std::string client_key{};
    std::vector<std::string> allowed_hosts{};
    std::string identity_key{};
    std::string identity_scope{};
};

class credential {
public:
    virtual ~credential() = default;
    virtual async::task<std::expected<std::string, std::error_code>> bearer() = 0;
    virtual void rejected() noexcept {}
    virtual std::optional<std::string> endpoint() const { return std::nullopt; }
};

class static_credential final : public credential {
public:
    explicit static_credential(std::string token) : token_{std::move(token)} {}

    async::task<std::expected<std::string, std::error_code>> bearer() override {
        if (token_.empty()) {
            co_return std::unexpected{std::make_error_code(std::errc::permission_denied)};
        }
        co_return token_;
    }

private:
    std::string token_;
};

struct provider_stats {
    std::uint64_t attempts = 0;
    std::uint64_t answered = 0;
    std::uint64_t failures = 0;
    std::uint64_t skipped = 0;
};

using counter = std::uint64_t;

struct repair_counts;

consteval {
    std::vector<std::meta::info> counters;
    for (auto const kind : std::meta::enumerators_of(^^anthropic::repair_kind)) {
        counters.push_back(std::meta::data_member_spec(^^counter, {.name = std::meta::identifier_of(kind)}));
    }
    std::meta::define_aggregate(^^repair_counts, counters);
}

static_assert(sizeof(repair_counts) == reflect::enum_count<anthropic::repair_kind> * sizeof(counter));

template <class Counts>
    requires std::same_as<std::remove_const_t<Counts>, repair_counts>
constexpr auto& repair_slot(Counts& counts, anthropic::repair_kind const kind) {
    std::conditional_t<std::is_const_v<Counts>, counter const, counter>* slot = nullptr;
    template for (constexpr auto enumerator : std::define_static_array(std::meta::enumerators_of(^^anthropic::repair_kind))) {
        if (kind == [:enumerator:]) {
            slot = &counts.[:*member_named(^^repair_counts, std::meta::identifier_of(enumerator)):];
        }
    }
    return *slot;
}

constexpr std::size_t repair_example_chars = 80;

struct stats {
    std::uint64_t requests = 0;
    std::uint64_t streamed = 0;
    std::uint64_t upstream_failures = 0;
    std::uint64_t upstream_timeouts = 0;
    std::uint64_t client_disconnects = 0;
    std::uint64_t refused = 0;
    std::uint64_t budget_refused = 0;
    std::uint64_t no_provider = 0;
    std::uint64_t connections_opened = 0;
    std::uint64_t connections_reused = 0;
    repair_counts repairs{};
    std::map<std::string, std::string> repair_examples{};
    routing::beta_counts betas{};
    std::map<std::string, std::uint64_t> dropped_fields{};
    std::uint64_t thinking_blocks_stripped = 0;
    std::uint64_t max_tokens_clamped = 0;
    std::map<std::string, std::uint64_t> classifier_requests{};
    std::map<std::string, std::uint64_t> classifier_unparseable{};
    std::map<std::string, std::uint64_t> ignored_events{};
    [[= json::skip]] std::map<std::string, provider_stats> providers;
};

using credential_map = std::map<std::string, credential*>;

}  // namespace llmgate::gateway

namespace llmgate::gateway::detail {

constexpr std::string_view user_agent = "llmgate/0.0";

constexpr auto copilot_client_headers = std::to_array<std::pair<std::string_view, std::string_view>>(
    {{"Editor-Version", vscode_editor_version}, {"Copilot-Integration-Id", vscode_integration_id}});

constexpr unsigned char ascii_case_bit = 0x20;
constexpr std::string_view anthropic_header_prefix = "anthropic-";

constexpr bool is_anthropic_header(std::string_view const name) noexcept {
    return name.size() > anthropic_header_prefix.size() &&
           std::ranges::equal(name.substr(0, anthropic_header_prefix.size()), anthropic_header_prefix,
                              [](char const given, char const wanted) { return (static_cast<unsigned char>(given) | ascii_case_bit) == static_cast<unsigned char>(wanted); });
}

static_assert(is_anthropic_header("anthropic-beta") && is_anthropic_header("Anthropic-Workspace-Id") && !is_anthropic_header("anthropic-") && !is_anthropic_header("x-anthropic-beta"));

void copy_present(http::header_map& to, http::header_map const& from, std::string_view const lookup, std::string_view const sent_as) {
    if (auto const found = from.find(lookup)) {
        to.add(std::string{sent_as}, std::string{*found});
    }
}

void copy_request_headers(http::header_map& to, http::header_map const& from, wire_protocol const wire) {
    constexpr auto shared = std::to_array<std::string_view>({"content-type", "accept"});
    constexpr auto anthropic_only = std::to_array<std::string_view>({"anthropic-version", "anthropic-beta"});
    for (auto const name : shared) {
        copy_present(to, from, name, name);
    }
    for (auto const name : wire == wire_protocol::anthropic ? std::span<std::string_view const>{anthropic_only} : std::span<std::string_view const>{}) {
        copy_present(to, from, name, name);
    }
}

void forward_client_credentials(http::header_map& to, http::header_map const& from) {
    copy_present(to, from, "authorization", "Authorization");
    copy_present(to, from, "x-api-key", "x-api-key");
    for (auto const& field : from.all()) {
        if (is_anthropic_header(field.name) && !to.find(field.name)) {
            to.add(field.name, field.value);
        }
    }
}

void add_missing(http::header_map& to, std::span<std::pair<std::string_view, std::string_view> const> const defaults) {
    for (auto const& [name, content] : defaults) {
        if (!to.find(name)) {
            to.add(std::string{name}, std::string{content});
        }
    }
}

http::header_map upstream_headers(httpio::request const& incoming, std::map<std::string, std::string> const& extra, bool const copilot_client,
                                  bool const forward_credentials, std::string_view const token, wire_protocol const wire) {
    http::header_map fields;
    copy_request_headers(fields, incoming.head.fields, wire);
    if (forward_credentials) {
        forward_client_credentials(fields, incoming.head.fields);
    }
    for (auto const& [name, content] : extra) {
        fields.add(name, content);
    }
    if (copilot_client) {
        add_missing(fields, copilot_client_headers);
    }
    if (!forward_credentials) {
        fields.add("Authorization", std::format("Bearer {}", token));
    }
    fields.add("User-Agent", std::string{user_agent});
    return fields;
}

constexpr std::string_view model_epoch = "1970-01-01T00:00:00Z";

struct model_entry {
    std::string type = "model";
    std::string id{};
    std::string display_name{};
    std::string created_at = std::string{model_epoch};
    std::optional<std::string> description{};
    std::optional<std::uint64_t> max_input_tokens{};
    std::optional<std::uint64_t> max_tokens{};
};

struct model_list {
    std::vector<model_entry> data{};
    bool has_more = false;
};

struct provider_report {
    [[= json::flatten]] provider_stats counts{};
    selection::circuit circuit = selection::circuit::closed;
};

constexpr std::optional<std::uint64_t> known_limit(std::uint64_t const value) {
    return value == 0 ? std::nullopt : std::optional{value};
}

template <std::invocable<control::target const&> Limits>
std::string models_json(std::set<std::string> const& aliases, std::span<control::target const> const targets, Limits const& limits_of) {
    model_list listing;
    for (auto const& alias : aliases) {
        listing.data.push_back({.id = alias, .display_name = alias});
    }
    for (auto const& item : targets) {
        auto const limits = limits_of(item);
        listing.data.push_back({.id = item.id,
                                .display_name = item.display_name,
                                .description = std::format("served by the provider {}", item.provider),
                                .max_input_tokens = known_limit(limits.prompt_tokens),
                                .max_tokens = known_limit(limits.output_tokens)});
    }
    return json::write(json::to_value(listing));
}

struct route_report {
    std::string role{};
    std::string via{};
    std::vector<std::string> providers{};
    std::optional<std::string> upstream{};
};

struct overview_target {
    std::string id{};
    std::string provider{};
    std::string upstream{};
    std::string label{};
    std::optional<std::uint64_t> max_input_tokens{};
    std::optional<std::uint64_t> max_tokens{};
};

struct overview_provider {
    std::string name{};
    [[= json::flatten]] provider_stats counts{};
    selection::circuit circuit = selection::circuit::closed;
    std::map<std::string, std::string> rate_limits{};
};

struct overview_spend {
    std::vector<usage::spend> today{};
    std::vector<usage::spend> month{};
};

struct overview_document {
    int schema_version = 1;
    std::vector<route_report> roles{};
    std::optional<control::served> served{};
    std::vector<overview_target> targets{};
    std::vector<overview_provider> providers{};
    overview_spend spend{};
    usage::summary totals{};
};

constexpr std::string_view rate_limit_prefix = "anthropic-ratelimit-";
constexpr std::size_t most_rate_limit_headers = 16;

std::string lowercase(std::string_view const text) {
    std::string out{text};
    std::ranges::transform(out, out.begin(), [](unsigned char const c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

struct planned {
    std::size_t index = 0;
    std::optional<std::string> upstream{};
};

struct planned_route {
    std::vector<planned> steps{};
    std::string source{};
};


std::pair<std::string, json::value> provider_member(std::string_view const name, provider_stats const& counts, selection::circuit const state) {
    return {std::string{name}, json::to_value(provider_report{.counts = counts, .circuit = state})};
}

std::string stats_json(stats const& counters, std::string_view const usage_json, json::object providers) {
    auto document = json::to_value(counters);
    auto& members = std::get<json::object>(document.data);
    members.emplace_back("providers", json::value{std::move(providers)});
    members.emplace_back("usage", json::parse(usage_json).value_or(json::value{}));
    return json::write(document);
}

std::optional<std::string> alias_of(std::map<std::string, std::string> const& models, std::string const& requested) {
    auto const mapped = models.find(requested);
    return mapped == models.end() ? std::nullopt : std::optional<std::string>{mapped->second};
}

std::string upstream_name_of(std::map<std::string, std::string> const& models, std::optional<std::string> const& pinned, std::string const& requested) {
    return pinned ? *pinned : alias_of(models, requested).value_or(requested);
}

struct request_note {
    std::string requested{};
    std::string session{};
    std::chrono::steady_clock::time_point began{};
    std::string kind{};
    httpio::request const* request = nullptr;
};

struct outcome_text {
    std::string stop_reason{};
    std::string error{};
    std::string verdict{};
};

constexpr int http_success_floor = 200;
constexpr int http_success_span = 100;
constexpr int http_error_floor = 400;
constexpr int too_many_requests = 429;

struct relay_state {
    stats* counters;
    std::size_t buffer_limit;
    std::optional<http::message_head> head{};
    bool head_sent = false;
    bool streaming = false;
    bool complete = false;
    std::string buffered{};
    std::map<std::string, std::string> rate_limits{};
    bool buffer_overflow = false;
    sse::parser parser{};
    anthropic::stream_guard guard{};
    std::optional<responses::stream_translator> translator{};
    bool capture = false;
    std::string raw{};
    std::deque<std::string> pending{};
    usage::observer observed{};
    std::uint64_t repairs_seen = 0;

    void emit(anthropic::output_event const& item) {
        observed.on_event(item.name, item.data);
        std::string frame;
        sse::write_event(frame, item.name, item.data);
        pending.push_back(std::move(frame));
    }

    void note(anthropic::repair const& item) {
        ++repair_slot(counters->repairs, item.kind);
        ++repairs_seen;
        if (!item.detail.empty()) {
            counters->repair_examples.try_emplace(std::string{reflect::enum_name(item.kind)}, item.detail.substr(0, repair_example_chars));
        }
    }

    void accept(sse::event const& item) {
        auto const guarded = [this](sse::event const& event) { guard.on_event(event, [this](auto const& out) { emit(out); }, [this](auto const& fix) { note(fix); }); };
        if (translator) {
            translator->on_event(item, guarded);
        } else {
            guarded(item);
        }
    }

    void on_chunk(std::span<std::byte const> bytes) {
        if (capture && streaming && raw.size() < capture::body_limit_bytes) {
            raw.append(reinterpret_cast<char const*>(bytes.data()), bytes.size());
        }
        if (streaming) {
            parser.feed(bytes, [this](sse::event const& item) { accept(item); });
            return;
        }
        if (buffer_limit < buffered.size() + bytes.size()) {
            buffer_overflow = true;
            return;
        }
        buffered.append(reinterpret_cast<char const*>(bytes.data()), bytes.size());
    }

    void on_head(http::message_head const& incoming) {
        head = incoming;
        for (auto const& field : incoming.fields.all()) {
            auto const name = lowercase(field.name);
            if (name.starts_with(rate_limit_prefix) && rate_limits.size() < most_rate_limit_headers) {
                rate_limits[name] = usage::label(field.value);
            }
        }
        auto const type = incoming.fields.find("content-type").value_or("");
        auto const success = incoming.status >= http_success_floor && incoming.status < http_success_floor + http_success_span;
        streaming = success && (type.starts_with("text/event-stream") || (translator && type.empty()));
    }

    bool unusable_success() const {
        return head.has_value() && head->status >= http_success_floor && head->status < http_success_floor + http_success_span && !streaming && !buffer_overflow &&
               !routing::is_message_json(buffered);
    }

    void finish_stream() {
        parser.finish([this](sse::event const& item) { accept(item); });
        guard.on_end([this](auto const& out) { emit(out); }, [this](auto const& fix) { note(fix); });
    }

    void operator()(http::event const& item) {
        if (auto const* start = std::get_if<http::message_head>(&item)) {
            on_head(*start);
        } else if (auto const* chunk = std::get_if<http::body_chunk>(&item)) {
            on_chunk(chunk->bytes);
        } else {
            complete = true;
        }
    }
};

enum class health { healthy, failing, neutral };

struct held_reply {
    int status = 0;
    http::header_map fields{};
    std::string body{};
};

struct attempt_result {
    bool delivered = false;
    net::result_void client{};
    std::optional<held_reply> held{};
    health state = health::healthy;
};

struct pump_outcome {
    std::error_code failure{};
    net::result_void client{};
};

constexpr bool client_gone(pump_outcome const& drained) noexcept {
    return drained.failure == std::errc::connection_aborted;
}


constexpr std::size_t request_id_bytes = 12;

std::string new_request_id() {
    static std::atomic<std::uint64_t> sequence{0};
    auto const random = proof::random_hex(request_id_bytes);
    return random ? "req_" + *random : std::format("req_llmgate_{}", ++sequence);
}

attempt_result refused(routing::reply_body problem, health const state) {
    auto const id = new_request_id();
    held_reply reply{.status = problem.status, .fields = {}, .body = routing::with_request_id(problem.body, id)};
    reply.fields.add("Content-Type", "application/json");
    reply.fields.add("request-id", id);
    return {.held = std::move(reply), .state = state};
}

constexpr std::size_t cache_key_chars = 32;
constexpr std::string_view cache_key_label = "llmgate-prompt-cache";

std::string prompt_cache_key(httpio::request const& incoming) {
    auto const session = incoming.head.fields.find("x-claude-code-session-id");
    return session ? proof::hmac_sha256_hex(cache_key_label, *session).substr(0, cache_key_chars) : std::string{};
}

std::expected<std::string, attempt_result> to_responses_body(std::string const& body, httpio::request const& incoming, stats& counters) {
    auto done = responses::translate_request(body, {.prompt_cache_key = prompt_cache_key(incoming)});
    if (!done) {
        return std::unexpected{refused(routing::error_reply(400, "invalid_request_error", done.error().message), health::neutral)};
    }
    if (!done->client_streams) {
        return std::unexpected{refused(routing::error_reply(400, "invalid_request_error", "stream must be true for a provider with the openai_responses protocol"), health::neutral)};
    }
    for (auto const& name : done->dropped) {
        ++counters.dropped_fields[name];
    }
    return std::move(done->body);
}

bool translates_errors(wire_protocol const wire, int const status) {
    return wire == wire_protocol::openai_responses && status >= http_error_floor;
}

std::string client_error_body(wire_protocol const wire, int const status, std::string body) {
    return translates_errors(wire, status) ? responses::error_body(status, body) : std::move(body);
}

attempt_result delivered(net::result_void client, health const state) {
    return {.delivered = true, .client = std::move(client), .state = state};
}

constexpr bool retryable_status(int const status) noexcept {
    return status == 429 || (status >= 500 && status < 600);
}

bool retryable(relay_state const& state, std::error_code const failure) {
    if (state.head_sent) {
        return false;
    }
    if (failure) {
        return !state.head.has_value();
    }
    return state.head.has_value() && !state.buffer_overflow && (retryable_status(state.head->status) || state.unusable_success());
}

health health_of(relay_state const& state, std::error_code const failure) {
    return failure || state.buffer_overflow || state.unusable_success() ? health::failing : health::healthy;
}

std::optional<routing::endpoint_url> parsed_url(std::string_view const url) {
    if (auto parsed = routing::parse_url(url)) {
        return std::move(*parsed);
    }
    return std::nullopt;
}

struct provider_runtime {
    std::string name;
    provider_config settings;
    credential* creds;
    std::optional<routing::endpoint_url> url;
    selection::breaker circuit{};
};

std::vector<control::offer> offers_of(std::span<provider_runtime const> const providers) {
    std::vector<control::offer> out;
    for (auto const& item : providers) {
        control::offer one{.provider = item.name};
        for (auto const& entry : item.settings.models) {
            one.models.push_back(entry.second);
        }
        one.models.append_range(item.settings.offer);
        out.push_back(std::move(one));
    }
    return out;
}

}  // namespace llmgate::gateway::detail

export namespace llmgate::gateway {

class proxy {
public:
    proxy(async::reactor& loop, config settings, access_policy access, credential_map credentials, tls::client_context const& tls_context)
        : loop_{&loop}, settings_{std::move(settings)}, access_{std::move(access)}, tls_context_{&tls_context},
          breaker_{.failures = settings_.fallback.failures, .cooldown_ms = settings_.fallback.cooldown.count()} {
        auto declared = effective_providers(settings_);
        routes_ = role_routes(settings_);
        declared_ = provider_names(declared);
        for (auto& [name, item] : declared) {
            auto const found = credentials.find(name);
            auto url = detail::parsed_url(item.base_url);
            providers_.push_back({.name = name, .settings = std::move(item), .creds = found == credentials.end() ? &missing_ : found->second, .url = std::move(url)});
            counters_.providers[name];
        }
        targets_ = control::catalogue(detail::offers_of(providers_));
    }

    proxy(async::reactor& loop, config settings, access_policy access, credential& credentials, tls::client_context const& tls_context)
        : proxy{loop, std::move(settings), std::move(access), single(credentials), tls_context} {}

    void use_overrides(control::overrides book) { overrides_ = std::move(book); }
    void capture_with(std::function<void(std::string const&)> sink) { capture_ = std::move(sink); }
    void persist_overrides_with(std::function<void(control::overrides const&)> sink) { persist_overrides_ = std::move(sink); }

    bool uses_credential_endpoint() const {
        return std::ranges::any_of(providers_, [](detail::provider_runtime const& item) { return item.creds->endpoint().has_value(); });
    }

    stats const& counters() const noexcept { return counters_; }
    usage::ledger const& ledger() const noexcept { return ledger_; }
    void use_ledger(usage::ledger book) { ledger_ = std::move(book); }
    void persist_usage_with(std::function<void(std::string const&)> sink) { persist_ = std::move(sink); }
    void use_clock(std::function<std::int64_t()> clock) { clock_ = std::move(clock); }
    void use_breaker_clock(std::function<std::int64_t()> clock) { breaker_clock_ = std::move(clock); }

    bool configured() const { return !providers_.empty() && usable(); }

    template <net::byte_stream Client>
    async::task<net::result_void> operator()(httpio::request const& incoming, httpio::response_writer<Client>& out) {
        ++counters_.requests;
        routing::count_betas(counters_.betas, incoming.head.fields.find("anthropic-beta").value_or(""));
        auto const which = routing::route_of(incoming.head.method, incoming.head.target);
        if (auto refusal = check_access(incoming, which)) {
            ++counters_.refused;
            co_return co_await reject(out, std::move(*refusal));
        }
        if (which == routing::route::messages) {
            co_return co_await forward(incoming, out);
        }
        if (which == routing::route::count_tokens) {
            co_return co_await estimate_tokens(incoming, out);
        }
        if (which == routing::route::control) {
            auto answer = apply_control(incoming);
            co_return co_await (answer.status >= detail::http_error_floor ? reject(out, std::move(answer)) : respond(out, answer.status, std::move(answer.body)));
        }
        if (auto body = json_body(which, incoming.head.target)) {
            co_return co_await respond(out, 200, std::move(*body));
        }
        co_return co_await reject(out, failure_for(which));
    }

private:
    bool usable() const {
        return std::ranges::all_of(providers_, [](detail::provider_runtime const& item) { return item.url.has_value(); }) &&
               selection::route_problem(routes_, declared_).empty();
    }

    std::optional<std::string> json_body(routing::route const which, std::string_view const target) const {
        switch (which) {
            case routing::route::models: return detail::models_json(alias_names(), targets_, [this](control::target const& item) { return limits_of(item); });
            case routing::route::health: return health_body(target);
            case routing::route::route_view: return route_body(target);
            case routing::route::overview: return overview_body(target);
            case routing::route::session_view: return session_body(target);
            case routing::route::statistics: return detail::stats_json(counters_, usage::summary_json(ledger_, settings_.usage.budget, clock_()), providers_object());
            default: return std::nullopt;
        }
    }

    static routing::reply_body failure_for(routing::route const which) {
        return which == routing::route::wrong_method ? routing::error_reply(405, "invalid_request_error", "method not allowed")
                                                     : routing::error_reply(404, "not_found_error", "unknown path");
    }

    static credential_map single(credential& only) {
        credential_map all;
        all.emplace(std::string{implicit_provider}, &only);
        return all;
    }

    std::set<std::string> alias_names() const {
        std::set<std::string> aliases;
        for (auto const& item : providers_) {
            for (auto const& entry : item.settings.models) {
                aliases.insert(entry.first);
            }
        }
        return aliases;
    }

    json::object providers_object() const {
        json::object items;
        for (auto const& item : providers_) {
            items.push_back(detail::provider_member(item.name, counters_.providers.at(item.name), selection::state_of(item.circuit, breaker_, breaker_clock_())));
        }
        return items;
    }

    routing::reply_body apply_control(httpio::request const& incoming) {
        if (access_.identity_key.empty()) {
            return routing::error_reply(403, "permission_error", "the control endpoint needs an identity key");
        }
        auto const sent = incoming.head.fields.find("x-llmgate-proof").value_or("");
        auto const expected = proof::hmac_sha256_hex(access_.identity_key, proof::identity_message(std::format("{}/control", access_.identity_scope), incoming.body));
        if (!routing::secrets_equal(sent, expected)) {
            return routing::error_reply(401, "authentication_error", "invalid or missing control proof");
        }
        auto changed = overrides_;
        if (auto problem = control::apply(changed, incoming.body, targets_); !problem.empty()) {
            return routing::error_reply(400, "invalid_request_error", problem);
        }
        overrides_ = std::move(changed);
        if (persist_overrides_) {
            persist_overrides_(overrides_);
        }
        return {.status = 200, .body = R"({"status":"ok"})"};
    }

    std::string session_body(std::string_view const target) const {
        auto const found = served_.items.find(routing::query_value(target, "id"));
        return found == served_.items.end() ? std::string{"{}"} : json::write(json::to_value(found->second));
    }

    detail::route_report route_for(std::string_view const requested, std::string const& role, std::string_view const session) const {
        auto const chosen = control::resolve({.requested = requested, .role = role, .session = session}, targets_, overrides_, selection::order_for(routes_, role, declared_));
        detail::route_report report{.role = role, .via = std::string{reflect::enum_name(chosen.source)}};
        for (auto const& item : chosen.steps) {
            report.providers.push_back(item.provider);
            report.upstream = item.upstream;
        }
        return report;
    }

    std::string route_body(std::string_view const target) const {
        auto const requested = routing::query_value(target, "requested");
        auto const role = std::string{roles::name_of(roles::of(requested.empty() ? routing::query_value(target, "role") : requested))};
        return json::write(json::to_value(route_for(requested, role, routing::query_value(target, "session"))));
    }

    std::string overview_body(std::string_view const target) const {
        auto const session = routing::query_value(target, "session");
        detail::overview_document document;
        for (auto const role : reflect::enum_values<roles::role>) {
            auto const name = std::string{roles::name_of(role)};
            document.roles.push_back(route_for(name, name, session));
        }
        if (auto const found = served_.items.find(session); found != served_.items.end()) {
            document.served = found->second;
        }
        for (auto const& item : targets_) {
            auto const limits = limits_of(item);
            document.targets.push_back({.id = item.id,
                                        .provider = item.provider,
                                        .upstream = item.upstream,
                                        .label = item.display_name,
                                        .max_input_tokens = detail::known_limit(limits.prompt_tokens),
                                        .max_tokens = detail::known_limit(limits.output_tokens)});
        }
        for (auto const& item : providers_) {
            auto const seen = rate_limits_.find(item.name);
            document.providers.push_back({.name = item.name,
                                          .counts = counters_.providers.at(item.name),
                                          .circuit = selection::state_of(item.circuit, breaker_, breaker_clock_()),
                                          .rate_limits = seen == rate_limits_.end() ? std::map<std::string, std::string>{} : seen->second});
        }
        auto const today = usage::day_of(clock_());
        document.spend = {.today = usage::spend_between(ledger_, today, today, settings_.pricing),
                          .month = usage::spend_between(ledger_, usage::first_day_of_month(today), today, settings_.pricing)};
        document.totals = usage::summary_of(ledger_, settings_.usage.budget, clock_());
        return json::write(json::to_value(document));
    }

    std::string health_body(std::string_view const target) const {
        auto const challenge = routing::query_value(target, "challenge");
        if (access_.identity_key.empty() || !routing::valid_challenge(challenge)) {
            return R"({"status":"ok"})";
        }
        return std::format(R"({{"status":"ok","proof":"{}"}})", proof::hmac_sha256_hex(access_.identity_key, proof::identity_message(access_.identity_scope, challenge)));
    }

    std::optional<routing::reply_body> check_access(httpio::request const& incoming, routing::route const which) const {
        auto const host = incoming.head.fields.find("host");
        if (!host || !routing::host_allowed(*host, access_.allowed_hosts)) {
            return routing::error_reply(403, "permission_error", "unexpected Host header");
        }
        if (access_.client_key.empty() || which == routing::route::health) {
            return std::nullopt;
        }
        auto const header_key = incoming.head.fields.find("x-api-key").value_or("");
        auto const bearer_key = routing::bearer_value(incoming.head.fields.find("authorization").value_or(""));
        if (routing::secrets_equal(header_key, access_.client_key) || routing::secrets_equal(bearer_key, access_.client_key)) {
            return std::nullopt;
        }
        return routing::error_reply(401, "authentication_error", "invalid or missing client key");
    }

    template <net::byte_stream Client>
    static async::task<net::result_void> respond(httpio::response_writer<Client>& out, int const status, std::string body) {
        http::header_map fields;
        fields.add("Content-Type", "application/json");
        co_return co_await out.send(status, fields, body);
    }

    template <net::byte_stream Client>
    static async::task<net::result_void> reject(httpio::response_writer<Client>& out, routing::reply_body problem) {
        auto const id = detail::new_request_id();
        http::header_map fields;
        fields.add("Content-Type", "application/json");
        fields.add("request-id", id);
        co_return co_await out.send(problem.status, fields, routing::with_request_id(problem.body, id));
    }

    template <net::byte_stream Client>
    async::task<net::result_void> estimate_tokens(httpio::request const& incoming, httpio::response_writer<Client>& out) {
        constexpr std::size_t bytes_per_token = 4;
        auto const tokens = (incoming.body.size() + bytes_per_token - 1) / bytes_per_token;
        http::header_map fields;
        fields.add("Content-Type", "application/json");
        fields.add("X-Llmgate-Estimated", "true");
        co_return co_await out.send(200, fields, std::format(R"({{"input_tokens":{}}})", tokens));
    }

    async::task<std::expected<net::any_stream, std::error_code>> connect_upstream(routing::endpoint_url target) {
        auto const key = net::endpoint{.host = target.host, .port = target.port};
        if (auto reused = pool_.take(key)) {
            ++counters_.connections_reused;
            co_return std::move(*reused);
        }
        ++counters_.connections_opened;
        if (target.secure) {
            auto stream = co_await net::tls_stream::connect(*loop_, *tls_context_, target.host, target.port, settings_.time.connect);
            if (!stream) {
                co_return std::unexpected{stream.error()};
            }
            co_return net::any_stream{std::move(*stream)};
        }
        auto stream = co_await net::tcp_stream::connect(*loop_, target.host, target.port, settings_.time.connect);
        if (!stream) {
            co_return std::unexpected{stream.error()};
        }
        co_return net::any_stream{std::move(*stream)};
    }

    template <net::byte_stream Client>
    async::task<net::result_void> forward(httpio::request const& incoming, httpio::response_writer<Client>& out) {
        auto first = routing::prepare(incoming.body, [](std::string const&) -> std::optional<std::string> { return std::nullopt; });
        if (!first) {
            co_return co_await reject(out, std::move(first.error()));
        }
        if (auto const now = verdict(); now.state == usage::level::exhausted) {
            ++counters_.budget_refused;
            co_return co_await reject(out, routing::error_reply(429, "rate_limit_error", usage::refusal_message(now)));
        }
        detail::request_note note{.requested = std::move(first->requested), .session = usage::session_of(incoming.body), .began = std::chrono::steady_clock::now(), .kind = usage::kind_of(incoming.body), .request = &incoming};
        auto order = plan_for(incoming, note);
        note.session = session_for(incoming, note);
        co_return co_await run_providers(incoming, out, std::move(order), std::move(note));
    }

    std::string session_for(httpio::request const& incoming, detail::request_note const& note) const {
        auto const header = incoming.head.fields.find("x-claude-code-session-id");
        return header ? std::string{*header} : note.session;
    }

    detail::planned_route plan_for(httpio::request const& incoming, detail::request_note const& note) const {
        auto const role = usage::role_of(note.requested);
        auto const configured = selection::order_for(routes_, role, declared_);
        auto const chosen = control::resolve({.requested = note.requested, .role = role, .session = session_for(incoming, note)}, targets_, overrides_, configured);
        detail::planned_route out{.source = std::string{reflect::enum_name(chosen.source)}};
        for (auto const& item : chosen.steps) {
            out.steps.push_back({.index = index_of(item.provider), .upstream = item.upstream});
        }
        return out;
    }

    std::size_t index_of(std::string_view const name) const {
        auto const found = std::ranges::find(providers_, name, &detail::provider_runtime::name);
        return static_cast<std::size_t>(found - providers_.begin());
    }

    template <net::byte_stream Client>
    async::task<net::result_void> run_providers(httpio::request const& incoming, httpio::response_writer<Client>& out, detail::planned_route order,
                                                detail::request_note note) {
        std::optional<detail::held_reply> held;
        std::size_t last = 0;
        for (auto const& step : order.steps) {
            auto const index = step.index;
            if (!admit(index)) {
                continue;
            }
            auto result = co_await attempt(index, step.upstream, incoming, out, note);
            settle(index, result.state);
            if (result.delivered) {
                if (result.state == detail::health::healthy) {
                    remember_served(note, order, step);
                }
                co_return result.client;
            }
            held = std::move(result.held);
            last = index;
        }
        co_return co_await deliver_failure(out, std::move(held), last, std::move(note));
    }

    void remember_served(detail::request_note const& note, detail::planned_route const& order, detail::planned const& step) {
        auto const& chosen = providers_[step.index];
        auto const upstream = detail::upstream_name_of(chosen.settings.models, step.upstream, note.requested);
        control::record(served_, note.session,
                        {.provider = chosen.name, .upstream = upstream, .source = order.source, .fallback = step.index != order.steps.front().index});
    }

    bool admit(std::size_t const index) {
        auto& chosen = providers_[index];
        auto& counts = counters_.providers[chosen.name];
        if (!selection::try_acquire(chosen.circuit, breaker_, breaker_clock_())) {
            ++counts.skipped;
            return false;
        }
        ++counts.attempts;
        return true;
    }

    void settle(std::size_t const index, detail::health const outcome) {
        auto& chosen = providers_[index];
        auto& counts = counters_.providers[chosen.name];
        switch (outcome) {
            case detail::health::healthy:
                selection::on_success(chosen.circuit);
                ++counts.answered;
                break;
            case detail::health::failing:
                selection::on_failure(chosen.circuit, breaker_, breaker_clock_());
                ++counts.failures;
                break;
            case detail::health::neutral: selection::on_release(chosen.circuit); break;
        }
    }

    template <net::byte_stream Client>
    async::task<net::result_void> deliver_failure(httpio::response_writer<Client>& out, std::optional<detail::held_reply> held, std::size_t const last,
                                                  detail::request_note note) {
        if (!held) {
            ++counters_.no_provider;
            co_return co_await reject(out, routing::error_reply(503, "api_error", "every provider for this model is unavailable (circuit open)"));
        }
        capture_exchange(note, providers_[last].name, held->status, {}, held->fields, held->body);
        record_entry(note, providers_[last].name, held->status, {}, {}, 0, {.error = usage::error_of_body(held->body)});
        co_return co_await out.send(held->status, held->fields, held->body);
    }

    template <net::byte_stream Client>
    async::task<detail::attempt_result> attempt(std::size_t const index, std::optional<std::string> upstream_model, httpio::request const& incoming,
                                                httpio::response_writer<Client>& out, detail::request_note note) {
        auto spec = co_await build_spec(index, upstream_model, incoming, note.session);
        if (!spec) {
            co_return std::move(spec.error());
        }
        auto upstream = co_await connect_upstream(*providers_[index].url);
        if (!upstream) {
            co_return unreachable(upstream.error());
        }
        if (auto refusal = co_await send_request(*upstream, std::move(*spec))) {
            co_return std::move(*refusal);
        }
        co_return co_await read_reply(index, *upstream, out, std::move(note));
    }

    detail::attempt_result unreachable(std::error_code const failure) {
        ++counters_.upstream_failures;
        auto const status = failure == std::errc::timed_out ? gateway_timeout_status : bad_gateway_status;
        return detail::refused(routing::error_reply(status, "api_error", "cannot reach the upstream: " + failure.message()), detail::health::failing);
    }

    std::string history_for(detail::provider_runtime const& chosen, std::string_view const session, std::string body) {
        auto const last = session.empty() ? served_.items.end() : served_.items.find(session);
        if (last == served_.items.end() || last->second.provider == chosen.name) {
            return body;
        }
        auto done = history::without_thinking(body);
        if (!done) {
            return body;
        }
        counters_.thinking_blocks_stripped += done->removed;
        return std::move(done->body);
    }

    std::string clamp_output(detail::provider_runtime const& chosen, std::string const& upstream_name, std::string body) {
        auto const found = chosen.settings.limits.find(upstream_name);
        if (found == chosen.settings.limits.end()) {
            return body;
        }
        auto clamped = routing::clamp_max_tokens(body, found->second.output_tokens);
        counters_.max_tokens_clamped += clamped.changed ? 1U : 0U;
        return std::move(clamped.body);
    }

    model_limits limits_of(control::target const& item) const {
        auto const provider = std::ranges::find(providers_, item.provider, &detail::provider_runtime::name);
        if (provider == providers_.end()) {
            return {};
        }
        auto const found = provider->settings.limits.find(item.upstream);
        return found == provider->settings.limits.end() ? model_limits{} : found->second;
    }

    std::expected<std::string, detail::attempt_result> upstream_body(detail::provider_runtime const& chosen, httpio::request const& incoming, std::string body) {
        if (chosen.settings.protocol != wire_protocol::openai_responses) {
            return body;
        }
        return detail::to_responses_body(body, incoming, counters_);
    }

    async::task<std::expected<std::string, detail::attempt_result>> upstream_token(std::size_t const index) {
        auto& chosen = providers_[index];
        if (chosen.settings.forward_credentials) {
            co_return std::string{};
        }
        auto bearer = co_await chosen.creds->bearer();
        if (!bearer) {
            co_return std::unexpected{detail::refused(
                routing::error_reply(401, "authentication_error", "no upstream credential: run llmgate login (" + bearer.error().message() + ")"), detail::health::neutral)};
        }
        co_return std::move(*bearer);
    }

    async::task<std::expected<httpio::request_spec, detail::attempt_result>> build_spec(std::size_t const index, std::optional<std::string> upstream_model, httpio::request const& incoming,
                                                                                                  std::string session) {
        auto& chosen = providers_[index];
        if (!chosen.url) {
            co_return std::unexpected{detail::refused(routing::error_reply(500, "api_error", "the upstream base_url is not valid"), detail::health::neutral)};
        }
        auto prepared = routing::prepare(incoming.body, [&chosen, &upstream_model](std::string const& requested) { return upstream_model ? upstream_model : detail::alias_of(chosen.settings.models, requested); });
        if (!prepared) {
            co_return std::unexpected{detail::refused(std::move(prepared.error()), detail::health::neutral)};
        }
        auto const upstream_name = detail::upstream_name_of(chosen.settings.models, upstream_model, prepared->requested);
        auto body = upstream_body(chosen, incoming, history_for(chosen, session, clamp_output(chosen, upstream_name, std::move(prepared->body))));
        if (!body) {
            co_return std::unexpected{std::move(body.error())};
        }
        auto token = co_await upstream_token(index);
        if (!token) {
            co_return std::unexpected{std::move(token.error())};
        }
        if (auto const dynamic = chosen.creds->endpoint()) {
            if (auto parsed = detail::parsed_url(*dynamic)) {
                chosen.url = std::move(parsed);
            }
        }
        co_return httpio::request_spec{.method = "POST",
                                       .target = chosen.url->base_path + effective_messages_path(chosen.settings.protocol, chosen.settings.messages_path),
                                       .host = routing::host_header(*chosen.url),
                                       .fields = detail::upstream_headers(incoming, chosen.settings.headers, chosen.settings.copilot.has_value(), chosen.settings.forward_credentials, *token,
                                                                          chosen.settings.protocol),
                                       .body = std::move(*body)};
    }

    async::task<std::optional<detail::attempt_result>> send_request(net::any_stream& upstream, httpio::request_spec spec) {
        auto const wire = httpio::format_request(spec);
        if (!wire) {
            co_return detail::refused(routing::error_reply(502, "api_error", "cannot format the upstream request"), detail::health::neutral);
        }
        if (auto const sent = co_await upstream.write_all(std::as_bytes(std::span{*wire})); !sent) {
            ++counters_.upstream_failures;
            co_return detail::refused(routing::error_reply(502, "api_error", "upstream write failed: " + sent.error().message()), detail::health::failing);
        }
        co_return std::optional<detail::attempt_result>{};
    }

    usage::verdict verdict() const {
        auto const day = usage::day_of(clock_());
        return usage::evaluate(settings_.usage.budget, ledger_.on_day(day).used.counted(), ledger_.in_month(usage::month_of(day)).used.counted());
    }

    static int final_status(detail::relay_state const& state, std::error_code const failure) {
        if (failure == std::errc::connection_aborted) {
            return client_closed_status;
        }
        if (state.head && !failure) {
            return state.head->status;
        }
        return failure == std::errc::timed_out ? gateway_timeout_status : bad_gateway_status;
    }

    void record_usage(detail::request_note const& note, detail::relay_state& state, std::error_code const failure, std::string_view const provider) {
        if (!state.streaming && state.head && !failure && state.head->status >= 200 && state.head->status < 300) {
            state.observed.on_body(state.buffered);
        }
        auto const status = final_status(state, failure);
        detail::outcome_text outcome{.stop_reason = state.observed.stop_reason(), .error = reply_error(state, status), .verdict = classifier_verdict(note, state, provider, status)};
        capture_reply(note, provider, status, state, outcome.verdict);
        record_entry(note, provider, status, state.observed.model(), state.observed.used(), state.repairs_seen, std::move(outcome));
    }

    void capture_exchange(detail::request_note const& note, std::string_view const provider, int const status, std::string_view const verdict,
                          http::header_map const& reply_headers, std::string_view const reply_body) {
        if (!capture_ || note.request == nullptr) {
            return;
        }
        capture::exchange seen{.time = clock_(), .session = note.session, .provider = std::string{provider}, .requested = note.requested, .kind = note.kind,
                               .verdict = std::string{verdict}, .status = status, .request_headers = capture::redacted_headers(note.request->head.fields),
                               .reply_headers = capture::redacted_headers(reply_headers)};
        seen.request_body = capture::bounded(note.request->body, capture::body_limit_bytes, seen.request_truncated);
        seen.reply_body = capture::bounded(reply_body, capture::body_limit_bytes, seen.reply_truncated);
        capture_(capture::line_of(seen));
    }

    void capture_reply(detail::request_note const& note, std::string_view const provider, int const status, detail::relay_state const& state, std::string_view const verdict) {
        if (state.head) {
            capture_exchange(note, provider, status, verdict, state.head->fields, state.streaming ? std::string_view{state.raw} : std::string_view{state.buffered});
        }
    }

    static std::string reply_error(detail::relay_state const& state, int const status) {
        auto error = state.observed.error();
        return error.empty() && status >= detail::http_error_floor && !state.streaming ? usage::error_of_body(state.buffered) : error;
    }

    std::string classifier_verdict(detail::request_note const& note, detail::relay_state const& state, std::string_view const provider, int const status) {
        if (note.kind != usage::classifier_kind || status < detail::http_success_floor || status >= detail::http_success_floor + detail::http_success_span) {
            return {};
        }
        auto const found = usage::verdict_of(state.observed.text());
        auto const key = std::format("{}/{}", provider, state.observed.model().empty() ? usage::label(note.requested) : state.observed.model());
        ++counters_.classifier_requests[key];
        if (found == usage::reply_verdict::unparseable) {
            ++counters_.classifier_unparseable[key];
        }
        return std::string{reflect::enum_name(found)};
    }

    void record_entry(detail::request_note const& note, std::string_view const provider, int const status, std::string_view const model,
                      usage::tokens const& used, std::uint64_t const repairs, detail::outcome_text outcome) {
        auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - note.began);
        usage::record entry{.time = clock_(),
                            .requested = usage::label(note.requested),
                            .model = usage::label(model.empty() ? std::string_view{note.requested} : model),
                            .role = std::string{usage::role_of(note.requested)},
                            .provider = usage::label(provider),
                            .session = note.session,
                            .used = used,
                            .duration_ms = elapsed.count(),
                            .status = status,
                            .repairs = repairs,
                            .stop_reason = std::move(outcome.stop_reason),
                            .error = std::move(outcome.error),
                            .kind = note.kind,
                            .verdict = std::move(outcome.verdict)};
        ledger_.add(entry);
        if (persist_) {
            persist_(usage::to_line(entry));
        }
    }

    net::endpoint endpoint_key(std::size_t const index) const {
        auto const& target = *providers_[index].url;
        return net::endpoint{.host = target.host, .port = target.port};
    }

    template <net::byte_stream Client>
    async::task<net::result_void> forward_to_client(detail::relay_state& state, httpio::response_writer<Client>& out) {
        if (state.head && !state.head_sent && state.streaming) {
            state.head_sent = true;
            ++counters_.streamed;
            if (auto const begun = co_await out.begin_stream(state.head->status, stream_headers(*state.head)); !begun) {
                co_return begun;
            }
        }
        for (auto const& frame : std::exchange(state.pending, {})) {
            if (auto const written = co_await out.write_chunk(std::as_bytes(std::span{frame})); !written) {
                co_return written;
            }
        }
        co_return net::result_void{};
    }

    template <net::byte_stream Client>
    async::task<detail::pump_outcome> pump(net::any_stream& upstream, detail::relay_state& state, httpio::response_writer<Client>& out) {
        httpio::message_reader<net::any_stream> reader{upstream, http::kind::response, httpio::limits{}};
        while (!state.complete) {
            auto const idle = state.head.has_value() ? settings_.time.stream_idle : settings_.time.response_head;
            auto const step = co_await reader.step(state, {.idle = idle, .watch = out.descriptor()});
            if (auto const sent = co_await forward_to_client(state, out); !sent) {
                co_return detail::pump_outcome{.failure = std::make_error_code(std::errc::connection_aborted), .client = sent};
            }
            if (!step) {
                co_return detail::pump_outcome{.failure = step.error(), .client = net::result_void{}};
            }
        }
        co_return detail::pump_outcome{};
    }

    template <net::byte_stream Client>
    async::task<detail::attempt_result> read_reply(std::size_t const index, net::any_stream& upstream, httpio::response_writer<Client>& out,
                                                   detail::request_note note) {
        detail::relay_state state{.counters = &counters_, .buffer_limit = buffer_limit};
        state.observed.collect_text(note.kind == usage::classifier_kind);
        state.capture = static_cast<bool>(capture_);
        if (providers_[index].settings.protocol == wire_protocol::openai_responses) {
            state.translator.emplace(note.requested);
        }
        auto const drained = co_await pump(upstream, state, out);
        absorb_translator_counts(state);
        auto const& provider = providers_[index].name;
        if (!state.rate_limits.empty()) {
            rate_limits_[provider] = state.rate_limits;
        }
        if (detail::client_gone(drained)) {
            ++counters_.client_disconnects;
        }
        if (!drained.client) {
            record_usage(note, state, drained.failure, provider);
            co_return detail::delivered(drained.client, detail::health::neutral);
        }
        if (detail::client_gone(drained)) {
            record_usage(note, state, drained.failure, provider);
            co_return detail::delivered(net::result_void{}, detail::health::neutral);
        }
        if (detail::retryable(state, drained.failure)) {
            co_return hold(index, upstream, state, drained.failure);
        }
        auto concluded = co_await conclude(index, upstream, state, drained.failure, out);
        record_usage(note, state, drained.failure, provider);
        co_return detail::delivered(std::move(concluded), detail::health_of(state, drained.failure));
    }

    void absorb_translator_counts(detail::relay_state const& state) {
        if (state.translator) {
            for (auto const& [name, count] : state.translator->ignored()) {
                counters_.ignored_events[name] += count;
            }
        }
    }

    detail::attempt_result hold(std::size_t const index, net::any_stream& upstream, detail::relay_state& state, std::error_code const failure) {
        if (failure) {
            (failure == std::errc::timed_out ? counters_.upstream_timeouts : counters_.upstream_failures) += 1;
            auto const status = failure == std::errc::timed_out ? gateway_timeout_status : bad_gateway_status;
            return detail::refused(routing::error_reply(status, "api_error", "the upstream did not answer: " + failure.message()), detail::health::failing);
        }
        if (state.head->keep_alive) {
            pool_.give_back(endpoint_key(index), std::move(upstream));
        }
        if (state.unusable_success()) {
            ++counters_.upstream_failures;
            return detail::refused(routing::error_reply(502, "api_error", "the upstream answered 200 with a body that is not a Message"), detail::health::failing);
        }
        return {.held = detail::held_reply{.status = state.head->status,
                                           .fields = routing::downstream_headers(state.head->fields),
                                           .body = detail::client_error_body(providers_[index].settings.protocol, state.head->status, std::move(state.buffered))},
                .state = state.head->status == detail::too_many_requests ? detail::health::neutral : detail::health::failing};
    }

    static http::header_map stream_headers(http::message_head const& head) {
        auto fields = routing::downstream_headers(head.fields);
        return fields;
    }

    template <net::byte_stream Client>
    async::task<net::result_void> conclude(std::size_t const index, net::any_stream& upstream, detail::relay_state& state, std::error_code const failure,
                                           httpio::response_writer<Client>& out) {
        count_failure(failure);
        if (state.streaming && state.head_sent) {
            if (!failure && state.complete) {
                settle_connection(index, upstream, *state.head);
            }
            co_return co_await close_stream(state, out);
        }
        co_return co_await relay_buffered(index, upstream, state, failure, out);
    }

    void count_failure(std::error_code const failure) {
        if (failure) {
            (failure == std::errc::timed_out ? counters_.upstream_timeouts : counters_.upstream_failures) += 1;
        }
    }

    template <net::byte_stream Client>
    async::task<net::result_void> close_stream(detail::relay_state& state, httpio::response_writer<Client>& out) {
        state.finish_stream();
        for (auto const& frame : state.pending) {
            (void)co_await out.write_chunk(std::as_bytes(std::span{frame}));
        }
        co_return co_await out.end_stream();
    }

    template <net::byte_stream Client>
    async::task<net::result_void> relay_buffered(std::size_t const index, net::any_stream& upstream, detail::relay_state& state,
                                                 std::error_code const failure, httpio::response_writer<Client>& out) {
        if (failure) {
            auto const status = failure == std::errc::timed_out ? gateway_timeout_status : bad_gateway_status;
            co_return co_await reject(out, routing::error_reply(status, "api_error", "the upstream did not answer: " + failure.message()));
        }
        if (state.buffer_overflow) {
            co_return co_await reject(out, routing::error_reply(502, "api_error", "the upstream response is too large"));
        }
        settle_connection(index, upstream, *state.head);
        auto const fields = routing::downstream_headers(state.head->fields);
        if (detail::translates_errors(providers_[index].settings.protocol, state.head->status)) {
            co_return co_await out.send(state.head->status, fields, responses::error_body(state.head->status, state.buffered));
        }
        co_return co_await out.send(state.head->status, fields, state.buffered);
    }

    void settle_connection(std::size_t const index, net::any_stream& upstream, http::message_head const& head) {
        if (head.status == 401 || head.status == 403) {
            providers_[index].creds->rejected();
        }
        if (head.keep_alive) {
            pool_.give_back(endpoint_key(index), std::move(upstream));
        }
    }

    static constexpr std::size_t buffer_limit = 33554432;
    static constexpr int client_closed_status = 499;
    static constexpr int gateway_timeout_status = 504;
    static constexpr int bad_gateway_status = 502;

    async::reactor* loop_;
    config settings_;
    access_policy access_;
    tls::client_context const* tls_context_;
    selection::breaker_settings breaker_;
    static_credential missing_{std::string{}};
    std::vector<detail::provider_runtime> providers_;
    std::vector<selection::role_route> routes_;
    std::vector<control::target> targets_;
    control::overrides overrides_;
    control::registry served_;
    std::map<std::string, std::map<std::string, std::string>> rate_limits_;
    std::function<void(control::overrides const&)> persist_overrides_;
    std::function<void(std::string const&)> capture_;
    std::vector<std::string> declared_;
    net::connection_pool<net::any_stream> pool_;
    stats counters_;
    usage::ledger ledger_{};
    std::function<void(std::string const&)> persist_{};
    std::function<std::int64_t()> clock_{[] { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }};
    std::function<std::int64_t()> breaker_clock_{[] { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }};
};

}  // namespace llmgate::gateway

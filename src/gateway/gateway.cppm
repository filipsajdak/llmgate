export module llmgate.gateway;

import std;
import llmgate.anthropic;
import llmgate.async;
import llmgate.claude;
import llmgate.http;
import llmgate.httpio;
import llmgate.json;
import llmgate.jsonscan;
import llmgate.net;
import llmgate.reflect;
import llmgate.routing;
import llmgate.sse;
import llmgate.tls;

export namespace llmgate::gateway {

using std::chrono::milliseconds;

struct timeouts {
    milliseconds connect = std::chrono::seconds{10};
    milliseconds response_head = std::chrono::minutes{2};
    milliseconds stream_idle = std::chrono::minutes{10};
};

struct upstream_config {
    [[= json::required]] std::string base_url{};
    std::string messages_path = "/v1/messages";
    std::map<std::string, std::string> headers{};
};

constexpr std::string_view vscode_copilot_client_id = "Iv1.b507a08c87ecfe98";

struct copilot_config {
    std::string token_file{};
    std::string client_id = std::string{vscode_copilot_client_id};
    std::string github_url = "https://github.com";
    std::string api_url = "https://api.github.com";
    std::optional<milliseconds> poll_interval{};
};

struct config {
    [[= json::required]] std::string listen{};
    [[= json::required]] upstream_config upstream{};
    std::map<std::string, std::string> models{};
    timeouts time{};
    tls::group_policy tls_groups = tls::group_policy::post_quantum_preferred;
    std::optional<copilot_config> copilot{};
    std::string static_token_env{};
    std::string client_key_env{};
    std::vector<std::string> allowed_hosts{};
    claude::launch_config claude{};
};

struct access_policy {
    std::string client_key{};
    std::vector<std::string> allowed_hosts{};
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

struct stats {
    std::uint64_t requests = 0;
    std::uint64_t streamed = 0;
    std::uint64_t upstream_failures = 0;
    std::uint64_t upstream_timeouts = 0;
    std::uint64_t client_disconnects = 0;
    std::uint64_t refused = 0;
    std::uint64_t connections_opened = 0;
    std::uint64_t connections_reused = 0;
    std::map<anthropic::repair_kind, std::uint64_t> repairs;
};

}  // namespace llmgate::gateway

namespace llmgate::gateway::detail {

constexpr std::string_view user_agent = "llmgate/0.0";

http::header_map upstream_headers(httpio::request const& incoming, upstream_config const& settings, std::string_view const token) {
    static constexpr auto passed = std::to_array<std::string_view>({"content-type", "accept", "anthropic-version", "anthropic-beta"});
    http::header_map fields;
    for (auto const name : passed) {
        if (auto const found = incoming.head.fields.find(name)) {
            fields.add(std::string{name}, std::string{*found});
        }
    }
    for (auto const& [name, content] : settings.headers) {
        fields.add(name, content);
    }
    fields.add("Authorization", std::format("Bearer {}", token));
    fields.add("User-Agent", std::string{user_agent});
    return fields;
}

std::string models_json(std::map<std::string, std::string> const& models) {
    std::string items;
    for (auto const& [alias, target] : models) {
        if (!items.empty()) {
            items += ",";
        }
        std::format_to(std::back_inserter(items), R"({{"type":"model","id":{},"display_name":{},"created_at":"1970-01-01T00:00:00Z"}})",
                       jsonscan::encode_string(alias), jsonscan::encode_string(alias));
    }
    return std::format(R"({{"data":[{}],"has_more":false}})", items);
}

std::string stats_json(stats const& counters) {
    std::string repairs;
    for (auto const& [kind, count] : counters.repairs) {
        std::format_to(std::back_inserter(repairs), "{}\"{}\":{}", repairs.empty() ? "" : ",", reflect::enum_name(kind), count);
    }
    return std::format(
        R"({{"requests":{},"streamed":{},"upstream_failures":{},"upstream_timeouts":{},"client_disconnects":{},"refused":{},"connections_opened":{},"connections_reused":{},"repairs":{{{}}}}})",
        counters.requests, counters.streamed, counters.upstream_failures, counters.upstream_timeouts, counters.client_disconnects, counters.refused,
        counters.connections_opened, counters.connections_reused, repairs);
}

struct relay_state {
    stats* counters;
    std::size_t buffer_limit;
    std::optional<http::message_head> head{};
    bool head_sent = false;
    bool streaming = false;
    bool complete = false;
    std::string buffered{};
    bool buffer_overflow = false;
    sse::parser parser{};
    anthropic::stream_guard guard{};
    std::deque<std::string> pending{};

    void emit(anthropic::output_event const& item) {
        std::string frame;
        sse::write_event(frame, item.name, item.data);
        pending.push_back(std::move(frame));
    }

    void note(anthropic::repair const& item) { ++counters->repairs[item.kind]; }

    void on_chunk(std::span<std::byte const> bytes) {
        if (streaming) {
            parser.feed(bytes, [this](sse::event const& item) {
                guard.on_event(item, [this](auto const& out) { emit(out); }, [this](auto const& fix) { note(fix); });
            });
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
        auto const type = incoming.fields.find("content-type").value_or("");
        streaming = incoming.status >= 200 && incoming.status < 300 && type.starts_with("text/event-stream");
    }

    void finish_stream() {
        parser.finish([this](sse::event const& item) {
            guard.on_event(item, [this](auto const& out) { emit(out); }, [this](auto const& fix) { note(fix); });
        });
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

}  // namespace llmgate::gateway::detail

export namespace llmgate::gateway {

class proxy {
public:
    proxy(async::reactor& loop, config settings, access_policy access, credential& credentials, tls::client_context const& tls_context)
        : loop_{&loop}, settings_{std::move(settings)}, access_{std::move(access)}, credentials_{&credentials}, tls_context_{&tls_context} {
        if (auto parsed = routing::parse_url(settings_.upstream.base_url)) {
            upstream_ = std::move(*parsed);
        }
    }

    bool uses_credential_endpoint() const { return credentials_->endpoint().has_value(); }

    stats const& counters() const noexcept { return counters_; }
    bool configured() const noexcept { return upstream_.has_value(); }

    template <net::byte_stream Client>
    async::task<net::result_void> operator()(httpio::request const& incoming, httpio::response_writer<Client>& out) {
        ++counters_.requests;
        auto const which = routing::route_of(incoming.head.method, incoming.head.target);
        if (auto refusal = check_access(incoming, which)) {
            ++counters_.refused;
            co_return co_await reject(out, std::move(*refusal));
        }
        switch (which) {
            case routing::route::messages: co_return co_await forward(incoming, out, false);
            case routing::route::count_tokens: co_return co_await estimate_tokens(incoming, out);
            case routing::route::models: co_return co_await respond(out, 200, detail::models_json(settings_.models));
            case routing::route::health: co_return co_await respond(out, 200, R"({"status":"ok"})");
            case routing::route::statistics: co_return co_await respond(out, 200, detail::stats_json(counters_));
            case routing::route::wrong_method: co_return co_await reject(out, routing::error_reply(405, "invalid_request_error", "method not allowed"));
            case routing::route::unknown_path: break;
        }
        co_return co_await reject(out, routing::error_reply(404, "not_found_error", "unknown path"));
    }

private:
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
        co_return co_await respond(out, problem.status, std::move(problem.body));
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

    async::task<std::expected<net::any_stream, std::error_code>> connect_upstream() {
        auto const key = net::endpoint{.host = upstream_->host, .port = upstream_->port};
        if (auto reused = pool_.take(key)) {
            ++counters_.connections_reused;
            co_return std::move(*reused);
        }
        ++counters_.connections_opened;
        if (upstream_->secure) {
            auto stream = co_await net::tls_stream::connect(*loop_, *tls_context_, upstream_->host, upstream_->port, settings_.time.connect);
            if (!stream) {
                co_return std::unexpected{stream.error()};
            }
            co_return net::any_stream{std::move(*stream)};
        }
        auto stream = co_await net::tcp_stream::connect(*loop_, upstream_->host, upstream_->port, settings_.time.connect);
        if (!stream) {
            co_return std::unexpected{stream.error()};
        }
        co_return net::any_stream{std::move(*stream)};
    }

    template <net::byte_stream Client>
    async::task<net::result_void> forward(httpio::request const& incoming, httpio::response_writer<Client>& out, bool) {
        if (!upstream_) {
            co_return co_await reject(out, routing::error_reply(500, "api_error", "the upstream base_url is not valid"));
        }
        auto prepared = routing::prepare(incoming.body, [this](std::string const& requested) -> std::optional<std::string> {
            auto const mapped = settings_.models.find(requested);
            return mapped == settings_.models.end() ? std::nullopt : std::optional<std::string>{mapped->second};
        });
        if (!prepared) {
            co_return co_await reject(out, std::move(prepared.error()));
        }
        auto token = co_await credentials_->bearer();
        if (!token) {
            co_return co_await reject(out, routing::error_reply(401, "authentication_error", "no upstream credential: run llmgate login (" + token.error().message() + ")"));
        }
        if (auto const dynamic = credentials_->endpoint()) {
            if (auto parsed = routing::parse_url(*dynamic)) {
                upstream_ = std::move(*parsed);
            }
        }
        auto upstream = co_await connect_upstream();
        if (!upstream) {
            ++counters_.upstream_failures;
            co_return co_await reject(out, routing::error_reply(502, "api_error", "cannot reach the upstream: " + upstream.error().message()));
        }
        httpio::request_spec spec{.method = "POST",
                                  .target = upstream_->base_path + settings_.upstream.messages_path,
                                  .host = upstream_->host,
                                  .fields = detail::upstream_headers(incoming, settings_.upstream, *token),
                                  .body = std::move(prepared->body)};
        co_return co_await relay(*upstream, std::move(spec), out, upstream_key());
    }

    net::endpoint upstream_key() const { return net::endpoint{.host = upstream_->host, .port = upstream_->port}; }

    template <net::byte_stream Client>
    async::task<net::result_void> relay(net::any_stream& upstream, httpio::request_spec spec, httpio::response_writer<Client>& out,
                                        net::endpoint key) {
        auto const wire = httpio::format_request(spec);
        if (!wire) {
            co_return co_await reject(out, routing::error_reply(502, "api_error", "cannot format the upstream request"));
        }
        if (auto const sent = co_await upstream.write_all(std::as_bytes(std::span{*wire})); !sent) {
            ++counters_.upstream_failures;
            co_return co_await reject(out, routing::error_reply(502, "api_error", "upstream write failed: " + sent.error().message()));
        }
        detail::relay_state state{.counters = &counters_, .buffer_limit = buffer_limit};
        httpio::message_reader<net::any_stream> reader{upstream, http::kind::response, httpio::limits{}};
        std::error_code failure;
        while (!state.complete) {
            auto const waiting_for_head = !state.head.has_value();
            auto const idle = waiting_for_head ? settings_.time.response_head : settings_.time.stream_idle;
            auto const step = co_await reader.step(state, {.idle = idle, .watch = out.descriptor()});
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
            if (!step) {
                failure = step.error();
                break;
            }
        }
        co_return co_await conclude(upstream, state, failure, out, std::move(key));
    }

    static http::header_map stream_headers(http::message_head const& head) {
        auto fields = routing::downstream_headers(head.fields);
        return fields;
    }

    template <net::byte_stream Client>
    async::task<net::result_void> conclude(net::any_stream& upstream, detail::relay_state& state, std::error_code const failure,
                                           httpio::response_writer<Client>& out, net::endpoint key) {
        if (failure == std::errc::connection_aborted) {
            ++counters_.client_disconnects;
            co_return net::result_void{};
        }
        if (failure) {
            (failure == std::errc::timed_out ? counters_.upstream_timeouts : counters_.upstream_failures) += 1;
        }
        if (state.streaming && state.head_sent) {
            state.finish_stream();
            for (auto const& frame : state.pending) {
                (void)co_await out.write_chunk(std::as_bytes(std::span{frame}));
            }
            co_return co_await out.end_stream();
        }
        if (failure || !state.head) {
            auto const status = failure == std::errc::timed_out ? 504 : 502;
            co_return co_await reject(out, routing::error_reply(status, "api_error", "the upstream did not answer: " + failure.message()));
        }
        if (state.buffer_overflow) {
            co_return co_await reject(out, routing::error_reply(502, "api_error", "the upstream response is too large"));
        }
        if (state.head->status == 401 || state.head->status == 403) {
            credentials_->rejected();
        }
        if (state.head->keep_alive) {
            pool_.give_back(std::move(key), std::move(upstream));
        }
        auto const fields = routing::downstream_headers(state.head->fields);
        co_return co_await out.send(state.head->status, fields, state.buffered);
    }

    static constexpr std::size_t buffer_limit = 33554432;

    async::reactor* loop_;
    config settings_;
    access_policy access_;
    credential* credentials_;
    tls::client_context const* tls_context_;
    std::optional<routing::endpoint_url> upstream_;
    net::connection_pool<net::any_stream> pool_;
    stats counters_;
};

}  // namespace llmgate::gateway

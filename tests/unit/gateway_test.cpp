#include <boost/ut.hpp>

import llmgate.anthropic;
import llmgate.async;
import llmgate.control;
import llmgate.gateway;
import llmgate.http;
import llmgate.httpio;
import llmgate.json;
import llmgate.net;
import llmgate.proof;
import llmgate.reflect;
import llmgate.routing;
import llmgate.sse;
import llmgate.tls;
import llmgate.usage;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate;

using upstream_writer = httpio::response_writer<net::tcp_stream>;
using std::chrono::milliseconds;

constexpr std::string_view loopback = "127.0.0.1";
constexpr milliseconds short_idle{80};
constexpr int tick_limit = 3000;
constexpr milliseconds tick{1};

enum class behaviour {
    json_ok,
    json_usage,
    sse_usage,
    sse_clean,
    sse_done_marker,
    sse_unparseable_event,
    sse_duplicate_start,
    sse_missing_start,
    sse_unclosed_tool_block,
    sse_truncated,
    sse_stalls,
    sse_endless,
    rate_limited,
    not_a_message,
    server_error,
    bad_request,
    silent,
};

struct recorded {
    std::string target;
    std::string body;
    std::map<std::string, std::string> headers;
};

std::string stream_body_event(std::string_view const type, std::string_view const extra = "") {
    return std::format(R"({{"type":"{}"{}}})", type, extra);
}

struct fake_upstream {
    behaviour mode = behaviour::json_ok;
    std::vector<recorded> seen;
    std::string request_id = "req_1";
    int connections = 0;
    bool endless_writer_failed = false;
    async::reactor* loop = nullptr;

    async::task<net::result_void> emit(upstream_writer& out, std::string_view const name, std::string data) {
        std::string frame;
        sse::write_event(frame, name, data);
        auto const written = co_await out.write_chunk(std::as_bytes(std::span{frame}));
        co_await loop->sleep(tick);
        co_return written;
    }

    async::task<net::result_void> script(upstream_writer& out, std::vector<std::pair<std::string, std::string>> events) {
        http::header_map fields;
        fields.add("Content-Type", "text/event-stream");
        if (auto const begun = co_await out.begin_stream(200, fields); !begun) {
            co_return begun;
        }
        for (auto& [name, data] : events) {
            if (auto const written = co_await emit(out, name, std::move(data)); !written) {
                co_return written;
            }
        }
        co_return co_await out.end_stream();
    }

    async::task<net::result_void> operator()(httpio::request const& incoming, upstream_writer& out) {
        recorded record{.target = incoming.head.target, .body = incoming.body, .headers = {}};
        for (auto const& field : incoming.head.fields.all()) {
            record.headers[field.name] = field.value;
        }
        seen.push_back(std::move(record));
        auto const start = std::pair{"message_start", stream_body_event("message_start", R"(,"message":{"id":"m"})")};
        auto const block = std::pair{"content_block_start", stream_body_event("content_block_start", R"(,"index":0)")};
        auto const text = std::pair{"content_block_delta", stream_body_event("content_block_delta", R"(,"index":0,"delta":{"text":"hi"})")};
        auto const end_block = std::pair{"content_block_stop", stream_body_event("content_block_stop", R"(,"index":0)")};
        auto const delta = std::pair{"message_delta", stream_body_event("message_delta", R"(,"delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":2})")};
        auto const stop = std::pair{"message_stop", stream_body_event("message_stop")};
        switch (mode) {
            case behaviour::json_ok: {
                http::header_map fields;
                fields.add("Content-Type", "application/json");
                fields.add("Request-Id", request_id);
                co_return co_await out.send(200, fields, R"({"type":"message","content":[]})");
            }
            case behaviour::json_usage: {
                http::header_map fields;
                fields.add("Content-Type", "application/json");
                co_return co_await out.send(200, fields, R"({"type":"message","model":"claude-opus-upstream","usage":{"input_tokens":11,"output_tokens":5,"cache_read_input_tokens":3}})");
            }
            case behaviour::sse_usage: {
                auto const counted_start = std::pair{"message_start", stream_body_event("message_start", R"(,"message":{"id":"m","model":"claude-opus-upstream","usage":{"input_tokens":20,"output_tokens":1,"cache_creation_input_tokens":4}})")};
                auto const counted_delta = std::pair{"message_delta", stream_body_event("message_delta", R"(,"delta":{},"usage":{"output_tokens":9})")};
                co_return co_await script(out, {counted_start, block, text, end_block, counted_delta, stop});
            }
            case behaviour::not_a_message: {
                http::header_map fields;
                fields.add("Content-Type", "application/json");
                co_return co_await out.send(200, fields, R"({"choices":[],"usage":{"completion_tokens":16000}})");
            }
            case behaviour::rate_limited: {
                http::header_map fields;
                fields.add("Content-Type", "application/json");
                co_return co_await out.send(429, fields, R"({"type":"error","error":{"type":"rate_limit_error"}})");
            }
            case behaviour::server_error: {
                http::header_map fields;
                fields.add("Content-Type", "application/json");
                co_return co_await out.send(502, fields, R"({"type":"error","error":{"type":"api_error"}})");
            }
            case behaviour::bad_request: {
                http::header_map fields;
                fields.add("Content-Type", "application/json");
                co_return co_await out.send(400, fields, R"({"type":"error","error":{"type":"invalid_request_error"}})");
            }
            case behaviour::silent: {
                co_await loop->sleep(std::chrono::seconds{30});
                co_return net::result_void{};
            }
            case behaviour::sse_clean: co_return co_await script(out, {start, block, text, end_block, delta, stop});
            case behaviour::sse_done_marker: co_return co_await script(out, {start, block, text, end_block, delta, stop, {"", "[DONE]"}});
            case behaviour::sse_unparseable_event: co_return co_await script(out, {start, block, {"note", "hello"}, text, end_block, delta, stop});
            case behaviour::sse_duplicate_start: co_return co_await script(out, {start, start, start, block, text, end_block, delta, stop});
            case behaviour::sse_missing_start: co_return co_await script(out, {block, text, end_block, delta, stop});
            case behaviour::sse_unclosed_tool_block: co_return co_await script(out, {start, block, text, delta, stop});
            case behaviour::sse_truncated: {
                http::header_map fields;
                fields.add("Content-Type", "text/event-stream");
                (void)co_await out.begin_stream(200, fields);
                (void)co_await emit(out, start.first, start.second);
                (void)co_await emit(out, block.first, block.second);
                co_return std::unexpected{std::make_error_code(std::errc::connection_reset)};
            }
            case behaviour::sse_stalls: {
                http::header_map fields;
                fields.add("Content-Type", "text/event-stream");
                (void)co_await out.begin_stream(200, fields);
                (void)co_await emit(out, start.first, start.second);
                co_await loop->sleep(std::chrono::seconds{30});
                co_return net::result_void{};
            }
            case behaviour::sse_endless: {
                http::header_map fields;
                fields.add("Content-Type", "text/event-stream");
                (void)co_await out.begin_stream(200, fields);
                (void)co_await emit(out, start.first, start.second);
                (void)co_await emit(out, block.first, block.second);
                for (;;) {
                    if (!co_await emit(out, text.first, text.second)) {
                        endless_writer_failed = true;
                        co_return net::result_void{};
                    }
                }
            }
        }
        co_return net::result_void{};
    }
};

async::task<void> accept_upstream(async::reactor& loop, net::listener& server, fake_upstream& handler) {
    for (;;) {
        auto accepted = co_await server.accept();
        if (!accepted) {
            co_return;
        }
        ++handler.connections;
        loop.spawn(httpio::serve_connection(std::move(*accepted), handler, httpio::limits{}));
    }
}

struct reply {
    int status = 0;
    std::string body;
    std::map<std::string, std::string> headers;
    std::error_code error;
};

async::task<reply> call(async::reactor& loop, std::uint16_t const port, httpio::request_spec spec) {
    reply result;
    auto client = co_await net::tcp_stream::connect(loop, std::string{loopback}, port);
    if (!client) {
        result.error = client.error();
        co_return result;
    }
    auto const finished = co_await httpio::exchange(*client, std::move(spec), [&result](http::event const& item) {
        if (auto const* head = std::get_if<http::message_head>(&item)) {
            result.status = head->status;
            for (auto const& field : head->fields.all()) {
                result.headers[field.name] = field.value;
            }
        } else if (auto const* chunk = std::get_if<http::body_chunk>(&item)) {
            result.body.append(reinterpret_cast<char const*>(chunk->bytes.data()), chunk->bytes.size());
        }
    });
    result.error = finished ? std::error_code{} : finished.error();
    co_return result;
}

httpio::request_spec messages_request(std::string body, std::string target = "/anthropic/v1/messages",
                                      std::string client_key = "client-secret") {
    http::header_map fields;
    fields.add("Content-Type", "application/json");
    fields.add("anthropic-version", "2023-06-01");
    fields.add("anthropic-beta", "tools-2024");
    if (!client_key.empty()) {
        fields.add("x-api-key", std::move(client_key));
    }
    return {.method = "POST", .target = std::move(target), .host = "localhost", .fields = std::move(fields), .body = std::move(body)};
}

httpio::request_spec control_request(std::string body, std::string_view const key) {
    auto request = messages_request(body, "/llmgate/control", "");
    if (!key.empty()) {
        request.fields.add("x-llmgate-proof", proof::hmac_sha256_hex(key, proof::identity_message("scope-a/control", body)));
    }
    return request;
}

std::string thinking_history(std::string_view const model) {
    return std::format(R"({{"model":"{}","max_tokens":8,"stream":false,"messages":[{{"role":"user","content":"hi"}},)"
                       R"({{"role":"assistant","content":[{{"type":"thinking","thinking":"x","signature":"sig-from-a"}},{{"type":"text","text":"ok"}}]}},{{"role":"user","content":"more"}}]}})",
                       model);
}

httpio::request_spec in_session(std::string body, std::string_view const session) {
    auto spec = messages_request(std::move(body), "/anthropic/v1/messages", "");
    if (!session.empty()) {
        spec.fields.add("x-claude-code-session-id", std::string{session});
    }
    return spec;
}

constexpr std::int64_t fixed_time = 1760659200;

constexpr std::string_view golden_statistics =
    R"({"requests":3,"streamed":1,"upstream_failures":0,"upstream_timeouts":0,"client_disconnects":0,"refused":0,"connections_opened":1,"connections_reused":1,"budget_refused":0,"no_provider":0,"repairs":{"duplicate_message_start":2},"repair_examples":{},"betas":{"tools-2024":3},"thinking_blocks_stripped":0,"classifier_requests":{},"classifier_unparseable":{},"dropped_fields":{},"ignored_events":{},"providers":{"default":{"attempts":2,"answered":2,"failures":0,"skipped":0,"circuit":"closed"}},"usage":{"today":{"input":11,"output":7,"cache_read":3,"cache_creation":0,"requests":2},"month":{"input":11,"output":7,"cache_read":3,"cache_creation":0,"requests":2},"budget":{"state":"ok","window":"daily","used":18,"limit":100,"percent":18}}})";
constexpr std::string_view golden_models = R"json({"data":[{"type":"model","id":"opus","display_name":"opus","created_at":"1970-01-01T00:00:00Z"},{"type":"model","id":"claude-opus-upstream@default","display_name":"claude-opus-upstream (default)","created_at":"1970-01-01T00:00:00Z","description":"served by the provider default"}],"has_more":false})json";
constexpr std::string_view golden_error = R"({"type":"error","error":{"type":"invalid_request_error","message":"a \"q\" \n b"}})";

json::value canonical(json::value source) {
    if (auto* members = std::get_if<json::object>(&source.data)) {
        for (auto& member : *members) {
            member.second = canonical(std::move(member.second));
        }
        std::ranges::sort(*members, {}, &json::object::value_type::first);
    } else if (auto* items = std::get_if<json::array>(&source.data)) {
        for (auto& item : *items) {
            item = canonical(std::move(item));
        }
    }
    return source;
}

json::value without_repairs(json::value source) {
    auto& members = std::get<json::object>(source.data);
    std::erase_if(members, [](auto const& member) { return member.first == "repairs"; });
    return source;
}

std::string chat_with_session(std::string_view const model, bool const stream) {
    return std::format(R"({{"model":"{}","max_tokens":8,"stream":{},"metadata":{{"user_id":"user_ab_account_cd_session_5f3a-90"}},"messages":[{{"role":"user","content":"hi"}}]}})", model, stream);
}

std::string chat(std::string_view const model, bool const stream) {
    return std::format(R"({{"model":"{}","max_tokens":8,"stream":{},"messages":[{{"role":"user","content":"hi"}}]}})", model, stream);
}

struct world {
    async::reactor loop;
    net::listener upstream_listener;
    net::listener gateway_listener;
    fake_upstream upstream;
    gateway::static_credential credential;
    tls::client_context tls_context;
    gateway::proxy gateway;

    static gateway::config settings(std::uint16_t const upstream_port, gateway::usage_config usage_settings) {
        return {.listen = "127.0.0.1:0",
                .upstream = {.base_url = std::format("http://127.0.0.1:{}", upstream_port),
                             .messages_path = "/v1/messages",
                             .headers = {{"Editor-Version", "test/1.0"}}},
                .models = {{"opus", "claude-opus-upstream"}},
                .time = {.connect = std::chrono::seconds{2}, .response_head = std::chrono::seconds{2}, .stream_idle = short_idle},
                .usage = std::move(usage_settings)};
    }

    explicit world(std::string token = "upstream-token", std::string client_key = "", std::optional<std::uint64_t> const seed = std::nullopt,
                   std::string identity = "", gateway::usage_config usage_settings = {})
        : loop{seed},
          upstream_listener{std::move(*net::listener::bind(loop, std::string{loopback}, 0))},
          gateway_listener{std::move(*net::listener::bind(loop, std::string{loopback}, 0))},
          credential{std::move(token)},
          tls_context{std::move(*tls::client_context::make(tls::group_policy::classical_only))},
          gateway{loop, settings(upstream_listener.port(), std::move(usage_settings)),
                  {.client_key = std::move(client_key), .allowed_hosts = routing::default_allowed_hosts(loopback, gateway_listener.port()), .identity_key = std::move(identity), .identity_scope = "scope-a"},
                  credential, tls_context} {
        upstream.loop = &loop;
        loop.spawn(accept_upstream(loop, upstream_listener, upstream));
        loop.spawn(httpio::serve(gateway_listener, gateway, httpio::limits{.max_body_bytes = 4096}));
    }

    std::uint16_t port() const noexcept { return gateway_listener.port(); }

    reply ask(httpio::request_spec spec) {
        spec.host = std::format("127.0.0.1:{}", port());
        return loop.run(call(loop, port(), std::move(spec)));
    }

    reply ask_as(httpio::request_spec spec, std::string host) {
        spec.host = std::move(host);
        return loop.run(call(loop, port(), std::move(spec)));
    }
};

constexpr milliseconds duo_cooldown{1000};
constexpr unsigned duo_failures = 2;
constexpr milliseconds duo_response_head{150};

void route(gateway::config& settings, std::string_view const role, std::initializer_list<std::string_view> const names) {
    auto& list = settings.routes[std::string{role}];
    for (auto const name : names) {
        list.emplace_back(name);
    }
}

struct duo {
    async::reactor loop;
    net::listener first_listener;
    net::listener second_listener;
    net::listener gateway_listener;
    fake_upstream first;
    fake_upstream second;
    gateway::static_credential credential_a;
    gateway::static_credential credential_b;
    tls::client_context tls_context;
    std::int64_t now_ms = 0;
    gateway::proxy gateway;

    static gateway::config settings(std::uint16_t const first_port, std::uint16_t const second_port, std::function<void(gateway::config&)> const& tune) {
        gateway::config built{.listen = "127.0.0.1:0",
                              .fallback = {.failures = duo_failures, .cooldown = duo_cooldown},
                              .time = {.connect = std::chrono::seconds{2}, .response_head = duo_response_head, .stream_idle = short_idle}};
        built.providers["a"] = {.base_url = std::format("http://127.0.0.1:{}", first_port), .headers = {{"Editor-Version", "a/1.0"}}, .models = {{"opus", "model-a"}}};
        built.providers["b"] = {.base_url = std::format("http://127.0.0.1:{}", second_port), .headers = {{"Editor-Version", "b/1.0"}}, .models = {{"opus", "model-b"}, {"extra", "model-extra"}}};
        tune(built);
        return built;
    }

    static gateway::credential_map credentials(gateway::credential& one, gateway::credential& two) {
        gateway::credential_map all;
        all.emplace("a", &one);
        all.emplace("b", &two);
        return all;
    }

    explicit duo(std::function<void(gateway::config&)> const& tune = [](gateway::config&) {}, std::string token_a = "token-a", std::uint16_t const first_port = 0)
        : first_listener{std::move(*net::listener::bind(loop, std::string{loopback}, 0))},
          second_listener{std::move(*net::listener::bind(loop, std::string{loopback}, 0))},
          gateway_listener{std::move(*net::listener::bind(loop, std::string{loopback}, 0))},
          credential_a{std::move(token_a)},
          credential_b{"token-b"},
          tls_context{std::move(*tls::client_context::make(tls::group_policy::classical_only))},
          gateway{loop, settings(first_port == 0 ? first_listener.port() : first_port, second_listener.port(), tune),
                  {.allowed_hosts = routing::default_allowed_hosts(loopback, gateway_listener.port())}, credentials(credential_a, credential_b), tls_context} {
        first.loop = &loop;
        second.loop = &loop;
        second.request_id = "from-b";
        gateway.use_clock([] { return fixed_time; });
        gateway.use_breaker_clock([this] { return now_ms; });
        loop.spawn(accept_upstream(loop, first_listener, first));
        loop.spawn(accept_upstream(loop, second_listener, second));
        loop.spawn(httpio::serve(gateway_listener, gateway, httpio::limits{.max_body_bytes = 4096}));
    }

    reply ask(httpio::request_spec spec) {
        spec.host = std::format("127.0.0.1:{}", gateway_listener.port());
        return loop.run(call(loop, gateway_listener.port(), std::move(spec)));
    }

    reply chat_with(std::string_view const model, bool const stream = false) { return ask(messages_request(chat(model, stream), "/anthropic/v1/messages", "")); }

    reply stats() {
        auto request = messages_request("");
        request.method = "GET";
        request.target = "/llmgate/stats";
        return ask(std::move(request));
    }

    gateway::provider_stats const& counts(std::string const& name) const { return gateway.counters().providers.at(name); }
};

std::vector<std::string> event_names(std::string_view const wire) {
    sse::parser parser;
    std::vector<std::string> names;
    auto note = [&names](sse::event const& item) { names.emplace_back(item.name); };
    parser.feed(std::as_bytes(std::span{wire}), note);
    parser.finish(note);
    return names;
}

const std::vector<std::string> complete_stream{"message_start", "content_block_start", "content_block_delta", "content_block_stop", "message_delta", "message_stop"};

async::task<bool> eventually(async::reactor& loop, std::function<bool()> condition) {
    for (int step = 0; step < tick_limit; ++step) {
        if (condition()) {
            co_return true;
        }
        co_await loop.sleep(tick);
    }
    co_return false;
}

template <net::byte_stream Inner>
class failing_writes {
public:
    failing_writes(Inner inner, std::size_t const allowed) : inner_{std::move(inner)}, allowed_{allowed} {}

    async::task<net::result_size> read_some(std::span<std::byte> into, net::read_options const options) { co_return co_await inner_.read_some(into, options); }

    async::task<net::result_void> write_all(std::span<std::byte const> data) {
        if (writes_++ >= allowed_) {
            co_return std::unexpected{std::make_error_code(std::errc::broken_pipe)};
        }
        co_return co_await inner_.write_all(data);
    }

    int descriptor() const noexcept { return inner_.descriptor(); }

private:
    Inner inner_;
    std::size_t allowed_;
    std::size_t writes_ = 0;
};

async::task<void> serve_failing_writes(world& served, net::listener& server, std::size_t const allowed_writes) {
    auto accepted = co_await server.accept();
    co_await httpio::serve_connection(failing_writes<net::tcp_stream>{std::move(*accepted), allowed_writes}, served.gateway, httpio::limits{});
}

const suite gateway_suite = [] {
    "capability_a_plain_reply_is_counted_in_the_ledger_and_written_as_a_line"_test = [] {
        world w;
        w.gateway.use_clock([] { return fixed_time; });
        std::vector<std::string> lines;
        w.gateway.persist_usage_with([&lines](std::string const& line) { lines.push_back(line); });
        w.upstream.mode = behaviour::json_usage;

        auto const answer = w.ask(messages_request(chat_with_session("opus", false)));

        expect(answer.status == 200);
        auto const today = w.gateway.ledger().on_day(usage::day_of(fixed_time));
        expect(today.used == usage::tokens{.input = 11, .output = 5, .cache_read = 3});
        expect(today.requests == 1_u);
        expect(lines.size() == 1_u);
        auto const entry = usage::from_line(lines.at(0));
        expect(entry.has_value());
        expect(entry->requested == "opus" && entry->model == "claude-opus-upstream" && entry->role == "opus");
        expect(entry->session == "5f3a-90" && entry->status == 200 && entry->time == fixed_time);
    };

    "defect_a_hostile_model_name_is_neutralised_before_it_is_recorded"_test = [] {
        world w;
        w.gateway.use_clock([] { return fixed_time; });
        std::vector<std::string> lines;
        w.gateway.persist_usage_with([&lines](std::string const& line) { lines.push_back(line); });

        (void)w.ask(messages_request(chat("evil\\u001b[2Jname", false)));

        auto const entry = usage::from_line(lines.at(0));
        expect(entry->requested == "evil?[2Jname");
        expect(!lines.at(0).contains("\\u001b"));
    };

    "capability_a_stream_is_counted_from_its_start_and_final_delta"_test = [] {
        world w;
        w.gateway.use_clock([] { return fixed_time; });
        w.upstream.mode = behaviour::sse_usage;

        auto const answer = w.ask(messages_request(chat("opus", true)));

        expect(answer.status == 200);
        expect(w.gateway.ledger().on_day(usage::day_of(fixed_time)).used == usage::tokens{.input = 20, .output = 9, .cache_creation = 4});
    };

    "capability_a_repaired_stream_records_how_many_repairs_it_needed"_test = [] {
        world w;
        w.gateway.use_clock([] { return fixed_time; });
        std::vector<std::string> lines;
        w.gateway.persist_usage_with([&lines](std::string const& line) { lines.push_back(line); });
        w.upstream.mode = behaviour::sse_duplicate_start;

        (void)w.ask(messages_request(chat("opus", true)));

        expect(lines.size() == 1_u);
        expect(usage::from_line(lines.at(0))->repairs == 2_u);
    };

    "limit_an_upstream_error_is_recorded_with_its_status_and_no_tokens"_test = [] {
        world w;
        w.gateway.use_clock([] { return fixed_time; });
        std::vector<std::string> lines;
        w.gateway.persist_usage_with([&lines](std::string const& line) { lines.push_back(line); });
        w.upstream.mode = behaviour::rate_limited;

        (void)w.ask(messages_request(chat("opus", false)));

        auto const entry = usage::from_line(lines.at(0));
        expect(entry->status == 429 && entry->used == usage::tokens{});
        expect(w.gateway.ledger().on_day(usage::day_of(fixed_time)).requests == 1_u);
    };

    "limit_an_exhausted_daily_budget_refuses_with_a_rate_limit_error_until_the_next_day"_test = [] {
        world w{"upstream-token", "", std::nullopt, "", {.budget = {.daily_tokens = 10}}};
        std::int64_t now = fixed_time;
        w.gateway.use_clock([&now] { return now; });
        w.upstream.mode = behaviour::json_usage;

        auto const first = w.ask(messages_request(chat("opus", false)));
        auto const second = w.ask(messages_request(chat("opus", false)));
        now += usage::seconds_per_day;
        auto const next_day = w.ask(messages_request(chat("opus", false)));

        expect(first.status == 200);
        expect(second.status == 429);
        expect(second.body.contains(R"("type":"rate_limit_error")"));
        expect(second.body.contains("llmgate budget used up: daily 16 of 10 tokens"));
        expect(next_day.status == 200);
        expect(w.upstream.seen.size() == 2_u);
        expect(w.gateway.counters().budget_refused == 1_u);
    };

    "limit_a_warning_level_budget_still_serves"_test = [] {
        world w{"upstream-token", "", std::nullopt, "", {.budget = {.daily_tokens = 20}}};
        w.gateway.use_clock([] { return fixed_time; });
        w.upstream.mode = behaviour::json_usage;

        (void)w.ask(messages_request(chat("opus", false)));
        auto const second = w.ask(messages_request(chat("opus", false)));

        expect(second.status == 200);
        expect(w.gateway.counters().budget_refused == 0_u);
    };

    "capability_the_statistics_wire_format_keeps_every_key_and_value"_test = [] {
        world w{"upstream-token", "", std::nullopt, "", {.budget = {.daily_tokens = 100}}};
        w.gateway.use_clock([] { return fixed_time; });
        w.upstream.mode = behaviour::sse_duplicate_start;
        (void)w.ask(messages_request(chat("opus", true)));
        w.upstream.mode = behaviour::json_usage;
        (void)w.ask(messages_request(chat("opus", false)));
        auto stats = messages_request("");
        stats.method = "GET";
        stats.target = "/llmgate/stats";

        auto const body = w.ask(std::move(stats)).body;

        auto actual = json::parse(body);
        auto golden = json::parse(golden_statistics);
        expect(actual.has_value() && golden.has_value());
        expect(json::write(without_repairs(canonical(*actual))) == json::write(without_repairs(canonical(*golden))));
        auto const* repairs = json::at(*actual, {"repairs", "duplicate_message_start"});
        expect(repairs != nullptr && json::count_at(*actual, {"repairs", "duplicate_message_start"}) == std::optional<std::uint64_t>{2});
        for (auto const key : {"requests", "streamed", "upstream_failures", "upstream_timeouts", "client_disconnects", "refused", "connections_opened",
                               "connections_reused", "budget_refused", "no_provider", "repairs", "providers", "usage"}) {
            expect(json::at(*actual, {key}) != nullptr) << key;
        }
    };

    "capability_every_repair_kind_has_a_counter_and_a_statistics_key_without_further_code"_test = [] {
        world w;
        gateway::repair_counts counts{};
        for (auto const kind : reflect::enum_values<anthropic::repair_kind>) {
            ++gateway::repair_slot(counts, kind);
        }
        auto stats = messages_request("");
        stats.method = "GET";
        stats.target = "/llmgate/stats";

        auto const fresh = json::parse(w.ask(std::move(stats)).body);

        expect(fresh.has_value());
        for (auto const kind : reflect::enum_values<anthropic::repair_kind>) {
            expect(gateway::repair_slot(counts, kind) == 1_u);
            expect(json::count_at(*fresh, {"repairs", reflect::enum_name(kind)}) == std::optional<std::uint64_t>{0}) << reflect::enum_name(kind);
        }
        auto const written = json::to_value(counts);
        for (auto const kind : reflect::enum_values<anthropic::repair_kind>) {
            expect(json::count_at(written, {reflect::enum_name(kind)}) == std::optional<std::uint64_t>{1});
        }
    };

    "capability_the_models_wire_format_keeps_every_key_and_value"_test = [] {
        world w;
        auto request = messages_request("");
        request.method = "GET";
        request.target = "/anthropic/v1/models";

        auto const body = w.ask(std::move(request)).body;

        expect(body == golden_models);
    };

    "capability_the_error_body_wire_format_is_unchanged"_test = [] {
        expect(routing::error_json("invalid_request_error", "a \"q\" \n b") == golden_error);
        expect(routing::error_reply(404, "not_found_error", "unknown path").body == R"({"type":"error","error":{"type":"not_found_error","message":"unknown path"}})");
    };

    "capability_the_statistics_endpoint_reports_todays_usage_and_the_budget"_test = [] {
        world w{"upstream-token", "", std::nullopt, "", {.budget = {.daily_tokens = 100}}};
        w.gateway.use_clock([] { return fixed_time; });
        w.upstream.mode = behaviour::json_usage;
        (void)w.ask(messages_request(chat("opus", false)));
        auto stats = messages_request("");
        stats.method = "GET";
        stats.target = "/llmgate/stats";

        auto const answer = w.ask(std::move(stats));

        auto const headline = usage::headline_from(answer.body);
        expect(headline.today == 16_u);
        expect(headline.state == "ok" && headline.window == "daily" && headline.limit == 100_u && headline.percent == 16_u);
    };

    "capability_a_known_alias_is_swapped_and_the_upstream_reply_returns_unchanged"_test = [] {
        world w;

        auto const answer = w.ask(messages_request(chat("opus", false)));

        expect(answer.status == 200) << answer.error.message();
        expect(answer.body == R"({"type":"message","content":[]})");
        expect(answer.headers.at("Request-Id") == "req_1");
        expect(w.upstream.seen.size() == 1_u);
        expect(w.upstream.seen[0].target == "/v1/messages");
        expect(w.upstream.seen[0].body.contains(R"("model":"claude-opus-upstream")"));
    };

    "capability_an_unknown_model_name_goes_upstream_as_it_is"_test = [] {
        world w;

        auto const answer = w.ask(messages_request(chat("claude-direct", false)));

        expect(answer.status == 200);
        expect(w.upstream.seen[0].body.contains(R"("model":"claude-direct")"));
    };

    "limit_the_client_key_never_reaches_the_upstream_and_the_gateway_credential_does"_test = [] {
        world w{"upstream-token"};

        (void)w.ask(messages_request(chat("opus", false)));

        auto const& headers = w.upstream.seen.at(0).headers;
        expect(headers.at("Authorization") == "Bearer upstream-token");
        expect(!headers.contains("x-api-key"));
        expect(headers.at("anthropic-version") == "2023-06-01");
        expect(headers.at("anthropic-beta") == "tools-2024");
        expect(headers.at("Editor-Version") == "test/1.0");
    };

    "capability_a_provider_with_a_copilot_login_gets_the_editor_headers_it_needs"_test = [] {
        duo d{[](gateway::config& built) {
            built.providers["a"].headers.clear();
            built.providers["a"].copilot = gateway::copilot_config{};
        }};

        (void)d.chat_with("opus");

        auto const& headers = d.first.seen.at(0).headers;
        expect(headers.at("Editor-Version") == "vscode/1.99.0");
        expect(headers.at("Copilot-Integration-Id") == "vscode-chat");
    };

    "limit_a_configured_editor_header_wins_over_the_default_whatever_its_case"_test = [] {
        duo d{[](gateway::config& built) {
            built.providers["a"].headers = {{"editor-version", "mine/9"}};
            built.providers["a"].copilot = gateway::copilot_config{};
        }};

        (void)d.chat_with("opus");

        auto const& headers = d.first.seen.at(0).headers;
        expect(headers.at("editor-version") == "mine/9");
        expect(!headers.contains("Editor-Version"));
        expect(headers.at("Copilot-Integration-Id") == "vscode-chat");
    };

    "limit_a_provider_without_a_copilot_login_gets_no_copilot_headers"_test = [] {
        world w;

        (void)w.ask(messages_request(chat("opus", false)));

        expect(!w.upstream.seen.at(0).headers.contains("Copilot-Integration-Id"));
    };

    "capability_a_done_marker_after_the_stream_is_dropped_without_counting_a_repair"_test = [] {
        world w;
        w.upstream.mode = behaviour::sse_done_marker;

        auto const answer = w.ask(messages_request(chat("opus", true)));

        expect(event_names(answer.body) == complete_stream);
        expect(gateway::repair_slot(w.gateway.counters().repairs, anthropic::repair_kind::unparseable_event) == 0_u);
        expect(w.gateway.counters().repair_examples.empty());
    };

    "capability_an_event_that_is_not_json_is_dropped_counted_and_explained_in_the_statistics"_test = [] {
        world w;
        w.upstream.mode = behaviour::sse_unparseable_event;

        auto const answer = w.ask(messages_request(chat("opus", true)));

        expect(event_names(answer.body) == complete_stream);
        expect(gateway::repair_slot(w.gateway.counters().repairs, anthropic::repair_kind::unparseable_event) == 1_u);
        expect(w.gateway.counters().repair_examples.at("unparseable_event") == "event=note data=hello");
    };

    "capability_a_forwarding_provider_gets_the_clients_credentials_and_the_next_provider_does_not"_test = [] {
        duo d{[](gateway::config& built) {
            built.providers["a"].forward_credentials = true;
            built.providers["a"].headers.clear();
        }};
        d.first.mode = behaviour::server_error;
        auto spec = messages_request(chat("opus", false), "/anthropic/v1/messages", "");
        spec.fields.add("Authorization", "Bearer client-login");
        spec.fields.add("anthropic-workspace-id", "ws-1");

        auto const answer = d.ask(std::move(spec));

        expect(answer.status == 200);
        auto const& first = d.first.seen.at(0).headers;
        expect(first.at("Authorization") == "Bearer client-login");
        expect(first.at("anthropic-workspace-id") == "ws-1");
        expect(first.at("anthropic-beta") == "tools-2024");
        auto const& second = d.second.seen.at(0).headers;
        expect(second.at("Authorization") == "Bearer token-b");
        expect(!second.contains("anthropic-workspace-id"));
    };

    "capability_a_forwarding_provider_passes_an_api_key_header_and_adds_no_token_of_its_own"_test = [] {
        duo d{[](gateway::config& built) {
            built.providers["a"].forward_credentials = true;
            built.providers["a"].headers.clear();
        }};
        auto spec = messages_request(chat("opus", false), "/anthropic/v1/messages", "sk-ant-client");

        (void)d.ask(std::move(spec));

        auto const& first = d.first.seen.at(0).headers;
        expect(first.at("x-api-key") == "sk-ant-client");
        expect(!first.contains("Authorization"));
    };

    "limit_without_forwarding_the_clients_authorization_never_reaches_the_upstream"_test = [] {
        duo d;
        auto spec = messages_request(chat("opus", false), "/anthropic/v1/messages", "");
        spec.fields.add("Authorization", "Bearer client-login");
        spec.fields.add("anthropic-workspace-id", "ws-1");

        (void)d.ask(std::move(spec));

        auto const& first = d.first.seen.at(0).headers;
        expect(first.at("Authorization") == "Bearer token-a");
        expect(!first.contains("anthropic-workspace-id"));
    };

    "capability_the_control_endpoint_applies_a_signed_command_and_the_route_view_reports_it"_test = [] {
        world w{"upstream-token", "", std::nullopt, "identity-k"};
        auto const command = std::string{R"({"action":"use","role":"opus","provider":"default","upstream":"claude-opus-upstream"})"};

        auto const applied = w.ask(control_request(command, "identity-k"));
        auto request = messages_request("");
        request.method = "GET";
        request.target = "/llmgate/route?role=opus&requested=opus";
        auto const view = w.ask(std::move(request)).body;

        expect(applied.status == 200);
        expect(view.contains(R"("via":"global_remap")") && view.contains(R"json("providers":["default"])json"));
    };

    "limit_a_control_command_with_a_wrong_or_missing_proof_is_refused_and_changes_nothing"_test = [] {
        world w{"upstream-token", "", std::nullopt, "identity-k"};
        auto const command = std::string{R"({"action":"use","role":"opus","provider":"default","upstream":"claude-opus-upstream"})"};
        auto unsigned_request = control_request(command, "identity-k");
        unsigned_request.fields = messages_request("").fields;

        expect(w.ask(control_request(command, "other-key")).status == 401);
        expect(w.ask(std::move(unsigned_request)).status == 401);
        auto request = messages_request("");
        request.method = "GET";
        request.target = "/llmgate/route?role=opus&requested=opus";
        expect(w.ask(std::move(request)).body.contains(R"("via":"config")"));
    };

    "limit_a_gateway_without_an_identity_key_has_no_control_endpoint"_test = [] {
        world w;

        expect(w.ask(control_request(R"({"action":"clear"})", "")).status == 403);
    };

    "limit_a_rejected_control_command_reports_why_with_status_400"_test = [] {
        world w{"upstream-token", "", std::nullopt, "identity-k"};

        auto const answer = w.ask(control_request(R"({"action":"use","role":"opus","provider":"ghost","upstream":"x"})", "identity-k"));

        expect(answer.status == 400 && answer.body.contains("ghost"));
    };

    "capability_the_session_view_reports_the_provider_that_last_answered_that_session"_test = [] {
        duo d;
        auto turn = messages_request(chat("opus", false), "/anthropic/v1/messages", "");
        turn.fields.add("x-claude-code-session-id", "sess-1");

        (void)d.ask(std::move(turn));
        auto view = messages_request("");
        view.method = "GET";
        view.target = "/llmgate/session?id=sess-1";
        auto const body = d.ask(std::move(view)).body;

        expect(body.contains(R"json("provider":"a")json") && body.contains(R"json("upstream":"model-a")json") && body.contains(R"json("source":"config")json"));
        expect(body.contains(R"json("fallback":false)json") && body.contains(R"json("switched":false)json"));
    };

    "capability_a_fallback_answer_is_marked_and_a_later_remap_is_marked_as_a_switch"_test = [] {
        duo d;
        d.first.mode = behaviour::server_error;
        auto ask_in_session = [&d] {
            auto turn = messages_request(chat("opus", false), "/anthropic/v1/messages", "");
            turn.fields.add("x-claude-code-session-id", "sess-2");
            (void)d.ask(std::move(turn));
            auto view = messages_request("");
            view.method = "GET";
            view.target = "/llmgate/session?id=sess-2";
            return d.ask(std::move(view)).body;
        };

        auto const after_fallback = ask_in_session();
        control::overrides pins;
        pins.global["opus"] = {.provider = "a", .upstream = "model-a"};
        d.first.mode = behaviour::json_ok;
        d.gateway.use_overrides(pins);
        auto const after_remap = ask_in_session();

        expect(after_fallback.contains(R"json("provider":"b")json") && after_fallback.contains(R"json("fallback":true)json"));
        expect(after_remap.contains(R"json("provider":"a")json") && after_remap.contains(R"json("switched":true)json") && after_remap.contains(R"json("fallback":false)json"));
    };

    "limit_an_unknown_session_view_is_empty_not_an_error"_test = [] {
        duo d;
        auto view = messages_request("");
        view.method = "GET";
        view.target = "/llmgate/session?id=nobody";

        auto const answer = d.ask(std::move(view));

        expect(answer.status == 200 && !answer.body.contains("provider"));
    };

    "capability_the_statistics_count_each_beta_token_the_clients_send"_test = [] {
        world w;
        auto first = messages_request(chat("opus", false), "/anthropic/v1/messages", "");
        first.fields = http::header_map{};
        first.fields.add("Content-Type", "application/json");
        first.fields.add("anthropic-beta", "alpha-2026, beta-2026 ,alpha-2026");
        auto second = messages_request(chat("opus", false), "/anthropic/v1/messages", "");

        (void)w.ask(std::move(first));
        (void)w.ask(std::move(second));
        auto const body = w.ask([] {
            auto request = messages_request("");
            request.method = "GET";
            request.target = "/llmgate/stats";
            return request;
        }()).body;

        expect(body.contains(R"json("betas":{)json") && body.contains(R"json("alpha-2026":2)json") && body.contains(R"json("beta-2026":1)json") && body.contains(R"json("tools-2024":2)json"));
    };

    "limit_the_beta_census_is_bounded_and_counts_the_rest_as_other"_test = [] {
        routing::beta_counts seen;

        for (std::size_t n = 0; n < routing::max_beta_tokens + 5; ++n) {
            routing::count_betas(seen, std::format("token-{}", n));
        }

        expect(seen.size() == routing::max_beta_tokens + 1);
        expect(seen.at("other") == 5_u);
        routing::count_betas(seen, "token-0");
        expect(seen.at("token-0") == 2_u);
    };

    "capability_a_responses_provider_receives_the_translated_request_on_its_own_path_without_anthropic_headers"_test = [] {
        duo d{[](gateway::config& built) { built.providers["b"].protocol = gateway::wire_protocol::openai_responses; }};

        (void)d.chat_with("claude-model-b@b", true);

        expect(d.first.seen.empty() && d.second.seen.size() == 1_u);
        auto const& seen = d.second.seen.at(0);
        expect(seen.target == "/responses") << seen.target;
        expect(seen.body.contains(R"("input":[{"type":"message","role":"user","content":[{"type":"input_text","text":"hi"}]}])") && seen.body.contains(R"("store":false)") &&
               seen.body.contains(R"("stream":true)") && seen.body.contains(R"("model":"model-b")"));
        expect(!seen.body.contains("max_tokens"));
        expect(seen.headers.at("Authorization") == "Bearer token-b" && seen.headers.at("content-type") == "application/json");
        expect(!seen.headers.contains("anthropic-beta") && !seen.headers.contains("anthropic-version"));
    };

    "capability_a_responses_provider_with_an_explicit_path_keeps_it"_test = [] {
        duo d{[](gateway::config& built) {
            built.providers["b"].protocol = gateway::wire_protocol::openai_responses;
            built.providers["b"].messages_path = "/v1/responses";
        }};

        (void)d.chat_with("claude-model-b@b", true);

        expect(d.second.seen.at(0).target == "/v1/responses");
    };

    "limit_a_responses_provider_refuses_a_request_that_is_not_streamed_without_calling_upstream"_test = [] {
        duo d{[](gateway::config& built) { built.providers["b"].protocol = gateway::wire_protocol::openai_responses; }};

        auto const answer = d.chat_with("claude-model-b@b", false);

        expect(answer.status == 400 && answer.body.contains("stream") && d.second.seen.empty());
    };

    "limit_a_responses_provider_refuses_a_server_tool_with_the_recognised_wording_before_sending"_test = [] {
        duo d{[](gateway::config& built) { built.providers["b"].protocol = gateway::wire_protocol::openai_responses; }};
        auto spec = messages_request(R"({"model":"claude-model-b@b","max_tokens":8,"stream":true,"messages":[{"role":"user","content":"hi"}],"tools":[{"type":"advisor_20260301","name":"advisor"}]})",
                                     "/anthropic/v1/messages", "");

        auto const answer = d.ask(std::move(spec));

        expect(answer.status == 400 && answer.body.contains("Input tag 'advisor_20260301'") && d.second.seen.empty());
    };

    "capability_the_statistics_count_fields_dropped_from_a_translated_request"_test = [] {
        duo d{[](gateway::config& built) { built.providers["b"].protocol = gateway::wire_protocol::openai_responses; }};
        auto spec = messages_request(R"({"model":"claude-model-b@b","max_tokens":8,"temperature":0.5,"stream":true,"messages":[{"role":"user","content":"hi"}]})", "/anthropic/v1/messages", "");

        (void)d.ask(std::move(spec));
        auto const body = d.stats().body;

        expect(body.contains(R"json("dropped_fields":{"max_tokens":1,"temperature":1})json")) << body;
    };

    "limit_a_responses_provider_cannot_forward_the_clients_credentials"_test = [] {
        gateway::config built;
        built.providers["a"] = {.base_url = "http://127.0.0.1:1", .protocol = gateway::wire_protocol::openai_responses, .forward_credentials = true};

        expect(gateway::config_problem(built).contains("provider a") && gateway::config_problem(built).contains("forward_credentials"));
    };

    "capability_an_error_the_gateway_makes_carries_a_request_id_in_header_and_body"_test = [] {
        world w;
        auto request = messages_request("");
        request.method = "GET";
        request.target = "/no/such/path";

        auto const answer = w.ask(std::move(request));

        expect(answer.status == 404);
        auto const id = answer.headers.contains("request-id") ? answer.headers.at("request-id") : std::string{};
        expect(id.starts_with("req_") && id.size() > 8) << id;
        expect(answer.body.contains(std::format(R"("request_id":"{}")", id))) << answer.body;
        expect(answer.body.contains(R"("type":"not_found_error")"));
    };

    "capability_each_gateway_made_error_gets_its_own_request_id"_test = [] {
        world w;
        auto first = messages_request("");
        first.method = "GET";
        first.target = "/nope";
        auto second = messages_request("");
        second.method = "GET";
        second.target = "/nope";

        auto const one = w.ask(std::move(first)).headers.at("request-id");
        auto const two = w.ask(std::move(second)).headers.at("request-id");

        expect(one != two);
    };

    "capability_a_refusal_made_while_building_the_upstream_request_carries_a_request_id"_test = [] {
        duo d{[](gateway::config& built) { built.providers["b"].protocol = gateway::wire_protocol::openai_responses; }};

        auto const answer = d.chat_with("claude-model-b@b", false);

        expect(answer.status == 400 && answer.headers.contains("request-id") && answer.body.contains("request_id")) << answer.body;
    };

    "limit_an_error_relayed_from_the_upstream_is_not_stamped_by_the_gateway"_test = [] {
        duo d;
        d.first.mode = behaviour::rate_limited;
        d.second.mode = behaviour::rate_limited;

        auto const answer = d.chat_with("opus");

        expect(answer.status == 429);
        expect(!answer.headers.contains("request-id") && !answer.body.contains("request_id")) << answer.body;
    };

    "capability_a_200_reply_that_is_not_a_message_becomes_a_502_anthropic_error"_test = [] {
        world w;
        w.upstream.mode = behaviour::not_a_message;

        auto const answer = w.ask(messages_request(chat("opus", false)));

        expect(answer.status == 502) << answer.status;
        expect(answer.body.contains(R"("type":"api_error")") && answer.body.contains("not a Message") && answer.body.contains("request_id")) << answer.body;
        expect(answer.headers.contains("request-id"));
    };

    "capability_an_unusable_200_reply_falls_back_to_the_next_provider_and_counts_against_the_first"_test = [] {
        duo d;
        d.first.mode = behaviour::not_a_message;

        auto const answer = d.chat_with("opus");

        expect(answer.status == 200 && d.second.seen.size() == 1_u);
        expect(d.counts("a").failures == 1_u);
    };

    "limit_a_real_message_reply_is_still_relayed_untouched"_test = [] {
        world w;

        auto const answer = w.ask(messages_request(chat("opus", false)));

        expect(answer.status == 200 && answer.body.contains(R"("type":"message")"));
    };

    "capability_thinking_blocks_are_stripped_when_the_session_moves_to_another_provider"_test = [] {
        duo d;

        (void)d.ask(in_session(chat("claude-model-a@a", false), "sw-1"));
        auto const answer = d.ask(in_session(thinking_history("claude-model-b@b"), "sw-1"));

        expect(answer.status == 200 && d.second.seen.size() == 1_u);
        expect(!d.second.seen.at(0).body.contains("signature") && !d.second.seen.at(0).body.contains("sig-from-a")) << d.second.seen.at(0).body;
        expect(d.second.seen.at(0).body.contains(R"("text":"ok")"));
        expect(d.stats().body.contains(R"("thinking_blocks_stripped":1)"));
    };

    "limit_thinking_stays_when_the_same_provider_answers_again"_test = [] {
        duo d;

        (void)d.ask(in_session(chat("claude-model-b@b", false), "sw-2"));
        (void)d.ask(in_session(thinking_history("claude-model-b@b"), "sw-2"));

        expect(d.second.seen.size() == 2_u && d.second.seen.at(1).body.contains("sig-from-a"));
        expect(d.stats().body.contains(R"("thinking_blocks_stripped":0)"));
    };

    "limit_a_session_the_gateway_has_not_seen_and_a_request_without_a_session_keep_their_history"_test = [] {
        duo d;

        (void)d.ask(in_session(thinking_history("claude-model-b@b"), "never-seen"));
        (void)d.ask(in_session(thinking_history("claude-model-b@b"), ""));

        expect(d.second.seen.size() == 2_u);
        expect(d.second.seen.at(0).body.contains("sig-from-a") && d.second.seen.at(1).body.contains("sig-from-a"));
    };

    "capability_each_attempt_is_judged_against_the_provider_that_answered_last"_test = [] {
        duo d;
        d.first.mode = behaviour::server_error;
        (void)d.ask(in_session(chat("claude-model-b@b", false), "sw-3"));

        (void)d.ask(in_session(thinking_history("opus"), "sw-3"));

        expect(d.first.seen.size() == 1_u && !d.first.seen.at(0).body.contains("sig-from-a"));
        expect(d.second.seen.size() == 2_u && d.second.seen.at(1).body.contains("sig-from-a"));
    };

    "capability_the_ledger_line_names_the_stop_reason_the_traffic_kind_and_the_error"_test = [] {
        world w;
        std::vector<std::string> lines;
        w.gateway.persist_usage_with([&lines](std::string const& line) { lines.push_back(line); });
        w.upstream.mode = behaviour::sse_clean;

        (void)w.ask(messages_request(chat("opus", true)));
        (void)w.ask(messages_request(R"({"model":"opus","max_tokens":8,"stream":true,"stop_sequences":["</block>"],"messages":[{"role":"user","content":"hi"}]})"));
        w.upstream.mode = behaviour::rate_limited;
        (void)w.ask(messages_request(chat("opus", false)));

        expect(lines.size() == 3_u);
        auto const normal = usage::from_line(lines.at(0));
        auto const classifier = usage::from_line(lines.at(1));
        auto const limited = usage::from_line(lines.at(2));
        expect(normal && normal->kind == "main" && normal->stop_reason == "end_turn" && normal->error.empty()) << lines.at(0);
        expect(classifier && classifier->kind == "classifier") << lines.at(1);
        expect(limited && limited->status == 429 && limited->error == "rate_limit_error") << lines.at(2);
    };

    "capability_a_classifier_reply_that_is_not_a_verdict_is_counted_per_provider_and_model"_test = [] {
        world w;
        std::vector<std::string> lines;
        w.gateway.persist_usage_with([&lines](std::string const& line) { lines.push_back(line); });
        w.upstream.mode = behaviour::sse_clean;
        auto const classifier = R"({"model":"opus","max_tokens":8,"stream":true,"stop_sequences":["</block>"],"messages":[{"role":"user","content":"hi"}]})";

        (void)w.ask(messages_request(classifier));
        (void)w.ask(messages_request(chat("opus", true)));
        auto stats_request = messages_request("");
        stats_request.method = "GET";
        stats_request.target = "/llmgate/stats";
        auto const body = w.ask(std::move(stats_request)).body;

        auto const entry = usage::from_line(lines.at(0));
        expect(entry && entry->kind == "classifier" && entry->verdict == "unparseable") << lines.at(0);
        auto const normal = usage::from_line(lines.at(1));
        expect(normal && normal->verdict.empty());
        expect(body.contains(R"json("classifier_requests":{"default/opus":1})json") && body.contains(R"json("classifier_unparseable":{"default/opus":1})json")) << body;
    };

    "capability_with_capture_on_each_reply_is_written_with_its_request_redacted_headers_and_the_raw_stream"_test = [] {
        world w;
        std::vector<std::string> lines;
        w.gateway.capture_with([&lines](std::string const& line) { lines.push_back(line); });
        w.upstream.mode = behaviour::sse_clean;

        (void)w.ask(messages_request(chat_with_session("opus", true)));

        expect(lines.size() == 1_u);
        auto const parsed = json::parse(lines.at(0), {.jsonc = false});
        expect(parsed.has_value());
        expect(json::text_at(*parsed, {"provider"}) == "default" && json::text_at(*parsed, {"kind"}) == "main" && json::count_at(*parsed, {"status"}).value_or(0) == 200_u);
        expect(json::text_at(*parsed, {"request_body"}).contains(R"("model":"opus")"));
        expect(json::text_at(*parsed, {"request_headers", "x-api-key"}) == "<redacted>") << lines.at(0);
        expect(json::text_at(*parsed, {"reply_body"}).contains("event: message_start") && json::text_at(*parsed, {"reply_body"}).contains("event: message_stop"));
        expect(json::text_at(*parsed, {"reply_headers", "content-type"}).starts_with("text/event-stream") || json::text_at(*parsed, {"reply_headers", "Content-Type"}).starts_with("text/event-stream"));
    };

    "capability_a_failed_exchange_is_captured_with_the_upstreams_error_body"_test = [] {
        world w;
        std::vector<std::string> lines;
        w.gateway.capture_with([&lines](std::string const& line) { lines.push_back(line); });
        w.upstream.mode = behaviour::rate_limited;

        (void)w.ask(messages_request(chat("opus", false)));

        expect(lines.size() == 1_u);
        auto const parsed = json::parse(lines.at(0), {.jsonc = false});
        expect(parsed.has_value() && json::count_at(*parsed, {"status"}).value_or(0) == 429_u && json::text_at(*parsed, {"reply_body"}).contains("rate_limit_error")) << lines.at(0);
    };

    "limit_without_a_capture_sink_nothing_is_captured_and_replies_are_unchanged"_test = [] {
        world w;

        auto const answer = w.ask(messages_request(chat("opus", false)));

        expect(answer.status == 200 && answer.body.contains(R"("type":"message")"));
    };

    "capability_a_picked_target_goes_to_exactly_that_provider_with_its_upstream_model"_test = [] {
        duo d;

        auto const answer = d.chat_with("claude-model-b@b");

        expect(answer.status == 200);
        expect(d.first.seen.empty());
        expect(d.second.seen.at(0).body.contains(R"("model":"model-b")"));
    };

    "limit_a_picked_target_that_fails_is_not_replaced_by_another_provider"_test = [] {
        duo d;
        d.second.mode = behaviour::server_error;

        auto const answer = d.chat_with("claude-model-b@b");

        expect(answer.status == 502);
        expect(d.first.seen.empty());
    };

    "capability_a_global_remap_sends_the_role_to_the_pinned_provider_without_fallback"_test = [] {
        duo d;
        control::overrides pins;
        pins.global["opus"] = {.provider = "b", .upstream = "model-extra"};
        d.gateway.use_overrides(pins);

        auto const answer = d.chat_with("opus");

        expect(answer.status == 200 && d.first.seen.empty());
        expect(d.second.seen.at(0).body.contains(R"("model":"model-extra")"));
    };

    "capability_a_session_remap_follows_the_claude_code_session_header_only"_test = [] {
        duo d;
        control::overrides pins;
        pins.session["session-1"]["opus"] = {.provider = "b", .upstream = "model-b"};
        d.gateway.use_overrides(pins);
        auto mine = messages_request(chat("opus", false), "/anthropic/v1/messages", "");
        mine.fields.add("x-claude-code-session-id", "session-1");
        auto other = messages_request(chat("opus", false), "/anthropic/v1/messages", "");
        other.fields.add("x-claude-code-session-id", "session-2");

        (void)d.ask(std::move(mine));
        (void)d.ask(std::move(other));

        expect(d.second.seen.size() == 1_u && d.first.seen.size() == 1_u);
    };

    "capability_the_models_endpoint_lists_every_target_with_a_claude_id_and_a_display_name"_test = [] {
        duo d;
        auto request = messages_request("");
        request.method = "GET";
        request.target = "/anthropic/v1/models";

        auto const body = d.ask(std::move(request)).body;

        expect(body.contains(R"("id":"claude-model-a@a")") && body.contains(R"("id":"claude-model-b@b")") && body.contains(R"("id":"claude-model-extra@b")"));
        expect(body.contains(R"json("display_name":"model-b (b)")json"));
    };

    "capability_a_clean_stream_passes_through_event_for_event"_test = [] {
        world w;
        w.upstream.mode = behaviour::sse_clean;

        auto const answer = w.ask(messages_request(chat("opus", true)));

        expect(answer.status == 200);
        expect(event_names(answer.body) == complete_stream);
        expect(std::ranges::all_of(reflect::enum_values<anthropic::repair_kind>, [&](auto const kind) { return gateway::repair_slot(w.gateway.counters().repairs, kind) == 0; }));
    };

    "defect_repeated_message_start_from_the_upstream_reaches_the_client_once"_test = [] {
        world w;
        w.upstream.mode = behaviour::sse_duplicate_start;

        auto const answer = w.ask(messages_request(chat("opus", true)));

        expect(event_names(answer.body) == complete_stream);
        expect(gateway::repair_slot(w.gateway.counters().repairs, anthropic::repair_kind::duplicate_message_start) == 2_u);
    };

    "defect_a_stream_with_no_message_start_gets_one"_test = [] {
        world w;
        w.upstream.mode = behaviour::sse_missing_start;

        auto const answer = w.ask(messages_request(chat("opus", true)));

        expect(event_names(answer.body) == complete_stream);
        expect(gateway::repair_slot(w.gateway.counters().repairs, anthropic::repair_kind::missing_message_start) == 1_u);
    };

    "defect_a_block_the_upstream_never_closed_is_closed_for_the_client"_test = [] {
        world w;
        w.upstream.mode = behaviour::sse_unclosed_tool_block;

        auto const answer = w.ask(messages_request(chat("opus", true)));

        expect(event_names(answer.body) == complete_stream);
        expect(gateway::repair_slot(w.gateway.counters().repairs, anthropic::repair_kind::unclosed_block) == 1_u);
    };

    "limit_an_upstream_that_dies_mid_stream_ends_the_client_stream_with_an_error_event"_test = [] {
        world w;
        w.upstream.mode = behaviour::sse_truncated;

        auto const answer = w.ask(messages_request(chat("opus", true)));

        auto const names = event_names(answer.body);
        expect(names.back() == "error");
        expect(!std::ranges::contains(names, std::string{"message_stop"}));
        expect(gateway::repair_slot(w.gateway.counters().repairs, anthropic::repair_kind::truncated_stream) == 1_u);
    };

    "limit_an_upstream_that_goes_silent_is_cut_at_the_stream_idle_limit"_test = [] {
        world w;
        w.upstream.mode = behaviour::sse_stalls;
        auto const started = std::chrono::steady_clock::now();

        auto const answer = w.ask(messages_request(chat("opus", true)));

        expect(event_names(answer.body).back() == "error");
        expect(std::chrono::steady_clock::now() - started < std::chrono::seconds{5});
        expect(w.gateway.counters().upstream_timeouts == 1_u);
    };

    "defect_a_write_to_the_client_that_fails_counts_as_a_disconnect"_test = [] {
        world w;
        w.upstream.mode = behaviour::sse_endless;
        auto server = net::listener::bind(w.loop, std::string{loopback}, 0);
        w.loop.spawn(serve_failing_writes(w, *server, 2));

        auto const counted = w.loop.run([](world& world, std::uint16_t const port) -> async::task<bool> {
            auto client = co_await net::tcp_stream::connect(world.loop, std::string{loopback}, port);
            auto spec = messages_request(chat("opus", true));
            spec.host = std::format("127.0.0.1:{}", world.port());
            auto const wire = httpio::format_request(spec);
            (void)co_await client->write_all(std::as_bytes(std::span{*wire}));
            co_return co_await eventually(world.loop, [&world] { return world.gateway.counters().client_disconnects == 1; });
        }(w, server->port()));

        expect(counted);
    };

    "limit_a_client_that_disconnects_mid_stream_stops_the_upstream_read"_test = [] {
        world w;
        w.upstream.mode = behaviour::sse_endless;

        auto const stopped = w.loop.run([](world& world) -> async::task<bool> {
            {
                auto client = co_await net::tcp_stream::connect(world.loop, std::string{loopback}, world.port());
                auto spec = messages_request(chat("opus", true));
                spec.host = std::format("127.0.0.1:{}", world.port());
                auto const wire = httpio::format_request(spec);
                (void)co_await client->write_all(std::as_bytes(std::span{*wire}));
                std::array<std::byte, 64> first{};
                (void)co_await client->read_some(first, {});
            }
            co_return co_await eventually(world.loop, [&world] { return world.gateway.counters().client_disconnects == 1; });
        }(w));

        expect(stopped);
        expect(w.gateway.counters().upstream_timeouts == 0_u);
    };

    "capability_an_upstream_error_status_and_body_reach_the_client"_test = [] {
        world w;
        w.upstream.mode = behaviour::rate_limited;

        auto const answer = w.ask(messages_request(chat("opus", false)));

        expect(answer.status == 429);
        expect(answer.body.contains("rate_limit_error"));
    };

    "capability_back_to_back_requests_reuse_one_upstream_connection"_test = [] {
        world w;

        (void)w.ask(messages_request(chat("opus", false)));
        (void)w.ask(messages_request(chat("opus", false)));

        expect(w.upstream.connections == 1);
        expect(w.gateway.counters().connections_reused == 1_u);
    };

    "capability_back_to_back_streamed_requests_reuse_one_upstream_connection"_test = [] {
        world w;
        w.upstream.mode = behaviour::sse_clean;

        (void)w.ask(messages_request(chat("opus", true)));
        (void)w.ask(messages_request(chat("opus", true)));

        expect(w.upstream.connections == 1);
        expect(w.gateway.counters().connections_reused == 1_u);
    };

    "limit_a_stream_cut_off_by_the_upstream_is_not_reused"_test = [] {
        world w;
        w.upstream.mode = behaviour::sse_truncated;
        (void)w.ask(messages_request(chat("opus", true)));
        w.upstream.mode = behaviour::sse_clean;

        (void)w.ask(messages_request(chat("opus", true)));

        expect(w.upstream.connections == 2);
        expect(w.gateway.counters().connections_reused == 0_u);
    };

    "limit_an_unreachable_upstream_is_reported_as_502"_test = [] {
        world w;
        auto const gone = net::listener::bind(w.loop, std::string{loopback}, 0)->port();
        gateway::static_credential credential{"t"};
        auto server = net::listener::bind(w.loop, std::string{loopback}, 0);
        gateway::proxy broken{w.loop, world::settings(gone, {}), {.allowed_hosts = routing::default_allowed_hosts(loopback, server->port())},
                              credential, w.tls_context};
        w.loop.spawn(httpio::serve(*server, broken, httpio::limits{}));
        auto request = messages_request(chat("opus", false));
        request.host = std::format("127.0.0.1:{}", server->port());

        auto const answer = w.loop.run(call(w.loop, server->port(), std::move(request)));

        expect(answer.status == 502);
        expect(answer.body.contains("api_error"));
    };

    "limit_a_request_without_a_credential_is_refused_with_401"_test = [] {
        world w{""};

        auto const answer = w.ask(messages_request(chat("opus", false)));

        expect(answer.status == 401);
        expect(w.upstream.seen.empty());
    };

    "limit_a_body_that_is_not_json_or_has_no_model_is_refused_with_400"_test = [] {
        world w;

        expect(w.ask(messages_request("not json")).status == 400);
        expect(w.ask(messages_request(R"({"max_tokens":1})")).status == 400);
        expect(w.upstream.seen.empty());
    };

    "defect_conflicting_duplicate_model_members_cannot_slip_an_alias_past_the_mapping"_test = [] {
        world w;

        auto const answer = w.ask(messages_request(R"({"model":"claude-direct","model":"opus","max_tokens":1})"));

        expect(answer.status == 400);
        expect(w.upstream.seen.empty());
    };

    "capability_agreeing_duplicate_model_members_are_all_mapped"_test = [] {
        world w;

        auto const answer = w.ask(messages_request(R"({"model":"opus","model":"opus","max_tokens":1})"));

        expect(answer.status == 200);
        expect(w.upstream.seen[0].body == R"({"model":"claude-opus-upstream","model":"claude-opus-upstream","max_tokens":1})");
    };

    "defect_a_host_header_for_another_name_is_refused_so_dns_rebinding_cannot_use_the_credential"_test = [] {
        world w;

        auto const rebound = w.ask_as(messages_request(chat("opus", false)), std::format("evil.example:{}", w.port()));
        auto const lowercase = w.ask_as(messages_request(chat("opus", false)), std::format("LOCALHOST:{}", w.port()));

        expect(rebound.status == 403);
        expect(rebound.body.contains("permission_error"));
        expect(lowercase.status == 200);
        expect(w.upstream.seen.size() == 1_u);
        expect(w.gateway.counters().refused == 1_u);
    };

    "limit_with_a_client_key_configured_requests_without_it_are_refused"_test = [] {
        world w{"upstream-token", "local-secret"};
        auto with_header = messages_request(chat("opus", false), "/anthropic/v1/messages", "local-secret");
        auto with_bearer = messages_request(chat("opus", false), "/anthropic/v1/messages", "");
        with_bearer.fields.add("Authorization", "Bearer local-secret");
        auto wrong = messages_request(chat("opus", false), "/anthropic/v1/messages", "local-secrex");

        expect(w.ask(messages_request(chat("opus", false), "/anthropic/v1/messages", "")).status == 401);
        expect(w.ask(std::move(wrong)).status == 401);
        expect(w.ask(std::move(with_header)).status == 200);
        expect(w.ask(std::move(with_bearer)).status == 200);
        expect(w.upstream.seen.size() == 2_u);
    };

    "limit_the_health_check_stays_open_when_a_client_key_is_configured"_test = [] {
        world w{"upstream-token", "local-secret"};
        auto health = messages_request("");
        health.method = "GET";
        health.target = "/healthz";

        expect(w.ask(std::move(health)).status == 200);
    };

    "capability_the_health_check_proves_the_gateways_identity_for_a_challenge"_test = [] {
        world w{"upstream-token", "", std::nullopt, "identity-secret"};
        auto health = messages_request("");
        health.method = "GET";
        health.target = "/healthz?challenge=0123abcd";

        auto const answer = w.ask(std::move(health));

        expect(answer.status == 200);
        expect(answer.body == std::format(R"({{"status":"ok","proof":"{}"}})", proof::hmac_sha256_hex("identity-secret", proof::identity_message("scope-a", "0123abcd"))));
    };

    "limit_a_proof_for_one_address_does_not_verify_for_another"_test = [] {
        world w{"upstream-token", "", std::nullopt, "identity-secret"};
        auto health = messages_request("");
        health.method = "GET";
        health.target = "/healthz?challenge=0123abcd";

        auto const answer = w.ask(std::move(health));

        expect(!answer.body.contains(proof::hmac_sha256_hex("identity-secret", proof::identity_message("scope-b", "0123abcd"))));
        expect(!answer.body.contains(proof::hmac_sha256_hex("identity-secret", "0123abcd")));
    };

    "limit_a_bad_challenge_or_no_identity_gives_the_plain_health_answer"_test = [] {
        world with_identity{"upstream-token", "", std::nullopt, "identity-secret"};
        world without_identity;
        auto bad = messages_request("");
        bad.method = "GET";
        bad.target = "/healthz?challenge=not-hex";
        auto plain = bad;
        plain.target = "/healthz?challenge=0123abcd";

        expect(with_identity.ask(std::move(bad)).body == R"({"status":"ok"})");
        expect(without_identity.ask(std::move(plain)).body == R"({"status":"ok"})");
    };

    "limit_unknown_paths_and_wrong_methods_are_refused"_test = [] {
        world w;

        auto get_messages = messages_request("");
        get_messages.method = "GET";

        expect(w.ask(messages_request("{}", "/nope")).status == 404);
        expect(w.ask(std::move(get_messages)).status == 405);
    };

    "capability_the_models_endpoint_lists_the_aliases"_test = [] {
        world w;
        auto request = messages_request("");
        request.method = "GET";
        request.target = "/anthropic/v1/models";

        auto const answer = w.ask(std::move(request));

        expect(answer.status == 200);
        expect(answer.body.contains(R"("id":"opus")"));
    };

    "capability_token_counting_returns_a_flagged_estimate"_test = [] {
        world w;

        auto const answer = w.ask(messages_request(chat("opus", false), "/anthropic/v1/messages/count_tokens"));

        expect(answer.status == 200);
        expect(answer.headers.at("X-Llmgate-Estimated") == "true");
        expect(answer.body.starts_with(R"({"input_tokens":)"));
    };

    "capability_health_and_stats_endpoints_answer"_test = [] {
        world w;
        auto health = messages_request("");
        health.method = "GET";
        health.target = "/healthz";
        auto statistics = health;
        statistics.target = "/llmgate/stats";

        expect(w.ask(std::move(health)).body == R"({"status":"ok"})");
        expect(w.ask(std::move(statistics)).body.contains(R"("requests":2)"));
    };

    "capability_a_request_goes_to_the_first_provider_with_its_own_credential_headers_and_model_name"_test = [] {
        duo d;
        std::vector<std::string> lines;
        d.gateway.persist_usage_with([&lines](std::string const& line) { lines.push_back(line); });

        auto const answer = d.chat_with("opus");

        expect(answer.status == 200 && answer.headers.at("Request-Id") == "req_1");
        expect(d.first.seen.size() == 1_u && d.second.seen.empty());
        expect(d.first.seen[0].headers.at("Authorization") == "Bearer token-a" && d.first.seen[0].headers.at("Editor-Version") == "a/1.0");
        expect(d.first.seen[0].body.contains(R"("model":"model-a")"));
        expect(lines.size() == 1_u && usage::from_line(lines.at(0))->provider == "a");
    };

    "capability_a_502_before_any_byte_falls_back_to_the_next_provider"_test = [] {
        duo d;
        std::vector<std::string> lines;
        d.gateway.persist_usage_with([&lines](std::string const& line) { lines.push_back(line); });
        d.first.mode = behaviour::server_error;

        auto const answer = d.chat_with("opus");

        expect(answer.status == 200 && answer.headers.at("Request-Id") == "from-b");
        expect(d.second.seen.at(0).body.contains(R"("model":"model-b")"));
        expect(d.second.seen.at(0).headers.at("Authorization") == "Bearer token-b");
        expect(d.counts("a").failures == 1_u && d.counts("b").answered == 1_u && d.counts("a").answered == 0_u);
        expect(lines.size() == 1_u);
        auto const entry = usage::from_line(lines.at(0));
        expect(entry->provider == "b" && entry->status == 200);
    };

    "capability_a_429_before_any_byte_falls_back_to_the_next_provider"_test = [] {
        duo d;
        d.first.mode = behaviour::rate_limited;

        auto const answer = d.chat_with("opus");

        expect(answer.status == 200 && d.second.seen.size() == 1_u);
        expect(d.counts("a").failures == 1_u);
    };

    "capability_a_timeout_waiting_for_the_response_head_falls_back_to_the_next_provider"_test = [] {
        duo d;
        d.first.mode = behaviour::silent;

        auto const answer = d.chat_with("opus");

        expect(answer.status == 200 && answer.headers.at("Request-Id") == "from-b");
        expect(d.first.seen.size() == 1_u);
        expect(d.gateway.counters().upstream_timeouts == 1_u && d.counts("a").failures == 1_u);
    };

    "capability_a_refused_connection_falls_back_to_the_next_provider"_test = [] {
        async::reactor scratch;
        auto const gone = net::listener::bind(scratch, std::string{loopback}, 0)->port();
        duo d{[](gateway::config&) {}, "token-a", gone};

        auto const answer = d.chat_with("opus");

        expect(answer.status == 200 && answer.headers.at("Request-Id") == "from-b");
        expect(d.gateway.counters().upstream_failures == 1_u && d.counts("a").failures == 1_u);
    };

    "limit_a_stream_that_began_is_never_retried_on_another_provider"_test = [] {
        duo d;
        d.first.mode = behaviour::sse_truncated;

        auto const answer = d.chat_with("opus", true);

        expect(event_names(answer.body).back() == "error");
        expect(d.second.seen.empty());
        expect(d.counts("a").failures == 1_u && d.counts("b").attempts == 0_u);
    };

    "limit_a_client_error_other_than_429_is_delivered_and_not_retried"_test = [] {
        duo d;
        d.first.mode = behaviour::bad_request;

        auto const answer = d.chat_with("opus");

        expect(answer.status == 400 && d.second.seen.empty());
        expect(d.counts("a").answered == 1_u && d.counts("a").failures == 0_u);
    };

    "limit_when_every_provider_fails_the_last_failure_reaches_the_client_and_one_line_is_written"_test = [] {
        duo d;
        std::vector<std::string> lines;
        d.gateway.persist_usage_with([&lines](std::string const& line) { lines.push_back(line); });
        d.first.mode = behaviour::server_error;
        d.second.mode = behaviour::rate_limited;

        auto const answer = d.chat_with("opus");

        expect(answer.status == 429 && answer.body.contains("rate_limit_error"));
        expect(d.first.seen.size() == 1_u && d.second.seen.size() == 1_u);
        expect(lines.size() == 1_u);
        auto const entry = usage::from_line(lines.at(0));
        expect(entry->provider == "b" && entry->status == 429 && entry->used == usage::tokens{});
    };

    "capability_a_breaker_opens_after_the_threshold_is_skipped_and_closes_after_a_good_probe"_test = [] {
        duo d;
        d.first.mode = behaviour::server_error;

        (void)d.chat_with("opus");
        (void)d.chat_with("opus");
        auto const skipped = d.chat_with("opus");

        expect(skipped.status == 200 && d.first.seen.size() == 2_u && d.second.seen.size() == 3_u);
        expect(d.counts("a").skipped == 1_u);
        expect(d.stats().body.contains(R"("a":{"attempts":2,"answered":0,"failures":2,"skipped":1,"circuit":"open"})"));

        d.now_ms += duo_cooldown.count();
        d.first.mode = behaviour::json_ok;
        auto const probe = d.chat_with("opus");
        auto const after = d.chat_with("opus");

        expect(probe.headers.at("Request-Id") == "req_1" && after.headers.at("Request-Id") == "req_1");
        expect(d.first.seen.size() == 4_u);
        expect(d.stats().body.contains(R"("circuit":"closed")"));
    };

    "limit_a_failed_probe_reopens_the_breaker_for_another_cooldown"_test = [] {
        duo d;
        d.first.mode = behaviour::server_error;
        (void)d.chat_with("opus");
        (void)d.chat_with("opus");

        d.now_ms += duo_cooldown.count();
        (void)d.chat_with("opus");
        (void)d.chat_with("opus");

        expect(d.first.seen.size() == 3_u);
        expect(d.counts("a").skipped == 1_u);
        d.now_ms += duo_cooldown.count();
        (void)d.chat_with("opus");
        expect(d.first.seen.size() == 4_u);
    };

    "limit_when_every_breaker_is_open_the_client_gets_503_and_no_upstream_is_called"_test = [] {
        duo d;
        d.first.mode = behaviour::server_error;
        d.second.mode = behaviour::server_error;
        (void)d.chat_with("opus");
        (void)d.chat_with("opus");
        auto const seen_before = d.first.seen.size() + d.second.seen.size();

        auto const answer = d.chat_with("opus");

        expect(answer.status == 503 && answer.body.contains("circuit open"));
        expect(d.first.seen.size() + d.second.seen.size() == seen_before);
        expect(d.gateway.counters().no_provider == 1_u);
    };

    "limit_missing_credentials_fall_through_without_opening_the_breaker"_test = [] {
        duo d{[](gateway::config&) {}, ""};

        for (int round = 0; round < 3; ++round) {
            expect(d.chat_with("opus").status == 200);
        }

        expect(d.first.seen.empty() && d.second.seen.size() == 3_u);
        expect(d.counts("a").failures == 0_u && d.counts("a").skipped == 0_u);
    };

    "capability_each_role_follows_its_own_route_and_other_covers_the_rest"_test = [] {
        duo d{[](gateway::config& settings) {
            route(settings, "opus", {"b", "a"});
            route(settings, "other", {"a"});
        }};

        (void)d.chat_with("opus");
        (void)d.chat_with("claude-sonnet-x");
        d.second.mode = behaviour::server_error;
        auto const fell_back = d.chat_with("opus");

        expect(d.second.seen.size() == 2_u && d.first.seen.size() == 2_u);
        expect(d.first.seen.at(0).body.contains("claude-sonnet-x"));
        expect(fell_back.status == 200 && fell_back.headers.at("Request-Id") == "req_1");
    };

    "capability_the_models_list_is_the_union_of_every_providers_aliases"_test = [] {
        duo d;
        auto request = messages_request("");
        request.method = "GET";
        request.target = "/anthropic/v1/models";

        auto const answer = d.ask(std::move(request));

        expect(answer.body.contains(R"("id":"opus")") && answer.body.contains(R"("id":"extra")"));
    };

    "capability_a_single_upstream_config_is_served_as_the_provider_named_default"_test = [] {
        world w;

        (void)w.ask(messages_request(chat("opus", false)));
        auto stats = messages_request("");
        stats.method = "GET";
        stats.target = "/llmgate/stats";
        auto const body = w.ask(std::move(stats)).body;

        expect(w.gateway.counters().providers.at("default").answered == 1_u);
        expect(body.contains(R"("providers":{"default":{"attempts":1,"answered":1,"failures":0,"skipped":0,"circuit":"closed"}})"));
    };

    "capability_the_single_upstream_keys_become_an_implicit_default_provider"_test = [] {
        gateway::config legacy{.upstream = {.base_url = "http://127.0.0.1:1", .messages_path = "/m"}, .models = {{"opus", "x"}}, .static_token_env = "T"};

        auto const providers = gateway::effective_providers(legacy);

        expect(providers.size() == 1_u);
        expect(providers.at("default").base_url == "http://127.0.0.1:1" && providers.at("default").messages_path == "/m");
        expect(providers.at("default").models.at("opus") == "x" && providers.at("default").static_token_env == "T");
        expect(gateway::config_problem(legacy).empty());
    };

    "limit_a_configuration_is_refused_for_missing_providers_bad_urls_and_bad_routes"_test = [] {
        gateway::config none{.listen = "127.0.0.1:0"};
        gateway::config bad_url;
        bad_url.providers["a"] = {.base_url = "ftp://x"};
        gateway::config bad_route;
        bad_route.providers["a"] = {.base_url = "http://127.0.0.1:1"};
        route(bad_route, "opus", {"zzz"});

        expect(gateway::config_problem(none).contains("providers"));
        expect(gateway::config_problem(bad_url).contains("provider a") && gateway::config_problem(bad_url).contains("http"));
        expect(gateway::config_problem(bad_route).contains("unknown provider zzz"));
    };

    "capability_the_configuration_reader_accepts_providers_routes_and_the_breaker_settings"_test = [] {
        constexpr std::string_view text = R"({
            "listen": "127.0.0.1:9480",
            "providers": {
                "copilot": { "base_url": "https://api.githubcopilot.com", "copilot": { "token_file": "t.json" }, "models": { "opus": "o" } },
                "direct": { "base_url": "https://api.anthropic.com", "static_token_env": "KEY", "headers": { "X": "1" } }
            },
            "routes": { "opus": ["direct", "copilot"], "other": ["copilot"] },
            "fallback": { "failures": 5, "cooldown": "45s" }
        })";

        auto const parsed = json::read_text<gateway::config>(text);

        expect(parsed.has_value());
        expect(parsed->providers.size() == 2_u && parsed->providers.at("copilot").copilot.has_value() && parsed->providers.at("direct").headers.at("X") == "1");
        expect(parsed->routes.at("opus") == std::vector<std::string>{"direct", "copilot"});
        expect(parsed->fallback.failures == 5_u && parsed->fallback.cooldown == std::chrono::seconds{45});
        expect(gateway::config_problem(*parsed).empty());
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}

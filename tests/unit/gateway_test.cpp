#include <boost/ut.hpp>

import llmgate.anthropic;
import llmgate.async;
import llmgate.gateway;
import llmgate.http;
import llmgate.httpio;
import llmgate.net;
import llmgate.sse;
import llmgate.tls;
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
    sse_clean,
    sse_duplicate_start,
    sse_missing_start,
    sse_unclosed_tool_block,
    sse_truncated,
    sse_stalls,
    sse_endless,
    rate_limited,
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
        auto const delta = std::pair{"message_delta", stream_body_event("message_delta", R"(,"delta":{},"usage":{"output_tokens":2})")};
        auto const stop = std::pair{"message_stop", stream_body_event("message_stop")};
        switch (mode) {
            case behaviour::json_ok: {
                http::header_map fields;
                fields.add("Content-Type", "application/json");
                fields.add("Request-Id", "req_1");
                co_return co_await out.send(200, fields, R"({"type":"message","content":[]})");
            }
            case behaviour::rate_limited: {
                http::header_map fields;
                fields.add("Content-Type", "application/json");
                co_return co_await out.send(429, fields, R"({"type":"error","error":{"type":"rate_limit_error"}})");
            }
            case behaviour::sse_clean: co_return co_await script(out, {start, block, text, end_block, delta, stop});
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

httpio::request_spec messages_request(std::string body, std::string target = "/anthropic/v1/messages") {
    http::header_map fields;
    fields.add("Content-Type", "application/json");
    fields.add("anthropic-version", "2023-06-01");
    fields.add("anthropic-beta", "tools-2024");
    fields.add("x-api-key", "client-secret");
    return {.method = "POST", .target = std::move(target), .host = "localhost", .fields = std::move(fields), .body = std::move(body)};
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

    static gateway::config settings(std::uint16_t const upstream_port) {
        return {.listen = "127.0.0.1:0",
                .upstream = {.base_url = std::format("http://127.0.0.1:{}", upstream_port),
                             .messages_path = "/v1/messages",
                             .headers = {{"Editor-Version", "test/1.0"}}},
                .models = {{"opus", "claude-opus-upstream"}},
                .time = {.connect = std::chrono::seconds{2}, .response_head = std::chrono::seconds{2}, .stream_idle = short_idle}};
    }

    explicit world(std::string token = "upstream-token", std::optional<std::uint64_t> const seed = std::nullopt)
        : loop{seed},
          upstream_listener{std::move(*net::listener::bind(loop, std::string{loopback}, 0))},
          gateway_listener{std::move(*net::listener::bind(loop, std::string{loopback}, 0))},
          credential{std::move(token)},
          tls_context{std::move(*tls::client_context::make(tls::group_policy::classical_only))},
          gateway{loop, settings(upstream_listener.port()), credential, tls_context} {
        upstream.loop = &loop;
        loop.spawn(accept_upstream(loop, upstream_listener, upstream));
        loop.spawn(httpio::serve(gateway_listener, gateway, httpio::limits{.max_body_bytes = 4096}));
    }

    std::uint16_t port() const noexcept { return gateway_listener.port(); }

    reply ask(httpio::request_spec spec) { return loop.run(call(loop, port(), std::move(spec))); }
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

const suite gateway_suite = [] {
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

    "capability_a_clean_stream_passes_through_event_for_event"_test = [] {
        world w;
        w.upstream.mode = behaviour::sse_clean;

        auto const answer = w.ask(messages_request(chat("opus", true)));

        expect(answer.status == 200);
        expect(event_names(answer.body) == complete_stream);
        expect(w.gateway.counters().repairs.empty());
    };

    "defect_repeated_message_start_from_the_upstream_reaches_the_client_once"_test = [] {
        world w;
        w.upstream.mode = behaviour::sse_duplicate_start;

        auto const answer = w.ask(messages_request(chat("opus", true)));

        expect(event_names(answer.body) == complete_stream);
        expect(w.gateway.counters().repairs.at(anthropic::repair_kind::duplicate_message_start) == 2_u);
    };

    "defect_a_stream_with_no_message_start_gets_one"_test = [] {
        world w;
        w.upstream.mode = behaviour::sse_missing_start;

        auto const answer = w.ask(messages_request(chat("opus", true)));

        expect(event_names(answer.body) == complete_stream);
        expect(w.gateway.counters().repairs.at(anthropic::repair_kind::missing_message_start) == 1_u);
    };

    "defect_a_block_the_upstream_never_closed_is_closed_for_the_client"_test = [] {
        world w;
        w.upstream.mode = behaviour::sse_unclosed_tool_block;

        auto const answer = w.ask(messages_request(chat("opus", true)));

        expect(event_names(answer.body) == complete_stream);
        expect(w.gateway.counters().repairs.at(anthropic::repair_kind::unclosed_block) == 1_u);
    };

    "limit_an_upstream_that_dies_mid_stream_ends_the_client_stream_with_an_error_event"_test = [] {
        world w;
        w.upstream.mode = behaviour::sse_truncated;

        auto const answer = w.ask(messages_request(chat("opus", true)));

        auto const names = event_names(answer.body);
        expect(names.back() == "error");
        expect(!std::ranges::contains(names, std::string{"message_stop"}));
        expect(w.gateway.counters().repairs.at(anthropic::repair_kind::truncated_stream) == 1_u);
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

    "limit_a_client_that_disconnects_mid_stream_stops_the_upstream_read"_test = [] {
        world w;
        w.upstream.mode = behaviour::sse_endless;

        auto const stopped = w.loop.run([](world& world) -> async::task<bool> {
            {
                auto client = co_await net::tcp_stream::connect(world.loop, std::string{loopback}, world.port());
                auto const wire = httpio::format_request(messages_request(chat("opus", true)));
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

    "limit_an_unreachable_upstream_is_reported_as_502"_test = [] {
        world w;
        auto const gone = net::listener::bind(w.loop, std::string{loopback}, 0)->port();
        gateway::static_credential credential{"t"};
        auto settings = world::settings(gone);
        gateway::proxy broken{w.loop, settings, credential, w.tls_context};
        auto server = net::listener::bind(w.loop, std::string{loopback}, 0);
        w.loop.spawn(httpio::serve(*server, broken, httpio::limits{}));

        auto const answer = w.loop.run(call(w.loop, server->port(), messages_request(chat("opus", false))));

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
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}

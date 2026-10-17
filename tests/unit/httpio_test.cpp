#include <boost/ut.hpp>

import llmgate.async;
import llmgate.http;
import llmgate.httpio;
import llmgate.net;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate;

using writer = httpio::response_writer<net::tcp_stream>;

constexpr std::string_view loopback = "127.0.0.1";
constexpr async::duration short_idle{40};
constexpr std::size_t small_body_limit = 16;
constexpr std::size_t small_head_limit = 64;

struct reply {
    int status = 0;
    bool chunked = false;
    bool complete = false;
    std::string body;
    std::error_code error;
};

auto collect_into(reply& out) {
    return [&out](http::event const& item) {
        if (auto const* head = std::get_if<http::message_head>(&item)) {
            out.status = head->status;
            out.chunked = head->chunked;
        } else if (auto const* chunk = std::get_if<http::body_chunk>(&item)) {
            out.body.append(reinterpret_cast<char const*>(chunk->bytes.data()), chunk->bytes.size());
        } else {
            out.complete = true;
        }
    };
}

struct echo_handler {
    async::task<net::result_void> operator()(httpio::request const& incoming, writer& out) {
        co_return co_await out.send(200, {}, "echo:" + incoming.body);
    }
};

struct stream_handler {
    async::task<net::result_void> operator()(httpio::request const&, writer& out) {
        http::header_map fields;
        fields.add("Content-Type", "text/event-stream");
        if (auto const begun = co_await out.begin_stream(200, fields); !begun) {
            co_return begun;
        }
        std::string const first = "data: one\n\n";
        std::string const second = "data: two\n\n";
        (void)co_await out.write_chunk(std::as_bytes(std::span{first}));
        (void)co_await out.write_chunk(std::as_bytes(std::span{second}));
        co_return co_await out.end_stream();
    }
};

struct failing_handler {
    async::task<net::result_void> operator()(httpio::request const&, writer&) {
        co_return std::unexpected{std::make_error_code(std::errc::io_error)};
    }
};

httpio::request_spec post(std::string body) {
    return {.method = "POST", .target = "/echo", .host = "localhost", .body = std::move(body)};
}

template <class Handler>
async::task<reply> single_request(async::reactor& loop, net::listener& server, Handler& handler, httpio::request_spec spec,
                                  httpio::limits bounds = {}) {
    async::spawn(httpio::serve(server, handler, bounds));
    reply result;
    auto client = co_await net::tcp_stream::connect(loop, std::string{loopback}, server.port());
    auto const done = co_await httpio::exchange(*client, std::move(spec), collect_into(result));
    result.error = done ? std::error_code{} : done.error();
    co_return result;
}

template <class Handler>
reply run_single(Handler& handler, httpio::request_spec spec, httpio::limits bounds = {}) {
    async::reactor loop;
    auto server = net::listener::bind(loop, std::string{loopback}, 0);
    return loop.run(single_request(loop, *server, handler, std::move(spec), bounds));
}

async::task<std::pair<reply, reply>> two_requests(async::reactor& loop, net::listener& server, echo_handler& handler) {
    async::spawn(httpio::serve(server, handler));
    std::pair<reply, reply> results;
    auto client = co_await net::tcp_stream::connect(loop, std::string{loopback}, server.port());
    (void)co_await httpio::exchange(*client, post("first"), collect_into(results.first));
    (void)co_await httpio::exchange(*client, post("second"), collect_into(results.second));
    co_return results;
}

async::task<std::string> raw_exchange(async::reactor& loop, net::listener& server, echo_handler& handler,
                                      std::string wire, httpio::limits bounds) {
    async::spawn(httpio::serve(server, handler, bounds));
    auto client = co_await net::tcp_stream::connect(loop, std::string{loopback}, server.port());
    (void)co_await client->write_all(std::as_bytes(std::span{wire}));
    std::string answer(32, '\0');
    auto const received = co_await client->read_some(std::as_writable_bytes(std::span{answer}), {});
    answer.resize(received.value_or(0));
    co_return answer;
}

async::task<std::size_t> silent_client(async::reactor& loop, net::listener& server, echo_handler& handler,
                                       httpio::limits bounds) {
    async::spawn(httpio::serve(server, handler, bounds));
    auto client = co_await net::tcp_stream::connect(loop, std::string{loopback}, server.port());
    std::array<std::byte, 1> buffer{};
    auto const received = co_await client->read_some(buffer, {});
    co_return received.value_or(std::numeric_limits<std::size_t>::max());
}

const suite httpio_suite = [] {
    "capability_a_request_reaches_the_handler_and_its_reply_returns"_test = [] {
        echo_handler handler;

        auto const result = run_single(handler, post("ping"));

        expect(!result.error);
        expect(result.status == 200);
        expect(result.body == "echo:ping");
        expect(result.complete);
    };

    "capability_a_streamed_reply_arrives_chunked_and_complete"_test = [] {
        stream_handler handler;

        auto const result = run_single(handler, post(""));

        expect(!result.error);
        expect(result.chunked);
        expect(result.body == "data: one\n\ndata: two\n\n");
        expect(result.complete);
    };

    "capability_one_connection_serves_two_requests"_test = [] {
        async::reactor loop;
        echo_handler handler;
        auto server = net::listener::bind(loop, std::string{loopback}, 0);

        auto const results = loop.run(two_requests(loop, *server, handler));

        expect(results.first.body == "echo:first");
        expect(results.second.body == "echo:second");
    };

    "limit_a_body_over_the_limit_is_answered_with_413"_test = [] {
        echo_handler handler;

        auto const result = run_single(handler, post(std::string(small_body_limit + 1, 'x')), {.max_body_bytes = small_body_limit});

        expect(result.status == 413);
    };

    "limit_a_head_over_the_limit_is_answered_with_431"_test = [] {
        async::reactor loop;
        echo_handler handler;
        auto server = net::listener::bind(loop, std::string{loopback}, 0);
        std::string const wire = "GET / HTTP/1.1\r\nX-Long: " + std::string(small_head_limit, 'a') + "\r\n\r\n";

        auto const answer = loop.run(raw_exchange(loop, *server, handler, wire, {.max_head_bytes = small_head_limit}));

        expect(answer.starts_with("HTTP/1.1 431"));
    };

    "limit_garbage_is_answered_with_400"_test = [] {
        async::reactor loop;
        echo_handler handler;
        auto server = net::listener::bind(loop, std::string{loopback}, 0);

        auto const answer = loop.run(raw_exchange(loop, *server, handler, "\x01 not http\r\n\r\n", {}));

        expect(answer.starts_with("HTTP/1.1 400"));
    };

    "limit_a_failing_handler_before_any_reply_is_answered_with_500"_test = [] {
        failing_handler handler;

        auto const result = run_single(handler, post(""));

        expect(result.status == 500);
    };

    "limit_a_silent_connection_is_closed_at_the_read_idle_limit"_test = [] {
        async::reactor loop;
        echo_handler handler;
        auto server = net::listener::bind(loop, std::string{loopback}, 0);

        auto const received = loop.run(silent_client(loop, *server, handler, {.read_idle = short_idle}));

        expect(received == 0_u);
    };

    "limit_header_values_with_line_breaks_are_refused"_test = [] {
        http::header_map fields;
        fields.add("X-Note", "a\r\nInjected: yes");

        auto const response = httpio::format_response_head(200, fields, 0);
        auto const request = httpio::format_request({.method = "GET", .target = "/", .host = "h", .fields = fields});

        expect(!response.has_value());
        expect(!request.has_value());
        expect(response.error() == http::errc::invalid_header);
    };

    "capability_chunk_frames_carry_a_hex_length"_test = [] {
        std::string const payload(26, 'x');

        auto const framed = httpio::frame_chunk(std::as_bytes(std::span{payload}));

        expect(framed.starts_with("1a\r\n"));
        expect(framed.ends_with("\r\n"));
    };
};

}  // namespace

int main() {}

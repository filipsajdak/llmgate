module;

#include "support/self_signed.hpp"
#include "test_io.h"

export module llmgate.scenarios;

import std;
import llmgate.async;
import llmgate.chaos;
import llmgate.http;
import llmgate.httpio;
import llmgate.net;
import llmgate.tls;

namespace llmgate::scenarios::detail {

constexpr std::string_view loopback = "127.0.0.1";
constexpr std::string_view localhost = "localhost";
constexpr int tiny_socket_buffer = 4096;
constexpr std::size_t writer_block_bytes = 131072;
constexpr std::size_t drain_buffer_bytes = 1000;
constexpr std::size_t http_client_count = 6;
constexpr std::size_t http_body_unit = 1500;
constexpr std::size_t duplex_total_bytes = 196608;
constexpr std::size_t duplex_piece_bytes = 3000;
constexpr std::size_t echo_buffer_bytes = 700;
constexpr int wait_tick_limit = 6000;
constexpr async::duration wait_tick{1};

using chaos_tcp = chaos::stream<net::tcp_stream>;

std::array<net::unique_fd, 2> tiny_socket_pair() {
    std::array<int, 2> fds{};
    test_io::open_pair(fds.data());
    for (auto const fd : fds) {
        test_io::make_nonblocking(fd);
        test_io::set_buffers(fd, tiny_socket_buffer);
    }
    return {net::adopt(fds[0]), net::adopt(fds[1])};
}

async::task<void> write_block(net::tcp_stream& stream, char const fill, int& failures, int& finished) {
    std::string const block(writer_block_bytes, fill);
    auto const sent = co_await stream.write_all(std::as_bytes(std::span{block}));
    failures += sent ? 0 : 1;
    ++finished;
}

async::task<void> wait_until(async::reactor& loop, int const& counter, int const target) {
    for (int tick = 0; tick < wait_tick_limit && counter < target; ++tick) {
        co_await loop.sleep(wait_tick);
    }
}

template <net::byte_stream Reader>
async::task<std::string> drain(Reader& reader, std::size_t const expected) {
    std::string received;
    std::array<std::byte, drain_buffer_bytes> buffer{};
    while (received.size() < expected) {
        auto const got = co_await reader.read_some(buffer, {});
        if (!got || *got == 0) {
            break;
        }
        received.append(reinterpret_cast<char const*>(buffer.data()), *got);
    }
    co_return received;
}

std::string describe_runs(std::string const& received) {
    auto const first = std::ranges::count(received, 'a');
    auto const second = std::ranges::count(received, 'b');
    auto const changes = std::ranges::adjacent_find(received, std::not_equal_to<>{}) == received.end()
                             ? 0
                             : std::ranges::count_if(std::views::adjacent<2>(received),
                                                     [](auto const pair) { return std::get<0>(pair) != std::get<1>(pair); });
    auto const intact = received.size() == 2 * writer_block_bytes && first == static_cast<std::ptrdiff_t>(writer_block_bytes) &&
                        second == static_cast<std::ptrdiff_t>(writer_block_bytes) && changes == 1;
    return intact ? std::string{} : std::format("writes interleaved or lost: size={} a={} b={} changes={}", received.size(), first, second, changes);
}

async::task<std::string> writers_scenario(async::reactor& loop, std::uint64_t const seed) {
    auto [writer_end, reader_end] = tiny_socket_pair();
    net::tcp_stream client{loop, std::move(writer_end)};
    chaos_tcp reader{net::tcp_stream{loop, std::move(reader_end)}, loop, seed};
    int failures = 0;
    int finished = 0;
    loop.spawn(write_block(client, 'a', failures, finished));
    loop.spawn(write_block(client, 'b', failures, finished));
    auto const received = co_await drain(reader, 2 * writer_block_bytes);
    co_await wait_until(loop, finished, 2);
    co_return failures != 0 ? std::string{"a writer failed"} : describe_runs(received);
}

struct echo_handler {
    async::task<net::result_void> operator()(httpio::request const& incoming, httpio::response_writer<chaos_tcp>& out) {
        co_return co_await out.send(200, {}, "echo:" + incoming.body);
    }
};

async::task<void> accept_connections(async::reactor& loop, net::listener& server, echo_handler& handler,
                                     std::uint64_t const seed) {
    for (std::size_t index = 0; index < http_client_count; ++index) {
        auto accepted = co_await server.accept();
        loop.spawn(httpio::serve_connection(chaos_tcp{std::move(*accepted), loop, seed + index}, handler, httpio::limits{}));
    }
}

async::task<void> http_round(async::reactor& loop, std::uint16_t const port, std::size_t const index, int& wrong, int& done) {
    std::string const body(http_body_unit * (index + 1), static_cast<char>('a' + index));
    std::string answer;
    auto client = co_await net::tcp_stream::connect(loop, std::string{loopback}, port);
    auto const finished = co_await httpio::exchange(
        *client, {.method = "POST", .target = "/echo", .host = "localhost", .body = body}, [&answer](http::event const& item) {
            if (auto const* chunk = std::get_if<http::body_chunk>(&item)) {
                answer.append(reinterpret_cast<char const*>(chunk->bytes.data()), chunk->bytes.size());
            }
        });
    wrong += (finished && answer == "echo:" + body) ? 0 : 1;
    ++done;
}

async::task<std::string> http_scenario(async::reactor& loop, std::uint64_t const seed) {
    auto server = net::listener::bind(loop, std::string{loopback}, 0);
    echo_handler handler;
    int wrong = 0;
    int done = 0;
    loop.spawn(accept_connections(loop, *server, handler, seed));
    for (std::size_t index = 0; index < http_client_count; ++index) {
        loop.spawn(http_round(loop, server->port(), index, wrong, done));
    }
    for (int tick = 0; tick < wait_tick_limit && done < static_cast<int>(http_client_count); ++tick) {
        co_await loop.sleep(wait_tick);
    }
    if (done < static_cast<int>(http_client_count)) {
        co_return std::format("stalled: {} of {} clients finished", done, http_client_count);
    }
    co_return wrong == 0 ? std::string{} : std::format("{} clients got a wrong echo", wrong);
}

testing::credentials const& server_credentials() {
    static testing::credentials const credentials = testing::make_self_signed(std::string{localhost});
    return credentials;
}

std::string pattern(std::size_t const size) {
    std::string data(size, '\0');
    for (std::size_t at = 0; at < size; ++at) {
        data[at] = static_cast<char>('a' + (at * 7 + at / 251) % 26);
    }
    return data;
}

async::task<void> tls_echo(async::reactor& loop, net::listener& server, tls::server_context const& context) {
    auto plain = co_await server.accept();
    auto secure = co_await net::tls_stream::accept(loop, context, std::move(*plain));
    if (!secure) {
        co_return;
    }
    std::array<std::byte, echo_buffer_bytes> buffer{};
    for (;;) {
        auto const got = co_await secure->read_some(buffer, {});
        if (!got || *got == 0) {
            break;
        }
        if (!co_await secure->write_all(std::span<std::byte const>{buffer}.first(*got))) {
            break;
        }
    }
    (void)co_await secure->close();
}

async::task<void> tls_pump(async::reactor& loop, net::tls_stream& stream, std::string const& data, int& failures,
                           int& finished) {
    std::span<std::byte const> remaining = std::as_bytes(std::span{data});
    while (!remaining.empty()) {
        auto const piece = remaining.first(std::min(duplex_piece_bytes, remaining.size()));
        failures += (co_await stream.write_all(piece)) ? 0 : 1;
        remaining = remaining.subspan(piece.size());
        co_await loop.yield();
    }
    (void)co_await stream.close();
    ++finished;
}

async::task<std::string> tls_scenario(async::reactor& loop, std::uint64_t) {
    auto server = net::listener::bind(loop, std::string{loopback}, 0);
    auto server_context = tls::server_context::make(tls::group_policy::post_quantum_preferred,
                                                    server_credentials().certificate_pem, server_credentials().private_key_pem);
    auto client_context = tls::client_context::make(tls::group_policy::post_quantum_preferred, server_credentials().certificate_pem);
    loop.spawn(tls_echo(loop, *server, *server_context));
    auto client = co_await net::tls_stream::connect(loop, *client_context, std::string{localhost}, server->port());
    if (!client) {
        co_return "tls connect failed: " + client.error().message();
    }
    auto const data = pattern(duplex_total_bytes);
    int failures = 0;
    int finished = 0;
    loop.spawn(tls_pump(loop, *client, data, failures, finished));
    auto const echoed = co_await drain(*client, data.size());
    co_await wait_until(loop, finished, 1);
    if (failures != 0) {
        co_return "a tls write failed";
    }
    co_return echoed == data ? std::string{} : std::format("echo differs: got {} of {} bytes", echoed.size(), data.size());
}

std::string run_scenario(async::task<std::string> (*scenario)(async::reactor&, std::uint64_t), std::uint64_t const seed) {
    async::reactor loop{seed};
    return loop.run(scenario(loop, seed));
}

}  // namespace llmgate::scenarios::detail

export namespace llmgate::scenarios {

enum class kind { concurrent_writers, http_clients, tls_duplex };

std::string run(kind const which, std::uint64_t const seed) {
    switch (which) {
        case kind::concurrent_writers:
            return detail::run_scenario(&detail::writers_scenario, seed);
        case kind::http_clients:
            return detail::run_scenario(&detail::http_scenario, seed);
        case kind::tls_duplex:
            break;
    }
    return detail::run_scenario(&detail::tls_scenario, seed);
}

}  // namespace llmgate::scenarios

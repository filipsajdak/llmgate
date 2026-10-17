#include <boost/ut.hpp>

#include <csignal>

import llmgate.async;
import llmgate.http;
import llmgate.httpio;
import llmgate.net;
import llmgate.shutdown;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate;

using writer = httpio::response_writer<net::tcp_stream>;
using steady = std::chrono::steady_clock;

constexpr std::string_view loopback = "127.0.0.1";
constexpr async::duration handler_delay{120};
constexpr async::duration stop_after{30};
constexpr async::duration short_drain{40};
constexpr async::duration long_drain = std::chrono::seconds{5};
constexpr async::duration quick_bound = std::chrono::seconds{2};
constexpr async::duration poll_step{5};
constexpr int poll_attempts = 2000;
constexpr std::uint64_t chaos_seeds = 32;

struct trace {
    int counter = 0;
    int handler_done = 0;
    int server_done = 0;
    std::optional<std::size_t> remaining;
    steady::duration server_took{};
};

struct slow_handler {
    async::reactor* loop;
    async::duration delay;
    trace* record;

    async::task<net::result_void> operator()(httpio::request const&, writer& out) {
        co_await loop->sleep(delay);
        auto const sent = co_await out.send(200, {}, "done");
        record->handler_done = ++record->counter;
        co_return sent;
    }
};

struct reply {
    int status = 0;
    bool complete = false;
    std::string body;
    std::error_code error;
};

auto collect_into(reply& out) {
    return [&out](http::event const& item) {
        if (auto const* head = std::get_if<http::message_head>(&item)) {
            out.status = head->status;
        } else if (auto const* chunk = std::get_if<http::body_chunk>(&item)) {
            out.body.append(reinterpret_cast<char const*>(chunk->bytes.data()), chunk->bytes.size());
        } else {
            out.complete = true;
        }
    };
}

httpio::request_spec ping() {
    return {.method = "POST", .target = "/slow", .host = "localhost", .body = "ping"};
}

async::task<void> run_server(net::listener& server, slow_handler& handler, shutdown::stop_signal& stop, async::duration drain, trace& record) {
    auto const started = steady::now();
    auto const remaining = co_await httpio::serve_until(server, handler, {}, stop.descriptor(), drain);
    record.server_took = steady::now() - started;
    record.server_done = ++record.counter;
    record.remaining = remaining;
}

async::task<void> raise_later(async::reactor& loop, shutdown::stop_signal& stop, async::duration delay) {
    co_await loop.sleep(delay);
    stop.request();
}

async::task<void> wait_for_server(async::reactor& loop, trace& record) {
    for (int attempt = 0; attempt < poll_attempts && !record.remaining; ++attempt) {
        co_await loop.sleep(poll_step);
    }
}

struct scenario_result {
    reply answer;
    trace record;
    bool refused_afterwards = false;
};

async::task<scenario_result> stop_during_request(async::reactor& loop, async::duration drain, async::duration delay) {
    scenario_result result;
    auto server = net::listener::bind(loop, std::string{loopback}, 0);
    auto stop = shutdown::stop_signal::make();
    slow_handler handler{.loop = &loop, .delay = delay, .record = &result.record};
    loop.spawn(run_server(*server, handler, *stop, drain, result.record));
    loop.spawn(raise_later(loop, *stop, stop_after));
    auto client = co_await net::tcp_stream::connect(loop, std::string{loopback}, server->port());
    auto const done = co_await httpio::exchange(*client, ping(), collect_into(result.answer));
    result.answer.error = done ? std::error_code{} : done.error();
    co_await wait_for_server(loop, result.record);
    auto const late = co_await net::tcp_stream::connect(loop, std::string{loopback}, server->port());
    result.refused_afterwards = !late.has_value();
    co_return result;
}

scenario_result run_stop_during_request(async::duration drain, async::duration delay, std::optional<std::uint64_t> seed = std::nullopt) {
    async::reactor loop{seed};
    return loop.run(stop_during_request(loop, drain, delay));
}

async::task<scenario_result> stop_while_idle(async::reactor& loop) {
    scenario_result result;
    auto server = net::listener::bind(loop, std::string{loopback}, 0);
    auto stop = shutdown::stop_signal::make();
    slow_handler handler{.loop = &loop, .delay = async::duration{0}, .record = &result.record};
    loop.spawn(run_server(*server, handler, *stop, long_drain, result.record));
    auto client = co_await net::tcp_stream::connect(loop, std::string{loopback}, server->port());
    (void)co_await httpio::exchange(*client, ping(), collect_into(result.answer));
    stop->request();
    co_await wait_for_server(loop, result.record);
    co_return result;
}

async::task<trace> stop_before_serving(async::reactor& loop) {
    trace record;
    auto server = net::listener::bind(loop, std::string{loopback}, 0);
    auto stop = shutdown::stop_signal::make();
    slow_handler handler{.loop = &loop, .delay = async::duration{0}, .record = &record};
    stop->request();
    co_await run_server(*server, handler, *stop, long_drain, record);
    co_return record;
}

async::task<trace> real_signal_stops_the_loop(async::reactor& loop) {
    trace record;
    auto server = net::listener::bind(loop, std::string{loopback}, 0);
    auto stop = shutdown::stop_signal::make();
    slow_handler handler{.loop = &loop, .delay = async::duration{0}, .record = &record};
    (void)stop->install();
    std::raise(SIGTERM);
    co_await run_server(*server, handler, *stop, long_drain, record);
    co_return record;
}

bool default_disposition(int const number) {
    struct sigaction current{};
    ::sigaction(number, nullptr, &current);
    return current.sa_handler == SIG_DFL;
}

const suite shutdown_suite = [] {
    "capability_a_request_in_flight_when_the_stop_arrives_still_completes"_test = [] {
        auto const result = run_stop_during_request(long_drain, handler_delay);

        expect(!result.answer.error);
        expect(result.answer.status == 200);
        expect(result.answer.body == "done");
        expect(result.record.remaining == std::optional<std::size_t>{0});
    };

    "capability_the_loop_returns_only_after_the_in_flight_handler_finished"_test = [] {
        auto const result = run_stop_during_request(long_drain, handler_delay);

        expect(result.record.handler_done != 0);
        expect(result.record.server_done > result.record.handler_done);
    };

    "capability_a_stopped_server_refuses_new_connections"_test = [] {
        auto const result = run_stop_during_request(long_drain, handler_delay);

        expect(result.refused_afterwards);
    };

    "limit_the_drain_timeout_ends_the_wait_and_reports_what_was_abandoned"_test = [] {
        auto const result = run_stop_during_request(short_drain, handler_delay * 4);

        expect(result.record.remaining == std::optional<std::size_t>{1});
        expect(result.record.server_done < result.record.handler_done);
    };

    "limit_an_idle_keep_alive_connection_does_not_hold_the_drain"_test = [] {
        async::reactor loop;

        auto const result = loop.run(stop_while_idle(loop));

        expect(result.record.remaining == std::optional<std::size_t>{0});
        expect(result.record.server_took < quick_bound);
    };

    "limit_a_stop_requested_before_serving_returns_at_once_without_accepting"_test = [] {
        async::reactor loop;

        auto const record = loop.run(stop_before_serving(loop));

        expect(record.remaining == std::optional<std::size_t>{0});
        expect(record.server_took < quick_bound);
    };

    "capability_a_real_sigterm_reaches_the_loop_through_the_self_pipe"_test = [] {
        async::reactor loop;

        auto const record = loop.run(real_signal_stops_the_loop(loop));

        expect(record.remaining == std::optional<std::size_t>{0});
        expect(default_disposition(SIGTERM) && default_disposition(SIGINT)) << "handlers are restored when the stop signal is destroyed";
    };

    "limit_a_stop_request_repeated_in_process_does_not_end_the_process"_test = [] {
        auto stop = shutdown::stop_signal::make();

        stop->request();
        stop->request();
        stop->acknowledge();

        expect(true);
    };

    "limit_the_stop_scenarios_hold_under_every_explored_schedule"_test = [] {
        for (std::uint64_t seed = 1; seed <= chaos_seeds; ++seed) {
            auto const finished = run_stop_during_request(long_drain, handler_delay, seed);
            expect(finished.answer.status == 200 && finished.answer.body == "done" && finished.record.remaining == std::optional<std::size_t>{0} &&
                   finished.record.server_done > finished.record.handler_done)
                << "replay with seed" << seed;
            auto const abandoned = run_stop_during_request(short_drain, handler_delay * 4, seed);
            expect(abandoned.record.remaining == std::optional<std::size_t>{1} && abandoned.record.server_done < abandoned.record.handler_done)
                << "replay with seed" << seed;
        }
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}

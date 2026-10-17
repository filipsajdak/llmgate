#include <sys/socket.h>
#include <unistd.h>

#include <boost/ut.hpp>

import llmgate.async;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate::async;

constexpr int unopened_descriptor = 99999;
constexpr int answer = 42;
constexpr duration short_wait{20};
constexpr duration long_wait{5000};

struct socket_pair {
    std::array<int, 2> fds{};
    socket_pair() { ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()); }
    ~socket_pair() {
        ::close(fds[0]);
        ::close(fds[1]);
    }
};

task<int> immediate(int const value) {
    co_return value;
}

task<int> chained(int const value) {
    auto const first = co_await immediate(value);
    auto const second = co_await immediate(first + 1);
    co_return second;
}

task<std::string> read_after_ready(reactor& loop, int const fd) {
    auto const ready = co_await loop.readable(fd);
    std::string received(4, '\0');
    if (!ready) {
        ::read(fd, received.data(), received.size());
    }
    co_return received;
}

task<std::error_code> wait_readable(reactor& loop, int const fd) {
    co_return co_await loop.readable(fd);
}

task<std::error_code> wait_writable(reactor& loop, int const fd) {
    co_return co_await loop.writable(fd);
}

task<std::error_code> wait_readable_for(reactor& loop, int const fd, duration const limit) {
    co_return co_await loop.readable(fd, limit);
}

task<std::error_code> wait_then_idle(reactor& loop, int const fd, duration const limit, duration const idle) {
    auto const first = co_await loop.readable(fd, limit);
    co_await loop.sleep(idle);
    co_return first;
}

task<reactor::race_result> race_two(reactor& loop, int const first, int const second, std::optional<duration> const limit) {
    co_return co_await loop.readable_or_other(first, second, limit);
}

task<void> write_then_flag(bool& flag) {
    flag = true;
    co_return;
}

const suite async_suite = [] {
    "capability_run_returns_the_value_of_a_task"_test = [] {
        reactor loop;

        expect(loop.run(immediate(answer)) == answer);
    };

    "capability_awaiting_tasks_chains_their_results"_test = [] {
        reactor loop;

        expect(loop.run(chained(answer)) == answer + 1);
    };

    "capability_run_drives_a_void_task_to_completion"_test = [] {
        reactor loop;
        bool ran = false;

        loop.run(write_then_flag(ran));

        expect(ran);
    };

    "capability_readable_resumes_after_the_peer_writes"_test = [] {
        reactor loop;
        socket_pair pair;
        ::write(pair.fds[1], "ping", 4);

        expect(loop.run(read_after_ready(loop, pair.fds[0])) == "ping");
    };

    "capability_writable_completes_on_an_empty_socket"_test = [] {
        reactor loop;
        socket_pair pair;

        expect(!loop.run(wait_writable(loop, pair.fds[0])));
    };

    "limit_readable_times_out_when_the_peer_stays_silent"_test = [] {
        reactor loop;
        socket_pair pair;

        auto const outcome = loop.run(wait_readable_for(loop, pair.fds[0], short_wait));

        expect(outcome == std::errc::timed_out);
    };

    "capability_readable_beats_its_timeout_and_the_timer_is_disarmed"_test = [] {
        reactor loop;
        socket_pair pair;
        ::write(pair.fds[1], "x", 1);

        auto const outcome = loop.run(wait_then_idle(loop, pair.fds[0], long_wait, 2 * short_wait));

        expect(!outcome);
    };

    "capability_sleep_waits_at_least_its_duration"_test = [] {
        reactor loop;
        auto const started = std::chrono::steady_clock::now();

        loop.run(loop.sleep(short_wait));

        expect(std::chrono::steady_clock::now() - started >= short_wait);
    };

    "limit_a_timed_out_wait_leaves_the_descriptor_usable"_test = [] {
        reactor loop;
        socket_pair pair;
        (void)loop.run(wait_readable_for(loop, pair.fds[0], short_wait));
        ::write(pair.fds[1], "x", 1);

        expect(!loop.run(wait_readable_for(loop, pair.fds[0], long_wait)));
    };

    "capability_race_reports_the_first_descriptor_when_it_is_ready"_test = [] {
        reactor loop;
        socket_pair first;
        socket_pair second;
        ::write(first.fds[1], "x", 1);

        auto const outcome = loop.run(race_two(loop, first.fds[0], second.fds[0], long_wait));

        expect(!outcome.error);
        expect(!outcome.other_ready);
    };

    "capability_race_reports_the_other_descriptor_when_it_is_ready"_test = [] {
        reactor loop;
        socket_pair first;
        socket_pair second;
        ::write(second.fds[1], "x", 1);

        auto const outcome = loop.run(race_two(loop, first.fds[0], second.fds[0], long_wait));

        expect(!outcome.error);
        expect(outcome.other_ready);
    };

    "limit_race_times_out_when_neither_descriptor_is_ready"_test = [] {
        reactor loop;
        socket_pair first;
        socket_pair second;

        auto const outcome = loop.run(race_two(loop, first.fds[0], second.fds[0], short_wait));

        expect(outcome.error == std::errc::timed_out);
        expect(!outcome.other_ready);
    };

    "limit_a_lost_race_leaves_both_descriptors_usable"_test = [] {
        reactor loop;
        socket_pair first;
        socket_pair second;
        ::write(first.fds[1], "x", 1);
        (void)loop.run(race_two(loop, first.fds[0], second.fds[0], long_wait));
        ::write(second.fds[1], "y", 1);

        auto const outcome = loop.run(race_two(loop, first.fds[0], second.fds[0], long_wait));

        expect(!outcome.error);
    };

    "limit_a_failed_arm_leaves_no_stale_registration_behind"_test = [] {
        reactor loop;
        socket_pair first;

        auto const failed = loop.run(race_two(loop, first.fds[0], unopened_descriptor, long_wait));
        ::write(first.fds[1], "x", 1);
        auto const later = loop.run(wait_readable_for(loop, first.fds[0], long_wait));

        expect(static_cast<bool>(failed.error));
        expect(!later);
    };

    "limit_readable_on_an_unopened_descriptor_reports_the_error_as_a_value"_test = [] {
        reactor loop;

        expect(static_cast<bool>(loop.run(wait_readable(loop, unopened_descriptor))));
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}

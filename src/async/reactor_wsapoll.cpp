module;

#include "platform.h"
#include "sockets.h"

module llmgate.async;

import std;

namespace llmgate::async {

namespace {

constexpr short readiness_events = POLLRDNORM | POLLERR | POLLHUP | POLLNVAL;
constexpr short writability_events = POLLWRNORM | POLLERR | POLLHUP | POLLNVAL;
constexpr auto idle_pause = std::chrono::milliseconds{10};

struct firing {
    detail::pending_op* op;
    detail::source_kind kind;
};

void add_interest(std::map<int, short>& wanted, int const fd, interest const what) {
    if (fd >= 0) {
        wanted[fd] |= what == interest::readable ? POLLRDNORM : POLLWRNORM;
    }
}

std::map<int, short> wanted_events(std::vector<detail::pending_op*> const& ops) {
    std::map<int, short> wanted;
    for (auto const* const op : ops) {
        add_interest(wanted, op->fd, op->what);
        add_interest(wanted, op->other_fd, interest::readable);
    }
    return wanted;
}

std::optional<std::chrono::steady_clock::time_point> soonest_deadline(std::vector<detail::pending_op*> const& ops) {
    std::optional<std::chrono::steady_clock::time_point> soonest;
    for (auto const* const op : ops) {
        if (op->has_timer && (!soonest || op->deadline < *soonest)) {
            soonest = op->deadline;
        }
    }
    return soonest;
}

int poll_timeout_ms(std::vector<detail::pending_op*> const& ops, bool const has_posted) {
    auto const soonest = soonest_deadline(ops);
    if (has_posted || !soonest) {
        return has_posted ? 0 : -1;
    }
    auto const wait = std::chrono::ceil<std::chrono::milliseconds>(*soonest - std::chrono::steady_clock::now());
    return static_cast<int>(std::clamp<std::chrono::milliseconds::rep>(wait.count(), 0, std::numeric_limits<int>::max()));
}

detail::pending_op* waiter_if(detail::waiter_table& waiters, int const fd, short const happened, short const wanted, interest const what) noexcept {
    return (happened & wanted) != 0 ? waiters.find(fd, what) : nullptr;
}

std::array<firing, 2> firings_for(detail::waiter_table& waiters, int const fd, short const happened) noexcept {
    std::array<firing, 2> result{};
    auto* const reader = waiter_if(waiters, fd, happened, readiness_events, interest::readable);
    auto* const writer = waiter_if(waiters, fd, happened, writability_events, interest::writable);
    auto slot = result.begin();
    for (auto* const waiter : {reader, writer}) {
        auto const second = waiter != nullptr && waiter->other_fd == fd && waiter->fd != fd;
        *slot++ = firing{.op = waiter, .kind = second ? detail::source_kind::other_descriptor : detail::source_kind::descriptor};
    }
    return result;
}

std::vector<WSAPOLLFD> watches_of(std::map<int, short> const& wanted) {
    std::vector<WSAPOLLFD> watches;
    for (auto const& [fd, events] : wanted) {
        watches.push_back(WSAPOLLFD{.fd = static_cast<SOCKET>(fd), .events = events, .revents = 0});
    }
    return watches;
}

void wait_for_events(std::vector<WSAPOLLFD>& watches, int const timeout) noexcept {
    if (watches.empty()) {
        if (timeout != 0) {
            std::this_thread::sleep_for(timeout < 0 ? idle_pause : std::min(std::chrono::milliseconds{timeout}, idle_pause));
        }
        return;
    }
    if (WSAPoll(watches.data(), static_cast<ULONG>(watches.size()), timeout) == SOCKET_ERROR && WSAGetLastError() != WSAEINTR) {
        std::terminate();
    }
}

struct collector {
    detail::waiter_table& waiters;
    std::vector<detail::pending_op*> ready;

    void fire(detail::pending_op* const op, detail::source_kind const kind) {
        if (op == nullptr || op->fired) {
            return;
        }
        op->fired = true;
        op->winner = kind;
        op->outcome = kind == detail::source_kind::timer ? std::make_error_code(std::errc::timed_out) : std::error_code{};
        op->has_timer = false;
        waiters.release_all(op);
        ready.push_back(op);
    }

    void descriptor(int const fd, short const happened) {
        if (happened == 0) {
            return;
        }
        for (auto const& [op, kind] : firings_for(waiters, fd, happened)) {
            fire(op, kind);
        }
    }

    void expired(std::vector<detail::pending_op*> const& ops, std::chrono::steady_clock::time_point const now) {
        for (auto* const op : ops) {
            if (op->has_timer && !op->fired && op->deadline <= now) {
                fire(op, detail::source_kind::timer);
            }
        }
    }
};

}  // namespace

reactor::reactor(std::optional<std::uint64_t> const chaos_seed) : poller_fd_{-1} {
    if (chaos_seed) {
        chaos_.emplace(*chaos_seed);
    }
}

reactor::~reactor() = default;

std::error_code reactor::sync_descriptor(int) noexcept {
    return {};
}

void reactor::release_claims(detail::pending_op* const op) noexcept {
    waiters_.release_all(op);
}

std::error_code reactor::arm_backend(int const, interest const, std::optional<duration> const timeout, detail::pending_op* const op) noexcept {
    op->fired = false;
    op->has_timer = timeout.has_value();
    op->outcome = {};
    op->timer_fd = -1;
    for (auto const descriptor : {op->fd, op->other_fd}) {
        if (descriptor >= 0) {
            if (auto const failure = platform::check_descriptor(descriptor)) {
                release_claims(op);
                return {failure, std::system_category()};
            }
        }
    }
    if (timeout) {
        op->deadline = std::chrono::steady_clock::now() + *timeout;
    }
    return {};
}

void reactor::poll_or_terminate() noexcept {
    std::vector<detail::pending_op*> const ops = waiters_.armed();
    auto watches = watches_of(wanted_events(ops));
    wait_for_events(watches, poll_timeout_ms(ops, has_posted()));
    collector fired{.waiters = waiters_, .ready = {}};
    for (auto const& watch : watches) {
        fired.descriptor(static_cast<int>(watch.fd), watch.revents);
    }
    fired.expired(ops, std::chrono::steady_clock::now());
    deliver(std::span{fired.ready.data(), fired.ready.size()});
    run_posted();
}

}  // namespace llmgate::async

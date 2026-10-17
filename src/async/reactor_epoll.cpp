module;

#include <cerrno>

#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

module llmgate.async;

import std;

namespace llmgate::async {

namespace {

constexpr std::size_t events_per_poll = 64;
constexpr long milliseconds_per_second = 1000;
constexpr long nanoseconds_per_millisecond = 1000000;

std::error_code last_error() noexcept {
    return {errno, std::system_category()};
}

epoll_event request_for(std::uint32_t const events, detail::event_source* const source) noexcept {
    epoll_event request{};
    request.events = events | EPOLLONESHOT;
    request.data.ptr = source;
    return request;
}

std::error_code watch(int const poller, int const fd, std::uint32_t const events,
                      detail::event_source* const source) noexcept {
    auto request = request_for(events, source);
    if (::epoll_ctl(poller, EPOLL_CTL_ADD, fd, &request) == 0) {
        return {};
    }
    if (errno == EEXIST && ::epoll_ctl(poller, EPOLL_CTL_MOD, fd, &request) == 0) {
        return {};
    }
    return last_error();
}

int make_timer(duration const length) noexcept {
    auto const timer = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer < 0) {
        return -1;
    }
    itimerspec expiry{};
    expiry.it_value.tv_sec = length.count() / milliseconds_per_second;
    expiry.it_value.tv_nsec = (length.count() % milliseconds_per_second) * nanoseconds_per_millisecond;
    if (expiry.it_value.tv_sec == 0 && expiry.it_value.tv_nsec == 0) {
        expiry.it_value.tv_nsec = 1;
    }
    if (::timerfd_settime(timer, 0, &expiry, nullptr) < 0) {
        ::close(timer);
        return -1;
    }
    return timer;
}

void disarm_others(int const poller, detail::pending_op* const op, detail::source_kind const fired) noexcept {
    if (fired != detail::source_kind::descriptor && op->fd >= 0) {
        ::epoll_ctl(poller, EPOLL_CTL_DEL, op->fd, nullptr);
    }
    if (fired != detail::source_kind::other_descriptor && op->other_fd >= 0) {
        ::epoll_ctl(poller, EPOLL_CTL_DEL, op->other_fd, nullptr);
    }
    if (op->has_timer) {
        ::close(op->timer_fd);
        op->has_timer = false;
    }
}

}  // namespace

reactor::reactor() : poller_fd_{::epoll_create1(EPOLL_CLOEXEC)} {
    contract_assert(poller_fd_ >= 0);
}

reactor::~reactor() {
    ::close(poller_fd_);
}

std::error_code reactor::arm(int const fd, interest const what, std::optional<duration> const timeout,
                             detail::pending_op* const op) noexcept {
    op->fired = false;
    op->has_timer = false;
    op->fd = fd;
    op->what = what;
    op->outcome = {};
    op->timer_fd = -1;
    std::error_code failure;
    if (fd >= 0) {
        auto const events = what == interest::readable ? EPOLLIN : EPOLLOUT;
        failure = watch(poller_fd_, fd, events, &op->fd_source);
    }
    if (!failure && op->other_fd >= 0) {
        failure = watch(poller_fd_, op->other_fd, EPOLLIN, &op->other_source);
    }
    if (!failure && timeout) {
        auto const timer = make_timer(*timeout);
        failure = timer < 0 ? last_error() : watch(poller_fd_, timer, EPOLLIN, &op->timer_source);
        if (timer >= 0) {
            op->timer_fd = timer;
            op->has_timer = true;
        }
    }
    if (failure) {
        disarm_others(poller_fd_, op, detail::source_kind::timer);
    }
    return failure;
}

void reactor::poll_or_terminate() noexcept {
    std::array<epoll_event, events_per_poll> events;
    auto const count = ::epoll_wait(poller_fd_, events.data(), static_cast<int>(events.size()), -1);
    if (count < 0) {
        if (errno != EINTR) {
            std::terminate();
        }
        return;
    }
    std::array<detail::pending_op*, events_per_poll> ready;
    std::size_t ready_count = 0;
    for (auto const& event : std::span{events.data(), static_cast<std::size_t>(count)}) {
        auto* const source = static_cast<detail::event_source*>(event.data.ptr);
        auto* const op = source->owner;
        if (op->fired) {
            continue;
        }
        op->fired = true;
        op->winner = source->kind;
        op->outcome = source->kind == detail::source_kind::timer ? std::make_error_code(std::errc::timed_out) : std::error_code{};
        disarm_others(poller_fd_, op, source->kind);
        ready[ready_count++] = op;
    }
    for (auto* const op : std::span{ready.data(), ready_count}) {
        op->complete(op, op->outcome);
    }
}

}  // namespace llmgate::async

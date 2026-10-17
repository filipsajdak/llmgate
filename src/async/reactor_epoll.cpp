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
constexpr std::uint64_t timer_tag = 1;
constexpr std::uint32_t readiness_events = EPOLLIN | EPOLLERR | EPOLLHUP;
constexpr std::uint32_t writability_events = EPOLLOUT | EPOLLERR | EPOLLHUP;

std::error_code last_error() noexcept {
    return {errno, std::system_category()};
}

std::uint64_t descriptor_tag(int const fd) noexcept {
    return static_cast<std::uint64_t>(fd) << 1;
}

std::uint64_t timer_source_tag(detail::event_source const* const source) noexcept {
    return reinterpret_cast<std::uintptr_t>(source) | timer_tag;
}

std::uint32_t mask_for(detail::waiter_table const& table, int const fd) noexcept {
    return (table.find(fd, interest::readable) != nullptr ? EPOLLIN : 0U) |
           (table.find(fd, interest::writable) != nullptr ? EPOLLOUT : 0U);
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

void close_timer(detail::pending_op* const op) noexcept {
    if (op->has_timer) {
        ::close(op->timer_fd);
        op->has_timer = false;
    }
}

}  // namespace

reactor::reactor(std::optional<std::uint64_t> const chaos_seed) : poller_fd_{::epoll_create1(EPOLL_CLOEXEC)} {
    contract_assert(poller_fd_ >= 0);
    if (chaos_seed) {
        chaos_.emplace(*chaos_seed);
    }
}

reactor::~reactor() {
    ::close(poller_fd_);
}

std::error_code reactor::sync_descriptor(int const fd) noexcept {
    if (fd < 0) {
        return {};
    }
    auto const mask = mask_for(waiters_, fd);
    if (mask == 0) {
        ::epoll_ctl(poller_fd_, EPOLL_CTL_DEL, fd, nullptr);
        return {};
    }
    epoll_event request{};
    request.events = mask;
    request.data.u64 = descriptor_tag(fd);
    if (::epoll_ctl(poller_fd_, EPOLL_CTL_MOD, fd, &request) == 0) {
        return {};
    }
    if (errno == ENOENT && ::epoll_ctl(poller_fd_, EPOLL_CTL_ADD, fd, &request) == 0) {
        return {};
    }
    return last_error();
}

void reactor::release_claims(detail::pending_op* const op) noexcept {
    waiters_.release_all(op);
    sync_descriptor(op->fd);
    sync_descriptor(op->other_fd);
}

std::error_code reactor::arm_backend(int const fd, interest const, std::optional<duration> const timeout,
                                     detail::pending_op* const op) noexcept {
    op->fired = false;
    op->has_timer = false;
    op->outcome = {};
    op->timer_fd = -1;
    auto failure = sync_descriptor(fd);
    if (!failure) {
        failure = sync_descriptor(op->other_fd);
    }
    if (!failure && timeout) {
        auto const timer = make_timer(*timeout);
        if (timer < 0) {
            failure = last_error();
        } else {
            op->timer_fd = timer;
            op->has_timer = true;
            epoll_event request{};
            request.events = EPOLLIN;
            request.data.u64 = timer_source_tag(&op->timer_source);
            failure = ::epoll_ctl(poller_fd_, EPOLL_CTL_ADD, timer, &request) == 0 ? std::error_code{} : last_error();
        }
    }
    if (failure) {
        close_timer(op);
        release_claims(op);
    }
    return failure;
}

void reactor::poll_or_terminate() noexcept {
    std::array<epoll_event, events_per_poll> events;
    auto const count = ::epoll_wait(poller_fd_, events.data(), static_cast<int>(events.size()), has_posted() ? 0 : -1);
    if (count < 0) {
        if (errno != EINTR) {
            std::terminate();
        }
        return;
    }
    std::array<detail::pending_op*, 2 * events_per_poll> ready;
    std::size_t ready_count = 0;
    auto const fire = [this, &ready, &ready_count](detail::pending_op* const op, detail::source_kind const kind) {
        if (op == nullptr || op->fired) {
            return;
        }
        op->fired = true;
        op->winner = kind;
        op->outcome = kind == detail::source_kind::timer ? std::make_error_code(std::errc::timed_out) : std::error_code{};
        close_timer(op);
        release_claims(op);
        ready[ready_count++] = op;
    };
    for (auto const& event : std::span{events.data(), static_cast<std::size_t>(count)}) {
        auto const tag = event.data.u64;
        if ((tag & timer_tag) != 0) {
            auto const* const source = reinterpret_cast<detail::event_source const*>(tag & ~timer_tag);
            fire(source->owner, detail::source_kind::timer);
            continue;
        }
        auto const fd = static_cast<int>(tag >> 1);
        auto* const reader = (event.events & readiness_events) != 0 ? waiters_.find(fd, interest::readable) : nullptr;
        auto* const writer = (event.events & writability_events) != 0 ? waiters_.find(fd, interest::writable) : nullptr;
        for (auto* const waiter : {reader, writer}) {
            auto const second = waiter != nullptr && waiter->other_fd == fd && waiter->fd != fd;
            fire(waiter, second ? detail::source_kind::other_descriptor : detail::source_kind::descriptor);
        }
    }
    deliver(std::span{ready.data(), ready_count});
    run_posted();
}

}  // namespace llmgate::async

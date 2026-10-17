module;

#include <cerrno>

#include <sys/event.h>
#include <unistd.h>

module llmgate.async;

import std;

namespace llmgate::async {

namespace {

constexpr std::size_t events_per_poll = 64;
constexpr std::size_t changes_per_arm = 3;

std::error_code last_error() noexcept {
    return {errno, std::system_category()};
}

std::uintptr_t timer_ident(detail::pending_op const* op) noexcept {
    return reinterpret_cast<std::uintptr_t>(op);
}

void remove_event(int const poller, std::uintptr_t const ident, int const filter) noexcept {
    struct kevent change;
    EV_SET(&change, ident, filter, EV_DELETE, 0, 0, nullptr);
    ::kevent(poller, &change, 1, nullptr, 0, nullptr);
}

int filter_of(interest const what) noexcept {
    return what == interest::readable ? EVFILT_READ : EVFILT_WRITE;
}

std::error_code outcome_of(struct kevent const& event, detail::source_kind const kind) noexcept {
    if (kind == detail::source_kind::timer) {
        return std::make_error_code(std::errc::timed_out);
    }
    if ((event.flags & EV_ERROR) != 0) {
        return {static_cast<int>(event.data), std::system_category()};
    }
    return {};
}

void disarm_others(int const poller, detail::pending_op const* const op, detail::source_kind const fired) noexcept {
    if (fired != detail::source_kind::descriptor && op->fd >= 0) {
        remove_event(poller, static_cast<std::uintptr_t>(op->fd), filter_of(op->what));
    }
    if (fired != detail::source_kind::other_descriptor && op->other_fd >= 0) {
        remove_event(poller, static_cast<std::uintptr_t>(op->other_fd), EVFILT_READ);
    }
    if (fired != detail::source_kind::timer && op->has_timer) {
        remove_event(poller, timer_ident(op), EVFILT_TIMER);
    }
}

}  // namespace

reactor::reactor(std::optional<std::uint64_t> const chaos_seed) : poller_fd_{::kqueue()} {
    contract_assert(poller_fd_ >= 0);
    if (chaos_seed) {
        chaos_.emplace(*chaos_seed);
    }
}

reactor::~reactor() {
    ::close(poller_fd_);
}

void reactor::release_claims(detail::pending_op* const op) noexcept {
    waiters_.release_all(op);
}

std::error_code reactor::arm_backend(int const fd, interest const what, std::optional<duration> const timeout,
                                     detail::pending_op* const op) noexcept {
    std::array<struct kevent, changes_per_arm> changes;
    int count = 0;
    op->fired = false;
    op->has_timer = timeout.has_value();
    op->outcome = {};
    if (fd >= 0) {
        EV_SET(&changes[count++], static_cast<std::uintptr_t>(fd), filter_of(what), EV_ADD | EV_ONESHOT, 0, 0,
               &op->fd_source);
    }
    if (op->other_fd >= 0) {
        EV_SET(&changes[count++], static_cast<std::uintptr_t>(op->other_fd), EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0,
               &op->other_source);
    }
    if (timeout) {
        EV_SET(&changes[count++], timer_ident(op), EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, timeout->count(),
               &op->timer_source);
    }
    if (::kevent(poller_fd_, changes.data(), count, nullptr, 0, nullptr) < 0) {
        auto const error = last_error();
        disarm_others(poller_fd_, op, detail::source_kind::nothing);
        release_claims(op);
        return error;
    }
    return {};
}

void reactor::poll_or_terminate() noexcept {
    std::array<struct kevent, events_per_poll> events;
    timespec const immediately{};
    auto const count = ::kevent(poller_fd_, nullptr, 0, events.data(), static_cast<int>(events.size()),
                                has_posted() ? &immediately : nullptr);
    if (count < 0) {
        if (errno != EINTR) {
            std::terminate();
        }
        return;
    }
    std::array<detail::pending_op*, events_per_poll> ready;
    std::size_t ready_count = 0;
    for (auto const& event : std::span{events.data(), static_cast<std::size_t>(count)}) {
        auto* const source = static_cast<detail::event_source*>(event.udata);
        auto* const op = source->owner;
        if (op->fired) {
            continue;
        }
        op->fired = true;
        op->winner = source->kind;
        op->outcome = outcome_of(event, source->kind);
        disarm_others(poller_fd_, op, source->kind);
        release_claims(op);
        ready[ready_count++] = op;
    }
    deliver(std::span{ready.data(), ready_count});
    run_posted();
}

}  // namespace llmgate::async

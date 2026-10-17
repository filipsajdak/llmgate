module;

#include <cerrno>

#include <sys/event.h>
#include <unistd.h>

module llmgate.async;

import std;

namespace llmgate::async {

namespace {

constexpr std::size_t events_per_poll = 64;
constexpr std::size_t changes_per_arm = 2;

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

}  // namespace

reactor::reactor() : poller_fd_{::kqueue()} {
    contract_assert(poller_fd_ >= 0);
}

reactor::~reactor() {
    ::close(poller_fd_);
}

std::error_code reactor::arm(int const fd, interest const what, std::optional<duration> const timeout,
                             detail::pending_op* const op) noexcept {
    std::array<struct kevent, changes_per_arm> changes;
    int count = 0;
    op->fired = false;
    op->has_timer = timeout.has_value();
    op->fd = fd;
    op->what = what;
    op->outcome = {};
    if (fd >= 0) {
        EV_SET(&changes[count++], static_cast<std::uintptr_t>(fd), filter_of(what), EV_ADD | EV_ONESHOT, 0, 0,
               &op->fd_source);
    }
    if (timeout) {
        EV_SET(&changes[count++], timer_ident(op), EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, timeout->count(),
               &op->timer_source);
    }
    if (::kevent(poller_fd_, changes.data(), count, nullptr, 0, nullptr) < 0) {
        auto const error = last_error();
        if (timeout) {
            remove_event(poller_fd_, timer_ident(op), EVFILT_TIMER);
        }
        return error;
    }
    return {};
}

void reactor::poll_or_terminate() noexcept {
    std::array<struct kevent, events_per_poll> events;
    auto const count = ::kevent(poller_fd_, nullptr, 0, events.data(), static_cast<int>(events.size()), nullptr);
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
        if (source->timer) {
            op->outcome = std::make_error_code(std::errc::timed_out);
            if (op->fd >= 0) {
                remove_event(poller_fd_, static_cast<std::uintptr_t>(op->fd), filter_of(op->what));
            }
        } else {
            auto const failed = (event.flags & EV_ERROR) != 0;
            op->outcome = failed ? std::error_code{static_cast<int>(event.data), std::system_category()} : std::error_code{};
            if (op->has_timer) {
                remove_event(poller_fd_, timer_ident(op), EVFILT_TIMER);
            }
        }
        ready[ready_count++] = op;
    }
    for (auto* const op : std::span{ready.data(), ready_count}) {
        op->complete(op, op->outcome);
    }
}

}  // namespace llmgate::async

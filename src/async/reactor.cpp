module;

#include <cerrno>

#include <sys/event.h>
#include <unistd.h>

module llmgate.async;

import std;

namespace llmgate::async {

namespace {

constexpr std::size_t events_per_poll = 64;

std::error_code last_error() noexcept {
    return {errno, std::system_category()};
}

}  // namespace

reactor::reactor() : kqueue_fd_{::kqueue()} {
    contract_assert(kqueue_fd_ >= 0);
}

reactor::~reactor() {
    ::close(kqueue_fd_);
}

std::error_code reactor::arm(int const fd, interest const what, detail::pending_op* const op) noexcept {
    struct kevent change;
    auto const filter = what == interest::readable ? EVFILT_READ : EVFILT_WRITE;
    EV_SET(&change, static_cast<uintptr_t>(fd), filter, EV_ADD | EV_ONESHOT, 0, 0, op);
    if (::kevent(kqueue_fd_, &change, 1, nullptr, 0, nullptr) < 0) {
        return last_error();
    }
    return {};
}

void reactor::poll_or_terminate() noexcept {
    std::array<struct kevent, events_per_poll> events;
    auto const count = ::kevent(kqueue_fd_, nullptr, 0, events.data(), static_cast<int>(events.size()), nullptr);
    if (count < 0) {
        if (errno != EINTR) {
            std::terminate();
        }
        return;
    }
    for (auto const& event : std::span{events.data(), static_cast<std::size_t>(count)}) {
        auto* const op = static_cast<detail::pending_op*>(event.udata);
        auto const failed = (event.flags & EV_ERROR) != 0;
        op->complete(op, failed ? std::error_code{static_cast<int>(event.data), std::system_category()} : std::error_code{});
    }
}

}  // namespace llmgate::async

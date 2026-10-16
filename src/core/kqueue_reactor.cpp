#include "core/kqueue_reactor.hpp"

#include <array>
#include <cerrno>
#include <span>

#include <sys/event.h>
#include <unistd.h>

namespace llmgate::core {

namespace {

constexpr std::size_t events_per_poll = 64;

std::error_code last_error() noexcept {
    return {errno, std::system_category()};
}

}  // namespace

kqueue_reactor::kqueue_reactor() : kqueue_fd_{::kqueue()} {
    contract_assert(kqueue_fd_ >= 0);
}

kqueue_reactor::~kqueue_reactor() {
    ::close(kqueue_fd_);
}

std::error_code kqueue_reactor::arm_readable(int const fd, pending_op* const op) noexcept {
    struct kevent change;
    EV_SET(&change, static_cast<uintptr_t>(fd), EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, op);
    if (::kevent(kqueue_fd_, &change, 1, nullptr, 0, nullptr) < 0) {
        return last_error();
    }
    return {};
}

std::error_code kqueue_reactor::poll_once() noexcept {
    std::array<struct kevent, events_per_poll> events;
    auto const count = ::kevent(kqueue_fd_, nullptr, 0, events.data(), static_cast<int>(events.size()), nullptr);
    if (count < 0) {
        return errno == EINTR ? std::error_code{} : last_error();
    }
    for (auto const& event : std::span{events.data(), static_cast<std::size_t>(count)}) {
        auto* const op = static_cast<pending_op*>(event.udata);
        auto const failed = (event.flags & EV_ERROR) != 0;
        op->complete(op, failed ? std::error_code{static_cast<int>(event.data), std::system_category()} : std::error_code{});
    }
    return {};
}

}  // namespace llmgate::core

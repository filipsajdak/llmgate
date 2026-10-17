module;

#include <cerrno>

#include <sys/epoll.h>
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

reactor::reactor() : poller_fd_{::epoll_create1(EPOLL_CLOEXEC)} {
    contract_assert(poller_fd_ >= 0);
}

reactor::~reactor() {
    ::close(poller_fd_);
}

std::error_code reactor::arm(int const fd, interest const what, detail::pending_op* const op) noexcept {
    epoll_event request{};
    request.events = (what == interest::readable ? EPOLLIN : EPOLLOUT) | EPOLLONESHOT;
    request.data.ptr = op;
    if (::epoll_ctl(poller_fd_, EPOLL_CTL_ADD, fd, &request) == 0) {
        return {};
    }
    if (errno == EEXIST && ::epoll_ctl(poller_fd_, EPOLL_CTL_MOD, fd, &request) == 0) {
        return {};
    }
    return last_error();
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
    for (auto const& event : std::span{events.data(), static_cast<std::size_t>(count)}) {
        auto* const op = static_cast<detail::pending_op*>(event.data.ptr);
        op->complete(op, {});
    }
}

}  // namespace llmgate::async

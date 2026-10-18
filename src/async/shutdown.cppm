module;

#include <cerrno>
#include <csignal>

#include <fcntl.h>
#include <unistd.h>

export module llmgate.shutdown;

import std;
import llmgate.async;

export namespace llmgate::shutdown {

constexpr async::duration default_drain = std::chrono::seconds{10};
constexpr int second_signal_status = 130;
constexpr std::array<int, 2> handled_signals{SIGINT, SIGTERM};

}  // namespace llmgate::shutdown

namespace llmgate::shutdown::detail {

inline std::atomic<int> notify_descriptor{-1};
inline std::atomic<int> signals_seen{0};

static_assert(std::atomic<int>::is_always_lock_free);

void on_signal(int const) noexcept {
    auto const saved = errno;
    if (signals_seen.fetch_add(1) >= 1) {
        ::_exit(second_signal_status);
    }
    if (auto const descriptor = notify_descriptor.load(); descriptor >= 0) {
        char const byte = 1;
        (void)::write(descriptor, &byte, 1);
    }
    errno = saved;
}

std::error_code last_error() noexcept {
    return {errno, std::system_category()};
}

std::error_code prepare(int const descriptor) noexcept {
    if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0 || ::fcntl(descriptor, F_SETFL, ::fcntl(descriptor, F_GETFL, 0) | O_NONBLOCK) < 0) {
        return last_error();
    }
    return {};
}

}  // namespace llmgate::shutdown::detail

export namespace llmgate::shutdown {

class stop_signal {
public:
    static std::expected<stop_signal, std::error_code> make() {
        std::array<int, 2> ends{};
        if (::pipe(ends.data()) < 0) {
            return std::unexpected{detail::last_error()};
        }
        stop_signal made{ends[0], ends[1]};
        for (auto const descriptor : ends) {
            if (auto const failure = detail::prepare(descriptor)) {
                return std::unexpected{failure};
            }
        }
        return made;
    }

    stop_signal(stop_signal&& other) noexcept
        : read_{std::exchange(other.read_, -1)}, write_{std::exchange(other.write_, -1)}, installed_{std::exchange(other.installed_, false)} {}
    stop_signal(stop_signal const&) = delete;
    stop_signal& operator=(stop_signal const&) = delete;
    stop_signal& operator=(stop_signal&&) = delete;

    ~stop_signal() {
        uninstall();
        for (auto const descriptor : {read_, write_}) {
            if (descriptor >= 0) {
                ::close(descriptor);
            }
        }
    }

    int descriptor() const noexcept { return read_; }

    void request() noexcept {
        char const byte = 1;
        (void)::write(write_, &byte, 1);
    }

    void acknowledge() noexcept {
        std::array<char, 16> sink;
        while (::read(read_, sink.data(), sink.size()) > 0) {
        }
    }

    std::error_code install() noexcept {
        detail::signals_seen.store(0);
        detail::notify_descriptor.store(write_);
        struct sigaction action{};
        action.sa_handler = &detail::on_signal;
        action.sa_flags = SA_RESTART;
        for (auto const number : handled_signals) {
            if (::sigaction(number, &action, nullptr) < 0) {
                auto const failure = detail::last_error();
                uninstall();
                return failure;
            }
        }
        installed_ = true;
        return {};
    }

private:
    stop_signal(int const read_end, int const write_end) noexcept : read_{read_end}, write_{write_end} {}

    void uninstall() noexcept {
        if (!std::exchange(installed_, false)) {
            return;
        }
        detail::notify_descriptor.store(-1);
        struct sigaction restore{};
        restore.sa_handler = SIG_DFL;
        for (auto const number : handled_signals) {
            ::sigaction(number, &restore, nullptr);
        }
    }

    int read_;
    int write_;
    bool installed_ = false;
};

class drain_gate : public std::enable_shared_from_this<drain_gate> {
public:
    explicit drain_gate(async::reactor& loop) noexcept : loop_{&loop} {}
    drain_gate(drain_gate const&) = delete;
    drain_gate& operator=(drain_gate const&) = delete;

    class scope {
    public:
        explicit scope(drain_gate& gate) noexcept : gate_{&gate} { ++gate.active_; }
        scope(scope const&) = delete;
        scope& operator=(scope const&) = delete;
        ~scope() { gate_->leave(); }

    private:
        drain_gate* gate_;
    };

    class change_awaiter {
    public:
        explicit change_awaiter(drain_gate& gate) noexcept : gate_{&gate} {}

        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> waiter) const noexcept { gate_->waiter_ = waiter; }
        void await_resume() const noexcept {}

    private:
        drain_gate* gate_;
    };

    std::size_t active() const noexcept { return active_; }
    bool closed() const noexcept { return closed_; }
    bool expired() const noexcept { return expired_; }
    void close() noexcept { closed_ = true; }

    void expire() {
        expired_ = true;
        wake();
    }

    change_awaiter changed() noexcept { return change_awaiter{*this}; }

    async::task<void> expire_after(async::duration const limit) {
        auto const keep = shared_from_this();
        co_await loop_->sleep(limit);
        expire();
    }

private:
    void leave() {
        --active_;
        wake();
    }

    void wake() {
        if (waiter_) {
            loop_->post(std::exchange(waiter_, {}));
        }
    }

    async::reactor* loop_;
    std::size_t active_ = 0;
    bool closed_ = false;
    bool expired_ = false;
    std::coroutine_handle<> waiter_{};
};

}  // namespace llmgate::shutdown

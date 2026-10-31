module;

#include "platform.h"

export module llmgate.shutdown;

import std;
import llmgate.async;

export namespace llmgate::shutdown {

constexpr async::duration default_drain = std::chrono::seconds{10};
constexpr int second_signal_status = 130;
}  // namespace llmgate::shutdown

namespace llmgate::shutdown::detail {

inline std::atomic<int> notify_descriptor{-1};
inline std::atomic<int> signals_seen{0};

static_assert(std::atomic<int>::is_always_lock_free);

void on_signal(int const) noexcept {
    if (signals_seen.fetch_add(1) >= 1) {
        platform::exit_immediately(second_signal_status);
    }
    if (auto const descriptor = notify_descriptor.load(); descriptor >= 0) {
        platform::signal_wake(descriptor);
    }
}

}  // namespace llmgate::shutdown::detail

export namespace llmgate::shutdown {

class stop_signal {
public:
    static std::expected<stop_signal, std::error_code> make() {
        auto const channel = platform::open_wake_channel();
        if (channel.error != 0) {
            return std::unexpected{std::error_code{channel.error, std::system_category()}};
        }
        return stop_signal{channel.read_end, channel.write_end};
    }

    stop_signal(stop_signal&& other) noexcept
        : read_{std::exchange(other.read_, -1)}, write_{std::exchange(other.write_, -1)}, installed_{std::exchange(other.installed_, false)} {}
    stop_signal(stop_signal const&) = delete;
    stop_signal& operator=(stop_signal const&) = delete;
    stop_signal& operator=(stop_signal&&) = delete;

    ~stop_signal() {
        uninstall();
        for (auto const descriptor : {read_, write_}) {
            platform::close_wake_end(descriptor);
        }
    }

    int descriptor() const noexcept { return read_; }

    void request() noexcept { platform::signal_wake(write_); }

    void acknowledge() noexcept { platform::clear_wake(read_); }

    std::error_code install() noexcept {
        detail::signals_seen.store(0);
        detail::notify_descriptor.store(write_);
        if (auto const failure = platform::install_stop_handler(&detail::on_signal)) {
            return {failure, std::system_category()};
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
        platform::remove_stop_handler();
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

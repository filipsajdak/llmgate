#pragma once

#include <exception>
#include <expected>
#include <system_error>
#include <utility>

#include <stdexec/execution.hpp>

namespace llmgate::core {

struct pending_op {
    void (*complete)(pending_op*, std::error_code) noexcept;
};

class kqueue_reactor {
public:
    kqueue_reactor();
    ~kqueue_reactor();
    kqueue_reactor(kqueue_reactor const&) = delete;
    kqueue_reactor& operator=(kqueue_reactor const&) = delete;

    std::error_code arm_readable(int fd, pending_op* op) noexcept
        pre(fd >= 0)
        pre(op != nullptr);
    std::error_code poll_once() noexcept;

    auto readable(int fd) noexcept;

    template <stdexec::sender Sender>
    std::expected<void, std::error_code> run(Sender&& sender);

private:
    int kqueue_fd_;
};

namespace detail {

template <class Receiver>
struct readable_op : pending_op {
    readable_op(kqueue_reactor& reactor, int fd, Receiver receiver)
        : pending_op{.complete = &finish}, reactor_{&reactor}, fd_{fd}, receiver_{std::move(receiver)} {}

    void start() noexcept {
        if (auto const error = reactor_->arm_readable(fd_, this)) {
            finish(this, error);
        }
    }

private:
    static void finish(pending_op* self, std::error_code error) noexcept {
        auto& op = *static_cast<readable_op*>(self);
        if (error) {
            stdexec::set_error(std::move(op.receiver_), error);
        } else {
            stdexec::set_value(std::move(op.receiver_));
        }
    }

    kqueue_reactor* reactor_;
    int fd_;
    Receiver receiver_;
};

struct readable_sender {
    using sender_concept = stdexec::sender_t;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t(), stdexec::set_error_t(std::error_code)>;

    kqueue_reactor* reactor;
    int fd;

    template <stdexec::receiver Receiver>
    auto connect(Receiver receiver) const {
        return readable_op<Receiver>(*reactor, fd, std::move(receiver));
    }
};

struct run_state {
    bool done = false;
    std::error_code error;
};

struct run_receiver {
    using receiver_concept = stdexec::receiver_t;

    run_state* state;

    void set_value() noexcept { state->done = true; }
    void set_error(std::error_code error) noexcept {
        state->error = error;
        state->done = true;
    }
    void set_error(std::exception_ptr) noexcept {
        state->error = std::make_error_code(std::errc::state_not_recoverable);
        state->done = true;
    }
    void set_stopped() noexcept {
        state->error = std::make_error_code(std::errc::operation_canceled);
        state->done = true;
    }
    stdexec::env<> get_env() const noexcept { return {}; }
};

}  // namespace detail

inline auto kqueue_reactor::readable(int fd) noexcept {
    return detail::readable_sender{.reactor = this, .fd = fd};
}

template <stdexec::sender Sender>
std::expected<void, std::error_code> kqueue_reactor::run(Sender&& sender) {
    detail::run_state state;
    auto operation = stdexec::connect(std::forward<Sender>(sender), detail::run_receiver{.state = &state});
    stdexec::start(operation);
    while (!state.done) {
        if (auto const error = poll_once()) {
            return std::unexpected{error};
        }
    }
    if (state.error) {
        return std::unexpected{state.error};
    }
    return {};
}

}  // namespace llmgate::core

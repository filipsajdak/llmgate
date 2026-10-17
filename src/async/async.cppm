module;

#include <coroutine>

export module llmgate.async;

import std;

export namespace llmgate::async {

enum class interest { readable, writable };

using duration = std::chrono::milliseconds;

template <class T>
class task;

}  // namespace llmgate::async

namespace llmgate::async::detail {

struct promise_base {
    std::coroutine_handle<> continuation = std::noop_coroutine();

    std::suspend_always initial_suspend() noexcept { return {}; }

    struct finish {
        bool await_ready() const noexcept { return false; }
        template <class Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> finished) const noexcept {
            return finished.promise().continuation;
        }
        void await_resume() const noexcept {}
    };

    finish final_suspend() noexcept { return {}; }
    void unhandled_exception() noexcept { std::terminate(); }
};

template <class T>
struct promise : promise_base {
    std::optional<T> value;

    task<T> get_return_object() noexcept;
    void return_value(T result) { value.emplace(std::move(result)); }
    T take() { return std::move(*value); }
};

template <>
struct promise<void> : promise_base {
    task<void> get_return_object() noexcept;
    void return_void() noexcept {}
    void take() noexcept {}
};

}  // namespace llmgate::async::detail

export namespace llmgate::async {

template <class T>
class [[nodiscard]] task {
public:
    using promise_type = detail::promise<T>;
    using handle = std::coroutine_handle<promise_type>;

    explicit task(handle coroutine) noexcept : coroutine_{coroutine} {}
    task(task&& other) noexcept : coroutine_{std::exchange(other.coroutine_, {})} {}
    task& operator=(task&& other) noexcept {
        if (this != &other) {
            destroy();
            coroutine_ = std::exchange(other.coroutine_, {});
        }
        return *this;
    }
    task(task const&) = delete;
    task& operator=(task const&) = delete;
    ~task() { destroy(); }

    void resume() { coroutine_.resume(); }

    auto operator co_await() && noexcept {
        struct awaiter {
            handle coroutine;
            bool await_ready() const noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) const noexcept {
                coroutine.promise().continuation = caller;
                return coroutine;
            }
            T await_resume() const { return coroutine.promise().take(); }
        };
        return awaiter{coroutine_};
    }

private:
    void destroy() noexcept {
        if (coroutine_) {
            coroutine_.destroy();
        }
    }

    handle coroutine_;
};

}  // namespace llmgate::async

namespace llmgate::async::detail {

template <class T>
task<T> promise<T>::get_return_object() noexcept {
    return task<T>{std::coroutine_handle<promise<T>>::from_promise(*this)};
}

inline task<void> promise<void>::get_return_object() noexcept {
    return task<void>{std::coroutine_handle<promise<void>>::from_promise(*this)};
}

template <class T>
using stored = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

template <class T>
task<void> drive(task<T> root, std::optional<stored<T>>& out, bool& finished) {
    if constexpr (std::is_void_v<T>) {
        co_await std::move(root);
        out.emplace();
    } else {
        out.emplace(co_await std::move(root));
    }
    finished = true;
}

struct pending_op;

struct event_source {
    pending_op* owner;
    bool timer;
};

struct pending_op {
    explicit pending_op(void (*on_complete)(pending_op*, std::error_code) noexcept) noexcept
        : complete{on_complete}, fd_source{.owner = this, .timer = false}, timer_source{.owner = this, .timer = true} {}
    pending_op(pending_op const&) = delete;
    pending_op& operator=(pending_op const&) = delete;

    void (*complete)(pending_op*, std::error_code) noexcept;
    event_source fd_source;
    event_source timer_source;
    std::error_code outcome{};
    int fd = -1;
    int timer_fd = -1;
    interest what = interest::readable;
    bool has_timer = false;
    bool fired = false;
};

}  // namespace llmgate::async::detail

export namespace llmgate::async {

class reactor {
public:
    reactor();
    ~reactor();
    reactor(reactor const&) = delete;
    reactor& operator=(reactor const&) = delete;

    class readiness final : detail::pending_op {
    public:
        readiness(reactor& owner, int fd, interest what, std::optional<duration> timeout) noexcept
            : detail::pending_op{&readiness::finish}, owner_{&owner}, what_{what}, timeout_{timeout} {
            this->fd = fd;
        }

        bool await_ready() const noexcept { return false; }
        bool await_suspend(std::coroutine_handle<> caller) noexcept {
            waiter_ = caller;
            outcome = owner_->arm(fd, what_, timeout_, this);
            return !outcome;
        }
        std::error_code await_resume() const noexcept { return outcome; }

    private:
        static void finish(detail::pending_op* self, std::error_code error) noexcept {
            auto& op = *static_cast<readiness*>(self);
            op.outcome = error;
            op.waiter_.resume();
        }

        reactor* owner_;
        interest what_;
        std::optional<duration> timeout_;
        std::coroutine_handle<> waiter_;
    };

    readiness readable(int const fd, std::optional<duration> const timeout = std::nullopt) noexcept {
        return readiness{*this, fd, interest::readable, timeout};
    }
    readiness writable(int const fd, std::optional<duration> const timeout = std::nullopt) noexcept {
        return readiness{*this, fd, interest::writable, timeout};
    }
    task<void> sleep(duration const length) {
        (void)co_await readiness{*this, -1, interest::readable, length};
    }

    void poll_or_terminate() noexcept;

    template <class T>
    T run(task<T> root) {
        std::optional<detail::stored<T>> result;
        bool finished = false;
        auto driver = detail::drive(std::move(root), result, finished);
        driver.resume();
        while (!finished) {
            poll_or_terminate();
        }
        if constexpr (!std::is_void_v<T>) {
            return std::move(*result);
        }
    }

private:
    std::error_code arm(int fd, interest what, std::optional<duration> timeout, detail::pending_op* op) noexcept
        pre(fd >= -1)
        pre(fd >= 0 || timeout.has_value())
        pre(op != nullptr);

    int poller_fd_;
};

}  // namespace llmgate::async

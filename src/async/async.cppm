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

enum class source_kind { descriptor, timer, other_descriptor, nothing };

struct event_source {
    pending_op* owner;
    source_kind kind;
};

struct pending_op {
    explicit pending_op(void (*on_complete)(pending_op*, std::error_code) noexcept) noexcept
        : complete{on_complete}, fd_source{.owner = this, .kind = source_kind::descriptor},
          timer_source{.owner = this, .kind = source_kind::timer},
          other_source{.owner = this, .kind = source_kind::other_descriptor} {}
    pending_op(pending_op const&) = delete;
    pending_op& operator=(pending_op const&) = delete;

    void (*complete)(pending_op*, std::error_code) noexcept;
    event_source fd_source;
    event_source timer_source;
    event_source other_source;
    std::error_code outcome{};
    source_kind winner = source_kind::descriptor;
    int other_fd = -1;
    int fd = -1;
    int timer_fd = -1;
    interest what = interest::readable;
    bool has_timer = false;
    bool fired = false;
};

class waiter_table {
public:
    bool claim_all(pending_op* const op) {
        if (!claim(op->fd, op->what, op)) {
            return false;
        }
        if (!claim(op->other_fd, interest::readable, op)) {
            release(op->fd, op->what, op);
            return false;
        }
        return true;
    }

    void release_all(pending_op const* const op) noexcept {
        release(op->fd, op->what, op);
        release(op->other_fd, interest::readable, op);
    }

    pending_op* find(int const fd, interest const what) const noexcept {
        auto const found = table_.find(key{fd, what});
        return found == table_.end() ? nullptr : found->second;
    }

private:
    using key = std::pair<int, interest>;

    bool claim(int const fd, interest const what, pending_op* const op) {
        return fd < 0 || table_.try_emplace(key{fd, what}, op).second;
    }

    void release(int const fd, interest const what, pending_op const* const op) noexcept {
        auto const found = table_.find(key{fd, what});
        if (fd >= 0 && found != table_.end() && found->second == op) {
            table_.erase(found);
        }
    }

    std::map<key, pending_op*> table_;
};

}  // namespace llmgate::async::detail

namespace llmgate::async::detail {

class spawn_registry {
public:
    spawn_registry() = default;
    spawn_registry(spawn_registry const&) = delete;
    spawn_registry& operator=(spawn_registry const&) = delete;
    ~spawn_registry() { destroy_unfinished(); }

    void add(std::coroutine_handle<> frame) { live_.insert(frame); }
    void remove(std::coroutine_handle<> frame) noexcept { live_.erase(frame); }

private:
    void destroy_unfinished() noexcept {
        std::vector<std::coroutine_handle<>> unfinished(live_.begin(), live_.end());
        for (auto const frame : unfinished) {
            frame.destroy();
        }
    }

    std::set<std::coroutine_handle<>> live_;
};

struct detached {
    struct promise_type {
        promise_type(spawn_registry& owner, task<void> const&) : owner_{&owner} {
            owner_->add(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        ~promise_type() { owner_->remove(std::coroutine_handle<promise_type>::from_promise(*this)); }

        detached get_return_object() const noexcept { return {}; }
        std::suspend_never initial_suspend() const noexcept { return {}; }
        std::suspend_never final_suspend() const noexcept { return {}; }
        void return_void() const noexcept {}
        void unhandled_exception() const noexcept { std::terminate(); }

    private:
        spawn_registry* owner_;
    };
};

inline detached run_detached(spawn_registry&, task<void> work) {
    co_await std::move(work);
}

}  // namespace llmgate::async::detail

export namespace llmgate::async {

class reactor {
public:
    explicit reactor(std::optional<std::uint64_t> chaos_seed = std::nullopt);
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

    struct race_result {
        std::error_code error;
        bool other_ready;
    };

    class race final : detail::pending_op {
    public:
        race(reactor& owner, int fd, int other, std::optional<duration> timeout) noexcept
            : detail::pending_op{&race::finish}, owner_{&owner}, timeout_{timeout} {
            this->fd = fd;
            this->other_fd = other;
        }

        bool await_ready() const noexcept { return false; }
        bool await_suspend(std::coroutine_handle<> caller) noexcept {
            waiter_ = caller;
            outcome = owner_->arm(fd, interest::readable, timeout_, this);
            return !outcome;
        }
        race_result await_resume() const noexcept {
            return {.error = outcome, .other_ready = !outcome && winner == detail::source_kind::other_descriptor};
        }

    private:
        static void finish(detail::pending_op* self, std::error_code error) noexcept {
            auto& op = *static_cast<race*>(self);
            op.outcome = error;
            op.waiter_.resume();
        }

        reactor* owner_;
        std::optional<duration> timeout_;
        std::coroutine_handle<> waiter_;
    };

    race readable_or_other(int const fd, int const other, std::optional<duration> const timeout = std::nullopt) noexcept {
        return race{*this, fd, other, timeout};
    }

    class yield_awaiter {
    public:
        explicit yield_awaiter(reactor& owner) noexcept : owner_{&owner} {}

        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> waiter) const { owner_->post(waiter); }
        void await_resume() const noexcept {}

    private:
        reactor* owner_;
    };

    yield_awaiter yield() noexcept { return yield_awaiter{*this}; }

    void post(std::coroutine_handle<> waiter) { posted_.push_back(waiter); }

    task<void> sleep(duration const length) {
        (void)co_await readiness{*this, -1, interest::readable, length};
    }

    void spawn(task<void> work) { detail::run_detached(spawned_, std::move(work)); }

    void poll_or_terminate() noexcept;

    template <class T>
    T run(task<T> root) {
        if (std::exchange(running_, true)) {
            std::terminate();
        }
        struct finished_running {
            bool& flag;
            ~finished_running() { flag = false; }
        } const leaving{running_};
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
    std::error_code arm(int const fd, interest const what, std::optional<duration> const timeout,
                        detail::pending_op* const op) noexcept {
        op->fd = fd;
        op->what = what;
        if (!waiters_.claim_all(op)) {
            return std::make_error_code(std::errc::device_or_resource_busy);
        }
        return arm_backend(fd, what, timeout, op);
    }

    std::error_code arm_backend(int fd, interest what, std::optional<duration> timeout, detail::pending_op* op) noexcept
        pre(fd >= -1)
        pre(fd >= 0 || timeout.has_value())
        pre(op != nullptr);
    void release_claims(detail::pending_op* op) noexcept;
    std::error_code sync_descriptor(int fd) noexcept;

    bool has_posted() const noexcept { return !posted_.empty(); }

    void deliver(std::span<detail::pending_op*> ready) noexcept {
        if (chaos_) {
            std::ranges::shuffle(ready, *chaos_);
        }
        for (auto* const op : ready) {
            op->complete(op, op->outcome);
        }
    }

    void run_posted() noexcept {
        auto batch = std::exchange(posted_, {});
        if (chaos_) {
            std::ranges::shuffle(batch, *chaos_);
        }
        for (auto const waiter : batch) {
            waiter.resume();
        }
    }

    int poller_fd_;
    bool running_ = false;
    std::optional<std::mt19937_64> chaos_;
    std::vector<std::coroutine_handle<>> posted_;
    detail::waiter_table waiters_;
    detail::spawn_registry spawned_;
};

class exclusive_scope {
public:
    explicit exclusive_scope(bool& flag) noexcept : flag_{&flag} { flag = true; }
    exclusive_scope(exclusive_scope const&) = delete;
    exclusive_scope& operator=(exclusive_scope const&) = delete;
    ~exclusive_scope() { *flag_ = false; }

private:
    bool* flag_;
};

class mutex {
public:
    explicit mutex(reactor& owner) noexcept : reactor_{&owner} {}
    mutex(mutex&& other) noexcept : reactor_{other.reactor_} {
        if (other.in_use()) {
            std::terminate();
        }
    }
    mutex& operator=(mutex&& other) noexcept {
        if (in_use() || other.in_use()) {
            std::terminate();
        }
        reactor_ = other.reactor_;
        return *this;
    }
    mutex(mutex const&) = delete;
    mutex& operator=(mutex const&) = delete;

    class scoped_lock {
    public:
        explicit scoped_lock(mutex& owner) noexcept : owner_{&owner} {}
        scoped_lock(scoped_lock const&) = delete;
        scoped_lock& operator=(scoped_lock const&) = delete;
        ~scoped_lock() { owner_->unlock(); }

    private:
        mutex* owner_;
    };

    class lock_awaiter {
    public:
        explicit lock_awaiter(mutex& owner) noexcept : owner_{&owner} {}

        bool await_ready() const noexcept { return owner_->try_acquire(); }
        void await_suspend(std::coroutine_handle<> waiter) const { owner_->waiters_.push_back(waiter); }
        scoped_lock await_resume() const noexcept { return scoped_lock{*owner_}; }

    private:
        mutex* owner_;
    };

    lock_awaiter lock() noexcept { return lock_awaiter{*this}; }

    bool in_use() const noexcept { return locked_ || !waiters_.empty(); }

private:

    bool try_acquire() noexcept { return !std::exchange(locked_, true); }

    void unlock() noexcept {
        if (waiters_.empty()) {
            locked_ = false;
            return;
        }
        auto const next = waiters_.front();
        waiters_.pop_front();
        reactor_->post(next);
    }

    reactor* reactor_;
    bool locked_ = false;
    std::deque<std::coroutine_handle<>> waiters_;
};

}  // namespace llmgate::async

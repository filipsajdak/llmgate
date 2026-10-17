export module llmgate.chaos;

import std;
import llmgate.async;
import llmgate.net;

export namespace llmgate::chaos {

template <net::byte_stream Inner>
class stream {
public:
    stream(Inner inner, async::reactor& loop, std::uint64_t const seed)
        : inner_{std::move(inner)}, loop_{&loop}, random_{seed} {}

    async::task<net::result_size> read_some(std::span<std::byte> into, net::read_options const options) {
        if (into.empty()) {
            co_return co_await inner_.read_some(into, options);
        }
        auto const limit = std::uniform_int_distribution<std::size_t>{1, into.size()}(random_);
        if (coin()) {
            co_await loop_->yield();
        }
        co_return co_await inner_.read_some(into.first(limit), options);
    }

    async::task<net::result_void> write_all(std::span<std::byte const> data) {
        if (coin()) {
            co_await loop_->yield();
        }
        co_return co_await inner_.write_all(data);
    }

    int descriptor() const noexcept { return inner_.descriptor(); }

private:
    bool coin() { return (random_() & 1U) != 0; }

    Inner inner_;
    async::reactor* loop_;
    std::mt19937_64 random_;
};

static_assert(net::byte_stream<stream<net::tcp_stream>>);

}  // namespace llmgate::chaos

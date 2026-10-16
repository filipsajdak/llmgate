#include <array>
#include <string>
#include <string_view>

#include <sys/socket.h>
#include <unistd.h>

#include <boost/ut.hpp>

#include "core/kqueue_reactor.hpp"

namespace {

using namespace boost::ut;
using llmgate::core::kqueue_reactor;

constexpr int unopened_descriptor = 99999;

struct socket_pair {
    std::array<int, 2> fds{};
    socket_pair() { ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()); }
    ~socket_pair() {
        ::close(fds[0]);
        ::close(fds[1]);
    }
};

const suite reactor = [] {
    "capability_readable_completes_after_peer_writes"_test = [] {
        kqueue_reactor reactor;
        socket_pair pair;
        std::string received(4, '\0');
        ::write(pair.fds[1], "ping", 4);

        auto result = reactor.run(reactor.readable(pair.fds[0]) | stdexec::then([&] {
                                      ::read(pair.fds[0], received.data(), received.size());
                                  }));

        expect(result.has_value());
        expect(received == std::string_view{"ping"});
    };

    "capability_readable_waits_until_data_arrives"_test = [] {
        kqueue_reactor reactor;
        socket_pair pair;
        bool wrote = false;
        auto work = reactor.readable(pair.fds[0]) | stdexec::then([&] { expect(wrote); });

        auto result = reactor.run(stdexec::just() | stdexec::then([&] {
                                      ::write(pair.fds[1], "x", 1);
                                      wrote = true;
                                  }) |
                                  stdexec::let_value([&] { return std::move(work); }));

        expect(result.has_value());
    };

    "limit_readable_on_unopened_descriptor_reports_error"_test = [] {
        kqueue_reactor reactor;

        auto result = reactor.run(reactor.readable(unopened_descriptor) | stdexec::then([] {}));

        expect(!result.has_value());
    };
};

}  // namespace

int main() {}

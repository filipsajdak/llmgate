#include <boost/ut.hpp>

import llmgate.scenarios;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate::scenarios;

constexpr std::uint64_t writer_seeds = 64;
constexpr std::uint64_t http_seeds = 48;
constexpr std::uint64_t tls_seeds = 24;

void sweep(kind const which, std::uint64_t const seeds) {
    for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
        auto const failure = run(which, seed);
        expect(failure.empty()) << "replay with seed" << seed << failure;
    }
}

const suite schedule_suite = [] {
    "limit_concurrent_writers_never_interleave_under_any_explored_schedule"_test = [] {
        sweep(kind::concurrent_writers, writer_seeds);
    };

    "limit_concurrent_http_clients_each_get_their_own_echo_under_any_explored_schedule"_test = [] {
        sweep(kind::http_clients, http_seeds);
    };

    "limit_a_full_duplex_tls_echo_stays_intact_under_any_explored_schedule"_test = [] {
        sweep(kind::tls_duplex, tls_seeds);
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}

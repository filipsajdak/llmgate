#include <meta>
#include <cstdio>
#include <string_view>

#include <stdexec/execution.hpp>

struct probe {
    int id;
    double weight;
};

consteval std::size_t member_count() {
    return std::meta::nonstatic_data_members_of(^^probe, std::meta::access_context::current()).size();
}

constexpr int checked_double(int const x)
    pre(x >= 0)
    post(r: r == 2 * x)
{
    return 2 * x;
}

int main() {
    static_assert(member_count() == 2);
    auto work = stdexec::just(21) | stdexec::then([](int v) { return checked_double(v); });
    auto [result] = stdexec::sync_wait(std::move(work)).value();
    std::printf("members=%zu result=%d\n", member_count(), result);
    return result == 42 ? 0 : 1;
}

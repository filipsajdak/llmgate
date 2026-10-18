#include <meta>
#include <ranges>
#include <span>
enum class kind { a, b };
struct handles { kind which; };
struct machine {
    int hits = 0;
    [[=handles{kind::a}]] constexpr void on_a() { hits += 1; }
    [[=handles{kind::b}]] constexpr void on_b() { hits += 10; }
    constexpr void accept(kind const which);
};
consteval std::span<std::meta::info const> rules() {
    return std::define_static_array(std::meta::members_of(^^machine, std::meta::access_context::current()) |
                                    std::views::filter([](std::meta::info const m) { return !std::meta::annotations_of_with_type(m, ^^handles).empty(); }));
}
constexpr void machine::accept(kind const which) {
    template for (constexpr auto member : rules()) {
        if (std::meta::extract<handles>(std::meta::annotations_of_with_type(member, ^^handles).front()).which == which) {
            this->[:member:]();
        }
    }
}
constexpr int run() {
    machine m;
    m.accept(kind::a);
    m.accept(kind::b);
    return m.hits;
}
static_assert(run() == 11);
int main() {}

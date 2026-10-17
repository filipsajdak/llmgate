#include <meta>
#include <vector>
enum class K { one, two, three };
template <class E> struct counters {
  struct type;
  consteval { std::vector<std::meta::info> specs;
    for (auto e : std::meta::enumerators_of(^^E)) specs.push_back(std::meta::data_member_spec(^^unsigned long, {.name = std::meta::identifier_of(e)}));
    std::meta::define_aggregate(^^type, specs); }
};
using C = counters<K>::type;
constexpr C c{.one = 1, .two = 2, .three = 3};
static_assert(c.two == 2 && sizeof(C) == 3 * sizeof(unsigned long));
int main() {}

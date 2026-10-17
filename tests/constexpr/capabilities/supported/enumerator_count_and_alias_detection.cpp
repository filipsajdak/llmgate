#include <meta>
enum class E { a, b, c, d = c };
static_assert(std::meta::enumerators_of(^^E).size() == 4);
consteval bool unique() {
  auto es = std::meta::enumerators_of(^^E);
  for (std::size_t i = 0; i < es.size(); ++i) for (std::size_t j = i + 1; j < es.size(); ++j)
    if (std::meta::extract<E>(es[i]) == std::meta::extract<E>(es[j])) return false;
  return true;
}
static_assert(!unique());
int main() {}

#include <meta>
#include <optional>
#include <string_view>
enum class C { serve, login };
constexpr std::optional<C> from(std::string_view n) { std::optional<C> r;
  template for (constexpr auto e : std::define_static_array(std::meta::enumerators_of(^^C))) if (n == std::meta::identifier_of(e)) r = [:e:];
  return r; }
static_assert(from("login") == C::login && !from("x"));
int main() {}

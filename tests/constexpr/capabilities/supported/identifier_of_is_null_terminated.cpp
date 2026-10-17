#include <meta>
#include <string_view>
enum class E { alpha };
constexpr auto e = std::meta::enumerators_of(^^E)[0];
constexpr std::string_view n = std::meta::identifier_of(e);
static_assert(n == "alpha" && n.data()[n.size()] == '\0');
int main() {}

#include <meta>
#include <string>
#include <string_view>
enum class L { ok, warning };
consteval std::string_view nm(L v) { for (auto e : std::meta::enumerators_of(^^L)) if (std::meta::extract<L>(e) == v) return std::meta::identifier_of(e); return {}; }
constexpr std::string s() { return std::string{nm(L::ok)}; }
static_assert(s() == "ok");
int main() {}

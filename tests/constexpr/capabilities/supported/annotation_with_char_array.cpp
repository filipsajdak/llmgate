#include <meta>
#include <string_view>
struct key { char text[16]; };
consteval key make(std::string_view s) { key k{}; for (std::size_t i = 0; i < s.size(); ++i) k.text[i] = s[i]; return k; }
struct S { [[=make("wire")]] int a; };
constexpr auto m = std::meta::nonstatic_data_members_of(^^S, std::meta::access_context::current())[0];
constexpr auto a = std::meta::annotations_of_with_type(m, ^^key)[0];
static_assert(std::string_view{std::meta::extract<key>(a).text} == "wire");
int main() {}

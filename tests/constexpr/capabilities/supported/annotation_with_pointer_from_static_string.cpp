#include <meta>
#include <string_view>
struct key { const char* text; };
consteval key make(std::string_view s) { return key{std::define_static_string(s)}; }
struct S { [[=make("wire")]] int a; };
constexpr auto m = std::meta::nonstatic_data_members_of(^^S, std::meta::access_context::current())[0];
constexpr auto a = std::meta::annotations_of_with_type(m, ^^key)[0];
static_assert(std::string_view{std::meta::extract<key>(a).text} == "wire");
int main() {}

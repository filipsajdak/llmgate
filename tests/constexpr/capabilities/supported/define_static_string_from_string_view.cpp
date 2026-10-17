#include <meta>
#include <string_view>
consteval const char* f(std::string_view s) { return std::define_static_string(s); }
constexpr const char* p = f("hello");
static_assert(std::string_view{p} == "hello");
int main() {}

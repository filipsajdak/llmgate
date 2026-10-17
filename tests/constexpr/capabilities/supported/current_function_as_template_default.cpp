#include <meta>
#include <string_view>
template <std::meta::info Caller = std::meta::current_function()> constexpr bool callee() { return std::meta::identifier_of(Caller) == "caller"; }
constexpr bool caller() { return callee(); }
static_assert(caller());
int main() {}

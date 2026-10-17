#include <meta>
struct skip {};
struct S { [[=skip{}]] int a; int b; };
consteval bool has_skip(std::meta::info m) { return !std::meta::annotations_of_with_type(m, ^^skip).empty(); }
constexpr auto ms = std::define_static_array(std::meta::nonstatic_data_members_of(^^S, std::meta::access_context::current()));
static_assert(has_skip(ms[0]) && !has_skip(ms[1]));
int main() {}

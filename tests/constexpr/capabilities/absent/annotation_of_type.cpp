#include <meta>
struct skip {};
struct S { [[=skip{}]] int a; };
constexpr auto m = std::meta::nonstatic_data_members_of(^^S, std::meta::access_context::current())[0];
constexpr auto a = std::meta::annotation_of_type<skip>(m);
int main() {}

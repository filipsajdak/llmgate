#include <meta>
struct B { int x; };
struct D : B { int y; };
static_assert(std::meta::subobjects_of(^^D, std::meta::access_context::current()).size() == 2);
static_assert(std::meta::nonstatic_data_members_of(^^D, std::meta::access_context::current()).size() == 1);
int main() {}

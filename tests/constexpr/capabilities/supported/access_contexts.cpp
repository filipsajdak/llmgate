#include <meta>
class C { int hidden = 1; public: int shown = 2; };
static_assert(std::meta::nonstatic_data_members_of(^^C, std::meta::access_context::unchecked()).size() == 2);
static_assert(std::meta::nonstatic_data_members_of(^^C, std::meta::access_context::unprivileged()).size() == 1);
int main() {}

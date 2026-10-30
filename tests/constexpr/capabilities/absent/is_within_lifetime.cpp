#include <type_traits>
union U { int a; char b; };
constexpr bool check() { U u{.a = 1}; return std::is_within_lifetime(&u.a); }
static_assert(check());
int main() {}

#include <meta>
struct S { int a = 0; long b = 0; };
constexpr S fill() { S s; int n = 1;
  template for (constexpr auto m : std::define_static_array(std::meta::nonstatic_data_members_of(^^S, std::meta::access_context::current()))) s.[:m:] = n++;
  return s; }
static_assert(fill().a == 1 && fill().b == 2);
int main() {}

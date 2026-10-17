#include <meta>
#include <format>
#include <string>
#include <type_traits>
template <class T> requires std::is_aggregate_v<T> && (!std::is_array_v<T>)
struct std::formatter<T> {
  constexpr auto parse(std::format_parse_context& c) { return c.begin(); }
  auto format(T const& v, std::format_context& ctx) const {
    auto out = ctx.out(); bool first = true; *out++ = '{';
    template for (constexpr auto m : std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()))) {
      if (!first) { *out++ = ','; *out++ = ' '; } first = false;
      out = std::format_to(out, "{}={}", std::meta::identifier_of(m), v.[:m:]);
    }
    *out++ = '}'; return out;
  }
};
struct P { int x; int y; };
int main() { return std::format("{}", P{1, 2}) == "{x=1, y=2}" ? 0 : 1; }

#include <meta>
#include <string_view>
constexpr char text[] = {
#embed __FILE__
};
static_assert(std::string_view{text, sizeof text}.starts_with("#include"));
int main() {}

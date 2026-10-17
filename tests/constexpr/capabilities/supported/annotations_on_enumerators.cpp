#include <meta>
struct tolerant {};
enum class E { a [[=tolerant{}]], b };
constexpr auto es = std::define_static_array(std::meta::enumerators_of(^^E));
static_assert(!std::meta::annotations_of_with_type(es[0], ^^tolerant).empty() && std::meta::annotations_of_with_type(es[1], ^^tolerant).empty());
int main() {}

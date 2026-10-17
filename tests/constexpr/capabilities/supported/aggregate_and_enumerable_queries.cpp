#include <meta>
struct A { int x; };
struct N { N(int); int x; };
enum class E { a };
static_assert(std::meta::is_aggregate_type(^^A) && !std::meta::is_aggregate_type(^^N) && std::meta::is_enumerable_type(^^E));
int main() {}

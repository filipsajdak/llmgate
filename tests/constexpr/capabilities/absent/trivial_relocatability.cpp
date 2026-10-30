#include <type_traits>
static_assert(std::is_trivially_relocatable_v<int>);
int main() {}

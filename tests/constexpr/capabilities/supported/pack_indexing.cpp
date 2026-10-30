template <class... T> constexpr auto first(T... values) { return values...[0]; }
static_assert(first(7, 8, 9) == 7);
int main() {}

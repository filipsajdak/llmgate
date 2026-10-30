#include <functional>
int main() { std::copyable_function<int()> f = [] { return 0; }; return f(); }

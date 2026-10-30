#include <generator>
std::generator<int> count(int n) { for (int i = 0; i < n; ++i) { co_yield i; } }
int main() { int total = 0; for (int i : count(3)) { total += i; } return total - 3; }

#include <inplace_vector>
static_assert([] { std::inplace_vector<int, 4> v; v.push_back(1); v.push_back(2); return v.size(); }() == 2);
int main() {}

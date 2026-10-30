#include <flat_map>
int main() { std::flat_map<int, int> m; m[1] = 2; return m.size() == 1 ? 0 : 1; }

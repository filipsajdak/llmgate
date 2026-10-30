#include <optional>
int main() { int x = 1; std::optional<int&> o{x}; return *o - 1; }

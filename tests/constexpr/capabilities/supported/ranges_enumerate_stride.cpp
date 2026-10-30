#include <ranges>
#include <vector>
int main() {
    std::vector<int> v{1, 2, 3, 4};
    for (auto [index, value] : std::views::enumerate(v)) { (void)index; (void)value; }
    for (int value : v | std::views::stride(2)) { (void)value; }
}

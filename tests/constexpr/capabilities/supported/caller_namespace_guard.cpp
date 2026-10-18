#include <meta>
namespace layer {
namespace app {}
consteval bool within(std::meta::info at, std::meta::info const scope) {
    while (at != ^^::) {
        if (at == scope) {
            return true;
        }
        at = std::meta::parent_of(at);
    }
    return false;
}
template <std::meta::info Caller = std::meta::current_function()>
int restricted() {
    static_assert(!within(Caller, ^^layer) || within(Caller, ^^layer::app), "restricted() is for layer::app");
    return 1;
}
namespace app {
struct S { int member() { return restricted(); } };
int direct() { return restricted(); }
int in_lambda() { return [] { return restricted(); }(); }
}
}
namespace { int outside() { return layer::restricted(); } }
int main() { return layer::app::direct() + layer::app::in_lambda() + layer::app::S{}.member() + outside(); }

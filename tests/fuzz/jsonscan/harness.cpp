import llmgate.fuzz_driver;
import llmgate.properties;
import std;

namespace {

void check(std::span<std::byte const> input) {
    std::string_view const text{reinterpret_cast<char const*>(input.data()), input.size()};
    if (!llmgate::properties::jsonscan_holds(text)) {
        std::abort();
    }
}

}  // namespace

int main(int argc, char** argv) {
    return llmgate::fuzz::run(argc, argv, check);
}

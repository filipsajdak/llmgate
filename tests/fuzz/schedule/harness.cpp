import llmgate.fuzz_driver;
import llmgate.reflect;
import llmgate.scenarios;
import std;

namespace {

constexpr std::size_t seed_bytes = 8;

void check(std::span<std::byte const> input) {
    if (input.size() <= seed_bytes) {
        return;
    }
    std::uint64_t seed = 0;
    std::memcpy(&seed, input.data(), seed_bytes);
    auto const which = static_cast<llmgate::scenarios::kind>(std::to_integer<unsigned>(input[seed_bytes]) % llmgate::reflect::enum_count<llmgate::scenarios::kind>);
    if (!llmgate::scenarios::run(which, seed).empty()) {
        std::abort();
    }
}

}  // namespace

int main(int argc, char** argv) {
    return llmgate::fuzz::run(argc, argv, check);
}

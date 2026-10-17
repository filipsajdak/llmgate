import llmgate.fuzz_driver;
import llmgate.scenarios;
import std;

namespace {

constexpr std::size_t seed_bytes = 8;
constexpr std::size_t scenario_count = 3;

void check(std::span<std::byte const> input) {
    if (input.size() <= seed_bytes) {
        return;
    }
    std::uint64_t seed = 0;
    std::memcpy(&seed, input.data(), seed_bytes);
    auto const which = static_cast<llmgate::scenarios::kind>(std::to_integer<unsigned>(input[seed_bytes]) % scenario_count);
    if (!llmgate::scenarios::run(which, seed).empty()) {
        std::abort();
    }
}

}  // namespace

int main(int argc, char** argv) {
    return llmgate::fuzz::run(argc, argv, check);
}

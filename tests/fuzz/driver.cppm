export module llmgate.fuzz_driver;

import std;

export namespace llmgate::fuzz {

inline std::vector<std::byte> read_all(std::istream& in) {
    std::vector<std::byte> bytes;
    std::array<char, 4096> block{};
    while (in.read(block.data(), block.size()) || in.gcount() > 0) {
        auto const got = static_cast<std::size_t>(in.gcount());
        bytes.insert(bytes.end(), reinterpret_cast<std::byte const*>(block.data()),
                     reinterpret_cast<std::byte const*>(block.data()) + got);
    }
    return bytes;
}

template <std::invocable<std::span<std::byte const>> Target>
int run(int const argc, char** const argv, Target&& target) {
    if (argc < 2) {
        auto const input = read_all(std::cin);
        target(input);
        return 0;
    }
    std::size_t replayed = 0;
    for (int index = 1; index < argc; ++index) {
        std::filesystem::path const given{argv[index]};
        std::vector<std::filesystem::path> files;
        if (std::filesystem::is_directory(given)) {
            for (auto const& entry : std::filesystem::directory_iterator{given}) {
                if (entry.is_regular_file()) {
                    files.push_back(entry.path());
                }
            }
        } else {
            files.push_back(given);
        }
        for (auto const& file : files) {
            std::ifstream in{file, std::ios::binary};
            auto const input = read_all(in);
            target(input);
            ++replayed;
        }
    }
    std::println("replayed {} inputs", replayed);
    return replayed == 0 ? 1 : 0;
}

}  // namespace llmgate::fuzz

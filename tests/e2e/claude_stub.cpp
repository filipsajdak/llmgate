#include <stdlib.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#elif !defined(_WIN32)
extern "C" char** environ;
#endif

import std;

namespace {

char** environment_block() {
#if defined(__APPLE__)
    return *_NSGetEnviron();
#else
    return environ;
#endif
}

}  // namespace

int main(int argc, char** argv) {
    auto const* destination = std::getenv("STUB_OUTPUT");
    if (destination == nullptr) {
        return 2;
    }
    std::string text = "ARGS:";
    std::string_view const joined{" "};
    for (auto const* word : std::span{argv, static_cast<std::size_t>(argc)}.subspan(1)) {
        text += joined;
        text += word;
    }
    text += '\n';
    std::vector<std::string> prefixes;
    if (auto const* wanted = std::getenv("STUB_ENV_PREFIXES")) {
        for (auto const part : std::views::split(std::string_view{wanted}, ',')) {
            prefixes.emplace_back(part.begin(), part.end());
        }
    }
    std::vector<std::string> kept;
    for (auto const* const* entry = environment_block(); entry != nullptr && *entry != nullptr; ++entry) {
        std::string_view const line{*entry};
        if (std::ranges::any_of(prefixes, [line](std::string const& prefix) { return line.starts_with(prefix); })) {
            kept.emplace_back(line);
        }
    }
    std::ranges::sort(kept);
    for (auto const& line : kept) {
        text += line;
        text += '\n';
    }
    std::ofstream out{destination, std::ios::binary};
    out << text;
    return out ? 0 : 1;
}

export module llmgate.text;

import std;

export namespace llmgate::text {

template <std::integral T>
constexpr std::string decimal(T const value) {
    std::array<char, 24> digits{};
    return std::string{digits.data(), std::to_chars(digits.data(), digits.data() + digits.size(), value).ptr};
}

constexpr std::uint64_t thousand = 1000;
constexpr std::uint64_t mega = 1000000;
constexpr std::uint64_t mega_tenth = 100000;

constexpr std::string compact(std::uint64_t const value) {
    if (value < thousand) {
        return text::decimal(value);
    }
    if (value < mega) {
        return text::decimal(value / thousand) + "k";
    }
    return text::decimal(value / mega) + "." + text::decimal(value % mega / mega_tenth) + "M";
}

static_assert(compact(999) == "999" && compact(1000) == "1k" && compact(412345) == "412k" && compact(1250000) == "1.2M");
static_assert(decimal(0) == "0" && decimal(-5) == "-5" && decimal(std::uint64_t{18446744073709551615ULL}) == "18446744073709551615");

}  // namespace llmgate::text

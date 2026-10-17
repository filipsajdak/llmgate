module;

export module llmgate.doc;

import std;

export namespace llmgate::reflect {

constexpr std::size_t doc_capacity = 400;
constexpr std::size_t example_capacity = 640;

template <std::size_t Capacity>
struct fixed_text {
    char text[Capacity]{};

    consteval fixed_text(std::string_view const words) {
        if (words.size() >= Capacity) {
            throw "reflect::fixed_text: the text does not fit in its capacity";
        }
        std::ranges::copy(words, text);
    }
};

struct doc : fixed_text<doc_capacity> {
    using fixed_text::fixed_text;
};

struct example : fixed_text<example_capacity> {
    using fixed_text::fixed_text;
};

struct commented_t {};
inline constexpr commented_t commented{};

}  // namespace llmgate::reflect

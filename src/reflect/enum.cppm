module;

#include <meta>

export module llmgate.reflect;

import std;

export namespace llmgate::reflect {

constexpr std::size_t meaning_capacity = 200;

struct meaning {
    char text[meaning_capacity]{};

    consteval meaning(std::string_view const words) {
        if (words.size() >= meaning_capacity) {
            throw "reflect::meaning: the text does not fit in its capacity";
        }
        std::ranges::copy(words, text);
    }
};

template <class E>
struct enumerator_entry {
    E value;
    char const* name;
    char const* meaning_text;
};

template <class E>
    requires std::is_enum_v<E>
consteval std::span<enumerator_entry<E> const> enumerator_table() {
    return std::define_static_array(std::meta::enumerators_of(^^E) | std::views::transform([](std::meta::info const item) {
                                        auto const notes = std::meta::annotations_of_with_type(item, ^^meaning);
                                        return enumerator_entry<E>{.value = std::meta::extract<E>(item),
                                                                   .name = std::define_static_string(std::meta::identifier_of(item)),
                                                                   .meaning_text = std::define_static_string(notes.empty() ? std::string_view{} : std::string_view{std::meta::extract<meaning>(notes.front()).text})};
                                    }));
}

template <class E>
    requires std::is_enum_v<E>
inline constexpr std::size_t enum_count = std::meta::enumerators_of(^^E).size();

template <class E>
    requires std::is_enum_v<E>
inline constexpr std::span<E const> enum_values = std::define_static_array(std::meta::enumerators_of(^^E) | std::views::transform([](std::meta::info const item) {
                                                                               return std::meta::extract<E>(item);
                                                                           }));

template <class E>
    requires std::is_enum_v<E>
constexpr bool unique_values() {
    auto const values = enum_values<E>;
    for (std::size_t at = 0; at < values.size(); ++at) {
        if (std::ranges::contains(values.subspan(at + 1), values[at])) {
            return false;
        }
    }
    return true;
}

template <class E>
    requires std::is_enum_v<E>
consteval bool starts_at_one() {
    return enum_count<E> != 0 && std::to_underlying(enum_values<E>.front()) == 1;
}

template <class E>
    requires std::is_enum_v<E>
constexpr std::string_view enum_name(E const value) noexcept {
    static constexpr auto table = enumerator_table<E>();
    auto const match = std::ranges::find(table, value, &enumerator_entry<E>::value);
    return match == table.end() ? std::string_view{} : std::string_view{match->name};
}

constexpr std::string copy_of(std::string_view const text) {
    std::string out;
    out.reserve(text.size());
    for (auto const c : text) {
        out.push_back(c);
    }
    return out;
}

template <class E>
    requires std::is_enum_v<E>
constexpr std::string enum_string(E const value) {
    return copy_of(enum_name(value));
}

template <class E>
    requires std::is_enum_v<E>
constexpr std::optional<E> enum_from_name(std::string_view const name) noexcept {
    static constexpr auto table = enumerator_table<E>();
    auto const match = std::ranges::find_if(table, [name](enumerator_entry<E> const& entry) { return name == entry.name; });
    return match == table.end() ? std::nullopt : std::optional<E>{match->value};
}

constexpr char module_marker = '@';

consteval std::string_view without_module(std::string_view const display) {
    return display.substr(0, display.find(module_marker));
}

template <class E>
    requires std::is_enum_v<E>
class enum_category final : public std::error_category {
public:
    char const* name() const noexcept override {
        static constexpr char const* qualified = std::define_static_string(without_module(std::meta::display_string_of(^^E)));
        return qualified;
    }
    std::string message(int const value) const override {
        static constexpr auto table = enumerator_table<E>();
        auto const match = std::ranges::find(table, static_cast<E>(value), &enumerator_entry<E>::value);
        if (match != table.end() && *match->meaning_text != '\0') {
            return std::string{match->meaning_text};
        }
        return std::string{enum_name(static_cast<E>(value))};
    }
};

template <class E>
    requires std::is_enum_v<E>
std::error_code make_code(E const value) noexcept {
    static enum_category<E> const category;
    return {static_cast<int>(value), category};
}

struct error_enum_t {};
inline constexpr error_enum_t error_enum{};

template <class E>
concept error_enum_type = std::is_enum_v<E> && !std::meta::annotations_of_with_type(^^E, ^^error_enum_t).empty();

template <error_enum_type E>
std::error_code make_error_code(E const value) noexcept {
    return make_code(value);
}

}  // namespace llmgate::reflect

export namespace std {
template <llmgate::reflect::error_enum_type E>
struct is_error_code_enum<E> : true_type {};
}  // namespace std

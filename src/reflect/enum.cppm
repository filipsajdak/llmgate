module;

#include <meta>

export module llmgate.reflect;

import std;

export namespace llmgate::reflect {

template <class E>
struct enumerator_entry {
    E value;
    char const* name;
};

template <class E>
    requires std::is_enum_v<E>
consteval std::span<enumerator_entry<E> const> enumerator_table() {
    return std::define_static_array(std::meta::enumerators_of(^^E) | std::views::transform([](std::meta::info const item) {
                                        return enumerator_entry<E>{.value = std::meta::extract<E>(item),
                                                                   .name = std::define_static_string(std::meta::identifier_of(item))};
                                    }));
}

template <class E>
    requires std::is_enum_v<E>
constexpr std::string_view enum_name(E const value) noexcept {
    static constexpr auto table = enumerator_table<E>();
    auto const match = std::ranges::find(table, value, &enumerator_entry<E>::value);
    return match == table.end() ? std::string_view{} : std::string_view{match->name};
}

template <class E>
    requires std::is_enum_v<E>
constexpr std::optional<E> enum_from_name(std::string_view const name) noexcept {
    static constexpr auto table = enumerator_table<E>();
    auto const match = std::ranges::find_if(table, [name](enumerator_entry<E> const& entry) { return name == entry.name; });
    return match == table.end() ? std::nullopt : std::optional<E>{match->value};
}

template <class E>
    requires std::is_enum_v<E>
class enum_category final : public std::error_category {
public:
    char const* name() const noexcept override {
        static constexpr char const* qualified = std::define_static_string(std::meta::display_string_of(^^E));
        return qualified;
    }
    std::string message(int const value) const override {
        return std::string{enum_name(static_cast<E>(value))};
    }
};

template <class E>
    requires std::is_enum_v<E>
std::error_code make_code(E const value) noexcept {
    static enum_category<E> const category;
    return {static_cast<int>(value), category};
}

}  // namespace llmgate::reflect

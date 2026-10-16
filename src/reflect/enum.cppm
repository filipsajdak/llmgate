module;

#include <meta>

export module llmgate.reflect;

import std;

export namespace llmgate::reflect {

template <class E>
    requires std::is_enum_v<E>
constexpr std::string_view enum_name(E const value) noexcept {
    template for (constexpr auto enumerator : std::define_static_array(std::meta::enumerators_of(^^E))) {
        if (value == [:enumerator:]) {
            return std::meta::identifier_of(enumerator);
        }
    }
    return {};
}

template <class E>
    requires std::is_enum_v<E>
constexpr std::optional<E> enum_from_name(std::string_view const name) noexcept {
    template for (constexpr auto enumerator : std::define_static_array(std::meta::enumerators_of(^^E))) {
        if (name == std::meta::identifier_of(enumerator)) {
            return [:enumerator:];
        }
    }
    return std::nullopt;
}

}  // namespace llmgate::reflect

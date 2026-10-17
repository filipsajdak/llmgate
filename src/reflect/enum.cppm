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

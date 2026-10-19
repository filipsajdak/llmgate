export module llmgate.role;

import std;
import llmgate.reflect;

export namespace llmgate::roles {

enum class role { opus, sonnet, haiku, fable, other };
static_assert(reflect::unique_values<role>() && reflect::enum_values<role>.back() == role::other);

constexpr role fallback = role::other;

constexpr std::string_view name_of(role const value) noexcept {
    return reflect::enum_name(value);
}

constexpr std::optional<role> from_name(std::string_view const name) noexcept {
    return reflect::enum_from_name<role>(name);
}

constexpr std::span<role const> named_families() noexcept {
    return reflect::enum_values<role>.first(reflect::enum_count<role> - 1);
}

constexpr bool mentions(std::string_view const text, std::string_view const word) {
    auto const lower = [](char const c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    return !std::ranges::search(text, word, {}, lower, {}).empty();
}

constexpr role of(std::string_view const requested) {
    auto const family = std::ranges::find_if(named_families(), [requested](role const candidate) { return mentions(requested, name_of(candidate)); });
    return family == named_families().end() ? fallback : *family;
}

constexpr std::string list_text() {
    std::string out;
    for (auto const value : named_families()) {
        out += reflect::enum_string(value);
        out += value == named_families().back() ? " or " : ", ";
    }
    return out + reflect::enum_string(fallback);
}

static_assert(of("claude-Sonnet-5-5") == role::sonnet && of("claude-haiku-5-5") == role::haiku && of("claude-OPUS-5") == role::opus && of("gpt") == role::other);

}  // namespace llmgate::roles

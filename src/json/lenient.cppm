module;

#include <meta>

export module llmgate.lenient;

import std;
import llmgate.json;
import llmgate.members;

namespace llmgate::lenient::detail {

template <class T>
concept optional_target = requires { typename T::value_type; } && std::same_as<T, std::optional<typename T::value_type>>;
template <class T>
concept count_target = std::unsigned_integral<T> && !std::same_as<T, bool>;
template <class T>
concept whole_target = std::signed_integral<T>;
template <class T>
concept flag_target = std::same_as<T, bool>;
template <class T>
concept real_target = std::floating_point<T>;
template <class T>
concept text_target = std::same_as<T, std::string>;
template <class T>
concept record_target = std::is_aggregate_v<T> && !std::ranges::range<T>;

using scrubbed = std::optional<json::value>;

consteval std::string_view wire_of(std::meta::info const member) {
    auto const renamed = reflect::annotation_of<json::key>(member);
    std::string_view const wire = renamed ? std::string_view{renamed->text} : std::meta::identifier_of(member);
    return {std::define_static_string(wire), wire.size()};
}

template <count_target T>
constexpr scrubbed scrub(json::value const& source) {
    auto const whole = json::count_at(source, {});
    if (!whole) {
        return std::nullopt;
    }
    return json::value{static_cast<std::int64_t>(std::min<std::uint64_t>(*whole, std::numeric_limits<T>::max()))};
}

template <whole_target T>
constexpr scrubbed scrub(json::value const& source) {
    auto const* whole = std::get_if<std::int64_t>(&source.data);
    if (whole == nullptr) {
        return std::nullopt;
    }
    return json::value{std::clamp<std::int64_t>(*whole, std::numeric_limits<T>::min(), std::numeric_limits<T>::max())};
}

template <flag_target T>
constexpr scrubbed scrub(json::value const& source) {
    return std::holds_alternative<bool>(source.data) ? scrubbed{source} : std::nullopt;
}

template <real_target T>
constexpr scrubbed scrub(json::value const& source) {
    return std::holds_alternative<std::int64_t>(source.data) || std::holds_alternative<double>(source.data) ? scrubbed{source} : std::nullopt;
}

template <text_target T>
constexpr scrubbed scrub(json::value const& source) {
    return std::holds_alternative<std::string>(source.data) ? scrubbed{source} : std::nullopt;
}

template <optional_target T>
constexpr scrubbed scrub(json::value const& source);

template <record_target T>
constexpr scrubbed scrub(json::value const& source);

template <class T>
constexpr void scrub_members(json::object& kept, json::object const& members);

template <class Member>
constexpr void scrub_plain(json::object& kept, json::object const& members, std::string_view const name) {
    auto const found = std::ranges::find(members, name, &json::object::value_type::first);
    if (found == members.end()) {
        return;
    }
    if (auto clean = scrub<Member>(found->second)) {
        kept.emplace_back(std::string{name}, std::move(*clean));
    }
}

template <std::meta::info field>
constexpr void scrub_field(json::object& kept, json::object const& members) {
    using member = typename [:std::meta::type_of(field):];
    if constexpr (reflect::has_annotation<json::skip_t>(field)) {
        return;
    } else if constexpr (reflect::has_annotation<json::flatten_t>(field)) {
        scrub_members<member>(kept, members);
    } else {
        scrub_plain<member>(kept, members, wire_of(field));
    }
}

template <class T>
constexpr void scrub_members(json::object& kept, json::object const& members) {
    static constexpr auto fields = std::define_static_array(
        std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()));
    template for (constexpr auto field : fields) {
        scrub_field<field>(kept, members);
    }
}

template <optional_target T>
constexpr scrubbed scrub(json::value const& source) {
    return std::holds_alternative<std::nullptr_t>(source.data) ? scrubbed{source} : scrub<typename T::value_type>(source);
}

template <record_target T>
constexpr scrubbed scrub(json::value const& source) {
    auto const* members = std::get_if<json::object>(&source.data);
    if (members == nullptr) {
        return std::nullopt;
    }
    json::object kept;
    scrub_members<T>(kept, *members);
    return json::value{std::move(kept)};
}

}  // namespace llmgate::lenient::detail

export namespace llmgate::lenient {

template <class T>
concept readable = detail::record_target<T>;

template <readable T>
constexpr std::expected<T, json::read_error> read(json::value const& source) {
    auto const clean = detail::scrub<T>(source);
    if (!clean) {
        return std::unexpected{json::read_error{.path = {}, .message = "expected an object"}};
    }
    return json::read<T>(*clean);
}

}  // namespace llmgate::lenient

template class std::optional<llmgate::json::value>;

module;

#include <meta>

export module llmgate.format;

import std;
import llmgate.json;
import llmgate.members;
import llmgate.reflect;

namespace llmgate::format::detail {

template <class T>
struct is_optional : std::false_type {};
template <class U>
struct is_optional<std::optional<U>> : std::true_type {};

template <class T>
concept optional_like = is_optional<T>::value;

template <class T>
concept text_like = std::convertible_to<T const&, std::string_view>;

template <class T>
concept map_like = std::ranges::range<T> && requires { typename T::mapped_type; };

template <class T>
concept sequence_like = std::ranges::range<T> && !text_like<T> && !map_like<T>;

constexpr std::string_view null_text = "null";
constexpr std::string_view separator = ", ";

template <std::output_iterator<char const&> Out>
constexpr Out put(Out out, std::string_view const text) {
    return std::ranges::copy(text, out).out;
}

template <std::output_iterator<char const&> Out, class T>
Out write_value(Out out, T const& value);

template <std::output_iterator<char const&> Out, class T>
Out write_enum(Out out, T const value) {
    auto const name = reflect::enum_name(value);
    return name.empty() ? std::format_to(out, "{}", std::to_underlying(value)) : put(out, name);
}

template <std::output_iterator<char const&> Out, class T>
Out write_items(Out out, T const& items) {
    out = put(out, "[");
    bool first = true;
    for (auto const& item : items) {
        out = write_value(first ? out : put(out, separator), item);
        first = false;
    }
    return put(out, "]");
}

template <std::output_iterator<char const&> Out, class T>
Out write_entries(Out out, T const& entries) {
    out = put(out, "{");
    bool first = true;
    for (auto const& [name, item] : entries) {
        out = write_value(put(write_value(first ? out : put(out, separator), name), ": "), item);
        first = false;
    }
    return put(out, "}");
}

template <std::output_iterator<char const&> Out, class T>
Out write_value(Out out, T const& value) {
    if constexpr (std::is_enum_v<T>) {
        return write_enum(out, value);
    } else if constexpr (text_like<T>) {
        return std::format_to(out, "{:?}", std::string_view{value});
    } else if constexpr (optional_like<T>) {
        return value ? write_value(out, *value) : put(out, null_text);
    } else if constexpr (map_like<T>) {
        return write_entries(out, value);
    } else if constexpr (sequence_like<T>) {
        return write_items(out, value);
    } else {
        return std::format_to(out, "{}", value);
    }
}

template <std::output_iterator<char const&> Out, class T, std::meta::info field>
Out write_field(Out out, T const& source, bool& first) {
    if constexpr (!reflect::has_annotation<json::skip_t>(field)) {
        out = put(first ? out : put(out, separator), std::meta::identifier_of(field));
        out = put(out, "=");
        first = false;
        if constexpr (reflect::has_annotation<json::redact_t>(field)) {
            out = put(out, json::redacted_text);
        } else {
            out = write_value(out, source.[:field:]);
        }
    }
    return out;
}

}  // namespace llmgate::format::detail

export namespace llmgate::format {

template <class T>
concept printable_aggregate = std::is_class_v<T> && std::is_aggregate_v<T> && !std::ranges::range<T>;

template <std::output_iterator<char const&> Out, printable_aggregate T>
Out write_aggregate(Out out, T const& source) {
    static constexpr auto fields = std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()));
    bool first = true;
    out = detail::put(out, "{");
    template for (constexpr auto field : fields) {
        out = detail::write_field<Out, T, field>(out, source, first);
    }
    return detail::put(out, "}");
}

}  // namespace llmgate::format

export namespace std {

template <llmgate::format::printable_aggregate T>
struct formatter<T> {
    constexpr auto parse(format_parse_context& context) { return context.begin(); }

    auto format(T const& source, format_context& context) const { return llmgate::format::write_aggregate(context.out(), source); }
};

}  // namespace std

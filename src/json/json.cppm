module;

#include <meta>

export module llmgate.json;

import std;
import llmgate.reflect;

export namespace llmgate::json {

class value;

using array = std::vector<value>;
using object = std::vector<std::pair<std::string, value>>;

class value {
public:
    using storage = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, array, object>;

    constexpr value() = default;
    constexpr explicit value(storage content) : data{std::move(content)} {}

    storage data{nullptr};
};

struct parse_error {
    std::size_t offset;
    std::string message;
};

struct parse_options {
    bool jsonc = true;
    std::size_t max_depth = 64;
};

struct read_error {
    std::string path;
    std::string message;
};

struct required_t {};
inline constexpr required_t required{};

}  // namespace llmgate::json

namespace llmgate::json::detail {

class parser {
public:
    constexpr parser(std::string_view const text, parse_options const options) noexcept : text_{text}, options_{options} {}

    constexpr std::expected<value, parse_error> run() {
        auto parsed = parse_value(0);
        if (!parsed) {
            return parsed;
        }
        skip_filler();
        if (at_ != text_.size()) {
            return fail("unexpected text after the document");
        }
        return parsed;
    }

private:
    constexpr std::unexpected<parse_error> fail(std::string message) const {
        return std::unexpected{parse_error{.offset = at_, .message = std::move(message)}};
    }

    constexpr bool done() const noexcept { return at_ >= text_.size(); }

    constexpr void skip_filler() {
        for (;;) {
            while (!done() && std::string_view{" \t\r\n"}.contains(text_[at_])) {
                ++at_;
            }
            if (!options_.jsonc || at_ + 1 >= text_.size() || text_[at_] != '/') {
                return;
            }
            if (text_[at_ + 1] == '/') {
                at_ = std::min(text_.find('\n', at_), text_.size());
            } else if (text_[at_ + 1] == '*') {
                auto const end = text_.find("*/", at_ + 2);
                at_ = end == std::string_view::npos ? text_.size() : end + 2;
            } else {
                return;
            }
        }
    }

    constexpr std::expected<value, parse_error> parse_value(std::size_t const depth) {
        skip_filler();
        if (done()) {
            return fail("unexpected end of input");
        }
        if (depth > options_.max_depth) {
            return fail("nesting is too deep");
        }
        switch (text_[at_]) {
            case '{': return parse_object(depth);
            case '[': return parse_array(depth);
            case '"': return parse_string_value();
            default: return parse_scalar();
        }
    }

    constexpr std::expected<value, parse_error> parse_scalar() {
        auto const start = at_;
        while (!done() && !std::string_view{" \t\r\n,]}/"}.contains(text_[at_])) {
            ++at_;
        }
        auto const token = text_.substr(start, at_ - start);
        if (token == "true" || token == "false") {
            return value{token == "true"};
        }
        if (token == "null") {
            return value{nullptr};
        }
        std::int64_t whole = 0;
        auto const [whole_end, whole_error] = std::from_chars(token.data(), token.data() + token.size(), whole);
        if (!token.empty() && whole_error == std::errc{} && whole_end == token.data() + token.size()) {
            return value{whole};
        }
        if (!std::is_constant_evaluated()) {
            double real = 0;
            auto const [real_end, real_error] = std::from_chars(token.data(), token.data() + token.size(), real);
            if (!token.empty() && real_error == std::errc{} && real_end == token.data() + token.size()) {
                return value{real};
            }
        }
        at_ = start;
        return fail("not a valid value");
    }

    constexpr std::expected<std::string, parse_error> parse_string() {
        ++at_;
        std::string out;
        while (!done() && text_[at_] != '"') {
            if (static_cast<unsigned char>(text_[at_]) < 0x20) {
                return fail("control character in a string");
            }
            if (text_[at_] != '\\') {
                out.push_back(text_[at_++]);
                continue;
            }
            auto const start = at_;
            at_ += 2;
            auto const end = std::min(at_ + (start + 1 < text_.size() && text_[start + 1] == 'u' ? 4U : 0U), text_.size());
            out += jsonscan_decode(text_.substr(start, end - start));
            at_ = end;
        }
        if (done()) {
            return fail("unterminated string");
        }
        ++at_;
        return out;
    }

    static constexpr std::string jsonscan_decode(std::string_view const escape) {
        if (escape.size() < 2) {
            return {};
        }
        switch (escape[1]) {
            case 'n': return "\n";
            case 't': return "\t";
            case 'r': return "\r";
            case 'b': return "\b";
            case 'f': return "\f";
            case 'u': return escape.size() == 6 ? decode_unicode(escape.substr(2)) : std::string{};
            default: return std::string{escape[1]};
        }
    }

    static constexpr std::string decode_unicode(std::string_view const digits) {
        unsigned code = 0;
        std::from_chars(digits.data(), digits.data() + digits.size(), code, 16);
        std::string out;
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        return out;
    }

    constexpr std::expected<value, parse_error> parse_string_value() {
        auto text = parse_string();
        if (!text) {
            return std::unexpected{text.error()};
        }
        return value{std::move(*text)};
    }

    constexpr std::expected<value, parse_error> parse_array(std::size_t const depth) {
        ++at_;
        array items;
        for (;;) {
            skip_filler();
            if (!done() && text_[at_] == ']') {
                ++at_;
                return value{std::move(items)};
            }
            auto item = parse_value(depth + 1);
            if (!item) {
                return item;
            }
            items.push_back(std::move(*item));
            skip_filler();
            if (!done() && text_[at_] == ',') {
                ++at_;
            } else if (done() || text_[at_] != ']') {
                return fail("expected ',' or ']'");
            }
        }
    }

    constexpr std::expected<value, parse_error> parse_object(std::size_t const depth) {
        ++at_;
        object members;
        for (;;) {
            skip_filler();
            if (!done() && text_[at_] == '}') {
                ++at_;
                return value{std::move(members)};
            }
            if (done() || text_[at_] != '"') {
                return fail("expected a string key");
            }
            auto key = parse_string();
            if (!key) {
                return std::unexpected{key.error()};
            }
            skip_filler();
            if (done() || text_[at_] != ':') {
                return fail("expected ':'");
            }
            ++at_;
            auto item = parse_value(depth + 1);
            if (!item) {
                return item;
            }
            members.emplace_back(std::move(*key), std::move(*item));
            skip_filler();
            if (!done() && text_[at_] == ',') {
                ++at_;
            } else if (done() || text_[at_] != '}') {
                return fail("expected ',' or '}'");
            }
        }
    }

    std::string_view text_;
    parse_options options_;
    std::size_t at_ = 0;
};

template <class T>
using result = std::expected<T, read_error>;

constexpr auto duration_units = std::to_array<std::pair<std::string_view, std::chrono::milliseconds>>({
    {"ms", std::chrono::milliseconds{1}},
    {"s", std::chrono::seconds{1}},
    {"m", std::chrono::minutes{1}},
    {"h", std::chrono::hours{1}},
});

constexpr result<std::chrono::milliseconds> read_duration(std::string_view const text, std::string const& path) {
    auto const split = text.find_first_not_of("0123456789");
    std::int64_t amount = 0;
    if (split == 0 || split == std::string_view::npos ||
        std::from_chars(text.data(), text.data() + split, amount).ec != std::errc{}) {
        return std::unexpected{read_error{path, "expected a duration such as \"500ms\", \"90s\" or \"10m\""}};
    }
    auto const unit = std::ranges::find(duration_units, text.substr(split), &std::pair<std::string_view, std::chrono::milliseconds>::first);
    if (unit == duration_units.end()) {
        return std::unexpected{read_error{path, "unknown duration unit"}};
    }
    return unit->second * amount;
}

template <class T>
consteval bool member_is_required(std::meta::info const member) {
    for (auto const annotation : std::meta::annotations_of(member)) {
        if (std::meta::remove_cvref(std::meta::type_of(annotation)) == ^^required_t) {
            return true;
        }
    }
    return false;
}

constexpr std::string element_path(std::string const& path, std::size_t const index) {
    std::array<char, 24> digits{};
    auto const end = std::to_chars(digits.data(), digits.data() + digits.size(), index).ptr;
    return path + "[" + std::string{digits.data(), end} + "]";
}

template <class T>
constexpr result<T> read_value(value const& source, std::string const& path);

template <class T>
constexpr result<T> read_aggregate(value const& source, std::string const& path) {
    auto const* members = std::get_if<object>(&source.data);
    if (members == nullptr) {
        return std::unexpected{read_error{path, "expected an object"}};
    }
    static constexpr auto fields = std::define_static_array(
        std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()));
    static constexpr auto names = std::define_static_array(
        fields | std::views::transform([](std::meta::info const field) { return std::define_static_string(std::meta::identifier_of(field)); }));
    for (auto const& [key, ignored] : *members) {
        if (!std::ranges::any_of(names, [&key](char const* const name) { return std::string_view{name} == key; })) {
            return std::unexpected{read_error{path.empty() ? key : path + "." + key, "unknown key"}};
        }
    }
    T result{};
    std::optional<read_error> failure;
    template for (constexpr auto field : fields) {
        constexpr auto key = std::meta::identifier_of(field);
        auto const found = std::ranges::find_if(*members, [](auto const& entry) { return entry.first == key; });
        auto const child_path = path.empty() ? std::string{key} : path + "." + std::string{key};
        if (!failure) {
            if (found == members->end()) {
                if constexpr (member_is_required<T>(field)) {
                    failure = read_error{child_path, "required"};
                }
            } else if (auto parsed = read_value<typename [:std::meta::type_of(field):]>(found->second, child_path)) {
                result.[:field:] = std::move(*parsed);
            } else {
                failure = std::move(parsed.error());
            }
        }
    }
    if (failure) {
        return std::unexpected{std::move(*failure)};
    }
    return result;
}

template <class T>
struct is_optional : std::false_type {};
template <class U>
struct is_optional<std::optional<U>> : std::true_type {};
template <class T>
struct is_vector : std::false_type {};
template <class U>
struct is_vector<std::vector<U>> : std::true_type {};
template <class T>
struct is_string_map : std::false_type {};
template <class U>
struct is_string_map<std::map<std::string, U>> : std::true_type {};

template <class T>
constexpr result<T> read_value(value const& source, std::string const& path) {
    if constexpr (std::is_same_v<T, bool>) {
        if (auto const* flag = std::get_if<bool>(&source.data)) {
            return *flag;
        }
        return std::unexpected{read_error{path, "expected true or false"}};
    } else if constexpr (std::is_integral_v<T>) {
        if (auto const* whole = std::get_if<std::int64_t>(&source.data)) {
            if (std::in_range<T>(*whole)) {
                return static_cast<T>(*whole);
            }
            return std::unexpected{read_error{path, "number is out of range"}};
        }
        return std::unexpected{read_error{path, "expected a whole number"}};
    } else if constexpr (std::is_floating_point_v<T>) {
        if (auto const* whole = std::get_if<std::int64_t>(&source.data)) {
            return static_cast<T>(*whole);
        }
        if (auto const* real = std::get_if<double>(&source.data)) {
            return static_cast<T>(*real);
        }
        return std::unexpected{read_error{path, "expected a number"}};
    } else if constexpr (std::is_same_v<T, std::string>) {
        if (auto const* text = std::get_if<std::string>(&source.data)) {
            return *text;
        }
        return std::unexpected{read_error{path, "expected a string"}};
    } else if constexpr (std::is_same_v<T, std::chrono::milliseconds>) {
        if (auto const* text = std::get_if<std::string>(&source.data)) {
            return read_duration(*text, path);
        }
        return std::unexpected{read_error{path, "expected a duration string"}};
    } else if constexpr (std::is_enum_v<T>) {
        if (auto const* text = std::get_if<std::string>(&source.data)) {
            if (auto const named = reflect::enum_from_name<T>(*text)) {
                return *named;
            }
        }
        return std::unexpected{read_error{path, "not one of the allowed names"}};
    } else if constexpr (is_optional<T>::value) {
        if (std::holds_alternative<std::nullptr_t>(source.data)) {
            return T{};
        }
        auto inner = read_value<typename T::value_type>(source, path);
        if (!inner) {
            return std::unexpected{std::move(inner.error())};
        }
        return T{std::move(*inner)};
    } else if constexpr (is_vector<T>::value) {
        auto const* items = std::get_if<array>(&source.data);
        if (items == nullptr) {
            return std::unexpected{read_error{path, "expected an array"}};
        }
        T out;
        for (auto const& [index, item] : std::views::enumerate(*items)) {
            auto element = read_value<typename T::value_type>(item, element_path(path, static_cast<std::size_t>(index)));
            if (!element) {
                return std::unexpected{std::move(element.error())};
            }
            out.push_back(std::move(*element));
        }
        return out;
    } else if constexpr (is_string_map<T>::value) {
        auto const* members = std::get_if<object>(&source.data);
        if (members == nullptr) {
            return std::unexpected{read_error{path, "expected an object"}};
        }
        T out;
        for (auto const& [key, item] : *members) {
            auto element = read_value<typename T::mapped_type>(item, path.empty() ? key : path + "." + key);
            if (!element) {
                return std::unexpected{std::move(element.error())};
            }
            out.insert_or_assign(key, std::move(*element));
        }
        return out;
    } else {
        static_assert(std::is_aggregate_v<T>, "read<T> supports aggregates and the listed leaf types");
        return read_aggregate<T>(source, path);
    }
}

}  // namespace llmgate::json::detail

export namespace llmgate::json {

constexpr std::expected<value, parse_error> parse(std::string_view const text, parse_options const options = {}) {
    return detail::parser{text, options}.run();
}

template <class T>
constexpr std::expected<T, read_error> read(value const& source) {
    return detail::read_value<T>(source, {});
}

template <class T>
constexpr std::expected<T, read_error> read_text(std::string_view const text, parse_options const options = {}) {
    auto parsed = parse(text, options);
    if (!parsed) {
        return std::unexpected{read_error{std::format("offset {}", parsed.error().offset), parsed.error().message}};
    }
    return read<T>(*parsed);
}

}  // namespace llmgate::json

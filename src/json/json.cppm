module;

#include <meta>

export module llmgate.json;

import std;
import llmgate.jsonscan;
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

struct read_options {
    bool allow_unknown_keys = false;
};

}  // namespace llmgate::json

namespace llmgate::json::detail {

constexpr auto simple_escapes = std::to_array<std::pair<char, char>>({
    {'n', '\n'},
    {'t', '\t'},
    {'r', '\r'},
    {'b', '\b'},
    {'f', '\f'},
});

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

    constexpr bool peek_is(char const expected) const noexcept { return !done() && text_[at_] == expected; }

    constexpr bool consume(char const expected) noexcept {
        if (!peek_is(expected)) {
            return false;
        }
        ++at_;
        return true;
    }

    constexpr void skip_blanks() noexcept {
        while (!done() && std::string_view{" \t\r\n"}.contains(text_[at_])) {
            ++at_;
        }
    }

    constexpr bool at_comment_start() const noexcept {
        return options_.jsonc && at_ + 1 < text_.size() && text_[at_] == '/';
    }

    constexpr bool skip_comment() noexcept {
        if (!at_comment_start()) {
            return false;
        }
        if (text_[at_ + 1] == '/') {
            at_ = std::min(text_.find('\n', at_), text_.size());
            return true;
        }
        if (text_[at_ + 1] == '*') {
            auto const end = text_.find("*/", at_ + 2);
            at_ = end == std::string_view::npos ? text_.size() : end + 2;
            return true;
        }
        return false;
    }

    constexpr void skip_filler() {
        do {
            skip_blanks();
        } while (skip_comment());
    }

    constexpr bool separated_or_closed(char const close) {
        skip_filler();
        return consume(',') || peek_is(close);
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

    constexpr std::string_view scan_token() {
        auto const start = at_;
        while (!done() && !std::string_view{" \t\r\n,]}/"}.contains(text_[at_])) {
            ++at_;
        }
        return text_.substr(start, at_ - start);
    }

    static constexpr std::optional<value> keyword_from(std::string_view const token) {
        if (token == "true") {
            return value{true};
        }
        if (token == "false") {
            return value{false};
        }
        if (token == "null") {
            return value{nullptr};
        }
        return std::nullopt;
    }

    template <class Number>
    static constexpr std::optional<Number> number_from(std::string_view const token) {
        Number number{};
        auto const [end, error] = std::from_chars(token.data(), token.data() + token.size(), number);
        if (token.empty() || error != std::errc{} || end != token.data() + token.size()) {
            return std::nullopt;
        }
        return number;
    }

    static constexpr std::optional<double> real_from(std::string_view const token) {
        return std::is_constant_evaluated() ? std::optional<double>{} : number_from<double>(token);
    }

    static constexpr std::optional<value> scalar_from(std::string_view const token) {
        if (auto keyword = keyword_from(token)) {
            return keyword;
        }
        if (auto const whole = number_from<std::int64_t>(token)) {
            return value{*whole};
        }
        if (auto const real = real_from(token)) {
            return value{*real};
        }
        return std::nullopt;
    }

    constexpr std::expected<value, parse_error> parse_scalar() {
        auto const start = at_;
        if (auto parsed = scalar_from(scan_token())) {
            return std::move(*parsed);
        }
        at_ = start;
        return fail("not a valid value");
    }

    constexpr void take_escape(std::string& out) {
        auto const start = at_;
        at_ += 2;
        auto const end = std::min(at_ + (start + 1 < text_.size() && text_[start + 1] == 'u' ? 4U : 0U), text_.size());
        out += jsonscan_decode(text_.substr(start, end - start));
        at_ = end;
    }

    constexpr void take_string_unit(std::string& out) {
        if (text_[at_] == '\\') {
            take_escape(out);
        } else {
            out.push_back(text_[at_++]);
        }
    }

    constexpr std::expected<std::string, parse_error> parse_string() {
        ++at_;
        std::string out;
        while (!done() && text_[at_] != '"') {
            if (static_cast<unsigned char>(text_[at_]) < 0x20) {
                return fail("control character in a string");
            }
            take_string_unit(out);
        }
        if (done()) {
            return fail("unterminated string");
        }
        ++at_;
        return out;
    }

    static constexpr char unescaped(char const escape) {
        auto const match = std::ranges::find(simple_escapes, escape, &std::pair<char, char>::first);
        return match == simple_escapes.end() ? escape : match->second;
    }

    static constexpr std::string jsonscan_decode(std::string_view const escape) {
        if (escape.size() < 2) {
            return {};
        }
        if (escape[1] == 'u') {
            return escape.size() == 6 ? decode_unicode(escape.substr(2)) : std::string{};
        }
        return std::string{unescaped(escape[1])};
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
            if (consume(']')) {
                return value{std::move(items)};
            }
            auto item = parse_value(depth + 1);
            if (!item) {
                return item;
            }
            items.push_back(std::move(*item));
            if (!separated_or_closed(']')) {
                return fail("expected ',' or ']'");
            }
        }
    }

    constexpr std::expected<std::pair<std::string, value>, parse_error> parse_member(std::size_t const depth) {
        if (!peek_is('"')) {
            return fail("expected a string key");
        }
        auto key = parse_string();
        if (!key) {
            return std::unexpected{key.error()};
        }
        skip_filler();
        if (!consume(':')) {
            return fail("expected ':'");
        }
        auto item = parse_value(depth + 1);
        if (!item) {
            return std::unexpected{item.error()};
        }
        return std::pair{std::move(*key), std::move(*item)};
    }

    constexpr std::expected<value, parse_error> parse_object(std::size_t const depth) {
        ++at_;
        object members;
        for (;;) {
            skip_filler();
            if (consume('}')) {
                return value{std::move(members)};
            }
            auto member = parse_member(depth);
            if (!member) {
                return std::unexpected{member.error()};
            }
            members.emplace_back(std::move(*member));
            if (!separated_or_closed('}')) {
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
    std::string joined = path;
    joined += '[';
    joined.append(digits.data(), end);
    joined += ']';
    return joined;
}

constexpr std::string member_path(std::string const& path, std::string_view const key) {
    std::string joined = path;
    if (!joined.empty()) {
        joined += '.';
    }
    joined += key;
    return joined;
}

template <class T>
constexpr result<T> read_value(value const& source, std::string const& path, read_options options);

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
concept boolean_target = std::same_as<T, bool>;
template <class T>
concept integer_target = std::integral<T> && !std::same_as<T, bool>;
template <class T>
concept real_target = std::floating_point<T>;
template <class T>
concept text_target = std::same_as<T, std::string>;
template <class T>
concept duration_target = std::same_as<T, std::chrono::milliseconds>;
template <class T>
concept enum_target = std::is_enum_v<T>;
template <class T>
concept optional_target = is_optional<T>::value;
template <class T>
concept vector_target = is_vector<T>::value;
template <class T>
concept map_target = is_string_map<T>::value;
template <class T>
concept listed_target = boolean_target<T> || integer_target<T> || real_target<T> || text_target<T> || duration_target<T> ||
                        enum_target<T> || optional_target<T> || vector_target<T> || map_target<T>;
template <class T>
concept aggregate_target = !listed_target<T>;

constexpr read_error expected_error(std::string const& path, std::string message) {
    return read_error{path, std::move(message)};
}

template <class Alternative>
constexpr Alternative const* alternative_of(value const& source) noexcept {
    return std::get_if<Alternative>(&source.data);
}

template <boolean_target T>
constexpr result<T> read_as(value const& source, std::string const& path, read_options) {
    if (auto const* flag = alternative_of<bool>(source)) {
        return *flag;
    }
    return std::unexpected{expected_error(path, "expected true or false")};
}

template <integer_target T>
constexpr result<T> read_as(value const& source, std::string const& path, read_options) {
    auto const* whole = alternative_of<std::int64_t>(source);
    if (whole == nullptr) {
        return std::unexpected{expected_error(path, "expected a whole number")};
    }
    if (!std::in_range<T>(*whole)) {
        return std::unexpected{expected_error(path, "number is out of range")};
    }
    return static_cast<T>(*whole);
}

template <real_target T>
constexpr result<T> read_as(value const& source, std::string const& path, read_options) {
    if (auto const* whole = alternative_of<std::int64_t>(source)) {
        return static_cast<T>(*whole);
    }
    if (auto const* real = alternative_of<double>(source)) {
        return static_cast<T>(*real);
    }
    return std::unexpected{expected_error(path, "expected a number")};
}

template <text_target T>
constexpr result<T> read_as(value const& source, std::string const& path, read_options) {
    if (auto const* text = alternative_of<std::string>(source)) {
        return *text;
    }
    return std::unexpected{expected_error(path, "expected a string")};
}

template <duration_target T>
constexpr result<T> read_as(value const& source, std::string const& path, read_options) {
    if (auto const* text = alternative_of<std::string>(source)) {
        return read_duration(*text, path);
    }
    return std::unexpected{expected_error(path, "expected a duration string")};
}

template <enum_target T>
constexpr result<T> read_as(value const& source, std::string const& path, read_options) {
    if (auto const* text = alternative_of<std::string>(source)) {
        if (auto const named = reflect::enum_from_name<T>(*text)) {
            return *named;
        }
    }
    return std::unexpected{expected_error(path, "not one of the allowed names")};
}

template <optional_target T>
constexpr result<T> read_as(value const& source, std::string const& path, read_options const options) {
    if (std::holds_alternative<std::nullptr_t>(source.data)) {
        return T{};
    }
    auto inner = read_value<typename T::value_type>(source, path, options);
    if (!inner) {
        return std::unexpected{std::move(inner.error())};
    }
    return T{std::move(*inner)};
}

template <vector_target T>
constexpr result<T> read_as(value const& source, std::string const& path, read_options const options) {
    auto const* items = alternative_of<array>(source);
    if (items == nullptr) {
        return std::unexpected{expected_error(path, "expected an array")};
    }
    T out;
    for (auto const& [index, item] : std::views::enumerate(*items)) {
        auto element = read_value<typename T::value_type>(item, element_path(path, static_cast<std::size_t>(index)), options);
        if (!element) {
            return std::unexpected{std::move(element.error())};
        }
        out.push_back(std::move(*element));
    }
    return out;
}

template <map_target T>
constexpr result<T> read_as(value const& source, std::string const& path, read_options const options) {
    auto const* members = alternative_of<object>(source);
    if (members == nullptr) {
        return std::unexpected{expected_error(path, "expected an object")};
    }
    T out;
    for (auto const& [key, item] : *members) {
        auto element = read_value<typename T::mapped_type>(item, member_path(path, key), options);
        if (!element) {
            return std::unexpected{std::move(element.error())};
        }
        out.insert_or_assign(key, std::move(*element));
    }
    return out;
}

template <class T>
constexpr std::optional<read_error> unknown_key_in(object const& members, std::string const& path, read_options const options) {
    static constexpr auto fields = std::define_static_array(
        std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()));
    static constexpr auto names = std::define_static_array(
        fields | std::views::transform([](std::meta::info const field) { return std::define_static_string(std::meta::identifier_of(field)); }));
    if (options.allow_unknown_keys) {
        return std::nullopt;
    }
    for (auto const& [key, ignored] : members) {
        if (!std::ranges::any_of(names, [&key](char const* const name) { return std::string_view{name} == key; })) {
            return read_error{member_path(path, key), "unknown key"};
        }
    }
    return std::nullopt;
}

template <class T, std::meta::info field>
constexpr std::optional<read_error> read_field(T& target, object const& members, std::string const& path, read_options const options) {
    constexpr auto key = std::meta::identifier_of(field);
    auto const found = std::ranges::find_if(members, [](auto const& entry) { return entry.first == key; });
    auto const child_path = member_path(path, key);
    if (found == members.end()) {
        if constexpr (member_is_required<T>(field)) {
            return read_error{child_path, "required"};
        }
        return std::nullopt;
    }
    auto parsed = read_value<typename [:std::meta::type_of(field):]>(found->second, child_path, options);
    if (!parsed) {
        return std::move(parsed.error());
    }
    target.[:field:] = std::move(*parsed);
    return std::nullopt;
}

template <aggregate_target T>
constexpr result<T> read_as(value const& source, std::string const& path, read_options const options) {
    static_assert(std::is_aggregate_v<T>, "read<T> supports aggregates and the listed leaf types");
    auto const* members = alternative_of<object>(source);
    if (members == nullptr) {
        return std::unexpected{expected_error(path, "expected an object")};
    }
    if (auto unknown = unknown_key_in<T>(*members, path, options)) {
        return std::unexpected{std::move(*unknown)};
    }
    static constexpr auto fields = std::define_static_array(
        std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()));
    T target{};
    std::optional<read_error> failure;
    template for (constexpr auto field : fields) {
        if (!failure) {
            failure = read_field<T, field>(target, *members, path, options);
        }
    }
    if (failure) {
        return std::unexpected{std::move(*failure)};
    }
    return target;
}

template <class T>
constexpr result<T> read_value(value const& source, std::string const& path, read_options const options) {
    return read_as<T>(source, path, options);
}

}  // namespace llmgate::json::detail

export namespace llmgate::json {

constexpr std::expected<value, parse_error> parse(std::string_view const text, parse_options const options = {}) {
    return detail::parser{text, options}.run();
}

template <class T>
constexpr std::expected<T, read_error> read(value const& source, read_options const options = {}) {
    return detail::read_value<T>(source, {}, options);
}

template <class T>
constexpr std::expected<T, read_error> read_text(std::string_view const text, parse_options const options = {},
                                                 read_options const reading = {}) {
    auto parsed = parse(text, options);
    if (!parsed) {
        return std::unexpected{read_error{std::format("offset {}", parsed.error().offset), parsed.error().message}};
    }
    return read<T>(*parsed, reading);
}

}  // namespace llmgate::json

namespace llmgate::json::detail {

constexpr void write_into(std::string& out, value const& source, std::size_t const indent, std::size_t const depth);

constexpr void break_line(std::string& out, std::size_t const indent, std::size_t const depth) {
    if (indent == 0) {
        return;
    }
    out.push_back('\n');
    out.append(indent * depth, ' ');
}

constexpr void write_number(std::string& out, double const number) {
    if (!std::is_constant_evaluated()) {
        std::array<char, 32> digits{};
        out.append(digits.data(), std::to_chars(digits.data(), digits.data() + digits.size(), number).ptr);
    } else {
        out += "0";
    }
}

constexpr void write_array(std::string& out, array const& items, std::size_t const indent, std::size_t const depth) {
    if (items.empty()) {
        out += "[]";
        return;
    }
    out.push_back('[');
    for (std::size_t at = 0; at < items.size(); ++at) {
        out += at == 0 ? "" : ",";
        break_line(out, indent, depth + 1);
        write_into(out, items[at], indent, depth + 1);
    }
    break_line(out, indent, depth);
    out.push_back(']');
}

constexpr void write_object(std::string& out, object const& members, std::size_t const indent, std::size_t const depth) {
    if (members.empty()) {
        out += "{}";
        return;
    }
    out.push_back('{');
    for (std::size_t at = 0; at < members.size(); ++at) {
        out += at == 0 ? "" : ",";
        break_line(out, indent, depth + 1);
        out += jsonscan::encode_string(members[at].first);
        out += indent == 0 ? ":" : ": ";
        write_into(out, members[at].second, indent, depth + 1);
    }
    break_line(out, indent, depth);
    out.push_back('}');
}

struct writer {
    std::string& out;
    std::size_t indent;
    std::size_t depth;

    constexpr void operator()(array const& items) const { write_array(out, items, indent, depth); }
    constexpr void operator()(object const& members) const { write_object(out, members, indent, depth); }
    constexpr void operator()(std::string const& text) const { out += jsonscan::encode_string(text); }
    constexpr void operator()(std::int64_t const whole) const {
        std::array<char, 24> digits{};
        out.append(digits.data(), std::to_chars(digits.data(), digits.data() + digits.size(), whole).ptr);
    }
    constexpr void operator()(double const real) const { write_number(out, real); }
    constexpr void operator()(bool const flag) const { out += flag ? "true" : "false"; }
    constexpr void operator()(std::nullptr_t) const { out += "null"; }
};

constexpr void write_into(std::string& out, value const& source, std::size_t const indent, std::size_t const depth) {
    std::visit(writer{out, indent, depth}, source.data);
}

}  // namespace llmgate::json::detail

export namespace llmgate::json {

constexpr std::string write(value const& source, std::size_t const indent = 0) {
    std::string out;
    detail::write_into(out, source, indent, 0);
    if (indent != 0) {
        out.push_back('\n');
    }
    return out;
}

constexpr value const* at(value const& root, std::initializer_list<std::string_view> const path) {
    value const* current = &root;
    for (auto const key : path) {
        auto const* members = std::get_if<object>(&current->data);
        if (members == nullptr) {
            return nullptr;
        }
        auto const found = std::ranges::find(*members, key, &object::value_type::first);
        if (found == members->end()) {
            return nullptr;
        }
        current = &found->second;
    }
    return current;
}

constexpr std::string text_at(value const& root, std::initializer_list<std::string_view> const path) {
    auto const* found = at(root, path);
    auto const* text = found == nullptr ? nullptr : std::get_if<std::string>(&found->data);
    return text == nullptr ? std::string{} : *text;
}

constexpr std::optional<std::uint64_t> count_at(value const& root, std::initializer_list<std::string_view> const path) {
    auto const* found = at(root, path);
    if (found == nullptr) {
        return std::nullopt;
    }
    if (auto const* whole = std::get_if<std::int64_t>(&found->data); whole != nullptr && *whole >= 0) {
        return static_cast<std::uint64_t>(*whole);
    }
    if (auto const* real = std::get_if<double>(&found->data); real != nullptr && *real >= 0.0 && *real < 9.0e18) {
        return static_cast<std::uint64_t>(*real);
    }
    return std::nullopt;
}

constexpr value make_object(std::initializer_list<std::pair<std::string, value>> const members) {
    return value{object{members}};
}

constexpr value make_string(std::string text) {
    return value{std::move(text)};
}

}  // namespace llmgate::json

template class std::expected<llmgate::json::value, llmgate::json::parse_error>;

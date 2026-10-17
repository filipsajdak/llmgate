export module llmgate.jsonscan;

import std;
import llmgate.reflect;
import llmgate.require;

export namespace llmgate::jsonscan {

enum class errc { not_an_object = 1, malformed, too_deep, trailing_bytes, member_missing, member_not_a_string };

}  // namespace llmgate::jsonscan

export namespace std {
template <>
struct is_error_code_enum<llmgate::jsonscan::errc> : true_type {};
}  // namespace std

export namespace llmgate::jsonscan {

std::error_code make_error_code(errc const value) noexcept {
    return reflect::make_code(value);
}

enum class value_kind { string, number, literal, object, array };

struct member {
    std::string_view raw_key;
    std::size_t value_begin;
    std::size_t value_end;
    value_kind kind;
};

constexpr std::size_t max_depth = 256;

}  // namespace llmgate::jsonscan

namespace llmgate::jsonscan::detail {

constexpr bool is_space(char const c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

constexpr bool is_hex(char const c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

constexpr unsigned hex_value(char const c) noexcept {
    return c <= '9' ? static_cast<unsigned>(c - '0') : static_cast<unsigned>((c | 0x20) - 'a' + 10);
}

struct utf8_form {
    char32_t last_code;
    unsigned lead;
    unsigned shift;
};

constexpr unsigned continuation_bits = 6;
constexpr unsigned continuation_mask = 0x3F;
constexpr unsigned continuation_lead = 0x80;

constexpr auto utf8_forms = std::to_array<utf8_form>({
    {.last_code = 0x7F, .lead = 0, .shift = 0},
    {.last_code = 0x7FF, .lead = 0xC0, .shift = continuation_bits},
    {.last_code = 0xFFFF, .lead = 0xE0, .shift = 2 * continuation_bits},
    {.last_code = std::numeric_limits<char32_t>::max(), .lead = 0xF0, .shift = 3 * continuation_bits},
});

constexpr void append_utf8(std::string& out, char32_t const code) {
    auto const form = *std::ranges::find_if(utf8_forms, [code](utf8_form const& candidate) { return code <= candidate.last_code; });
    out.push_back(static_cast<char>(form.lead | (code >> form.shift)));
    for (auto shift = form.shift; shift > 0;) {
        shift -= continuation_bits;
        out.push_back(static_cast<char>(continuation_lead | ((code >> shift) & continuation_mask)));
    }
}

class cursor {
public:
    constexpr explicit cursor(std::string_view const text) noexcept : text_{text} {}

    constexpr bool done() const noexcept { return at_ >= text_.size(); }
    constexpr char peek() const noexcept {
        require(at_ < text_.size());
        return text_[at_];
    }
    constexpr std::size_t position() const noexcept { return at_; }
    constexpr void advance() noexcept { ++at_; }

    constexpr void skip_space() noexcept {
        while (!done() && is_space(peek())) {
            advance();
        }
    }

    constexpr bool consume(char const expected) noexcept {
        if (!done() && peek() == expected) {
            advance();
            return true;
        }
        return false;
    }

    constexpr bool skip_string() noexcept {
        if (!consume('"')) {
            return false;
        }
        while (!done()) {
            auto const c = peek();
            advance();
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return false;
            }
            if (c == '\\' && !skip_escape()) {
                return false;
            }
        }
        return false;
    }

    constexpr bool skip_scalar() noexcept {
        auto const start = at_;
        while (!done() && !is_space(peek()) && peek() != ',' && peek() != '}' && peek() != ']' && peek() != ':') {
            advance();
        }
        return at_ > start;
    }

private:
    constexpr bool skip_escape() noexcept {
        if (done()) {
            return false;
        }
        auto const c = peek();
        advance();
        if (std::string_view{"\"\\/bfnrt"}.contains(c)) {
            return true;
        }
        if (c != 'u') {
            return false;
        }
        for (int digit = 0; digit < 4; ++digit) {
            if (done() || !is_hex(peek())) {
                return false;
            }
            advance();
        }
        return true;
    }

    std::string_view text_;
    std::size_t at_ = 0;
};

constexpr std::optional<errc> malformed_unless(bool const ok) noexcept {
    return ok ? std::nullopt : std::optional{errc::malformed};
}

constexpr std::expected<value_kind, errc> kind_unless_malformed(bool const ok, value_kind const kind) noexcept {
    return ok ? std::expected<value_kind, errc>{kind} : std::unexpected{errc::malformed};
}

constexpr value_kind scalar_kind(char const first) noexcept {
    return first == '-' || (first >= '0' && first <= '9') ? value_kind::number : value_kind::literal;
}

constexpr bool opens_container(char const c) noexcept {
    return std::string_view{"{["}.contains(c);
}

constexpr bool closes_container(char const c) noexcept {
    return std::string_view{"}]"}.contains(c);
}

constexpr bool is_separator(char const c) noexcept {
    return std::string_view{",:"}.contains(c);
}

constexpr std::optional<errc> open_container(cursor& in, std::vector<char>& closers, std::size_t const depth_limit) {
    if (closers.size() >= depth_limit) {
        return errc::too_deep;
    }
    closers.emplace_back(in.peek() == '{' ? '}' : ']');
    in.advance();
    return std::nullopt;
}

constexpr std::optional<errc> close_container(cursor& in, std::vector<char>& closers) {
    if (closers.empty() || closers.back() != in.peek()) {
        return errc::malformed;
    }
    closers.pop_back();
    in.advance();
    return std::nullopt;
}

constexpr std::optional<errc> skip_leaf(cursor& in) noexcept {
    if (in.peek() == '"') {
        return malformed_unless(in.skip_string());
    }
    if (is_separator(in.peek())) {
        in.advance();
        return std::nullopt;
    }
    return malformed_unless(in.skip_scalar());
}

constexpr std::optional<errc> skip_token(cursor& in, std::vector<char>& closers, std::size_t const depth_limit) {
    if (opens_container(in.peek())) {
        return open_container(in, closers, depth_limit);
    }
    if (closes_container(in.peek())) {
        return close_container(in, closers);
    }
    return skip_leaf(in);
}

constexpr std::optional<errc> skip_container(cursor& in, std::size_t const depth_limit) {
    std::vector<char> closers;
    do {
        in.skip_space();
        if (in.done()) {
            return errc::malformed;
        }
        if (auto const error = skip_token(in, closers, depth_limit)) {
            return error;
        }
    } while (!closers.empty());
    return std::nullopt;
}

constexpr std::expected<value_kind, errc> skip_value(cursor& in, std::size_t const depth_limit) {
    in.skip_space();
    if (in.done()) {
        return std::unexpected{errc::malformed};
    }
    auto const first = in.peek();
    if (first == '"') {
        return kind_unless_malformed(in.skip_string(), value_kind::string);
    }
    if (!opens_container(first)) {
        return kind_unless_malformed(in.skip_scalar(), scalar_kind(first));
    }
    if (auto const error = skip_container(in, depth_limit)) {
        return std::unexpected{*error};
    }
    return first == '{' ? value_kind::object : value_kind::array;
}

constexpr std::optional<char32_t> hex4(std::string_view const raw, std::size_t const from) noexcept {
    constexpr std::size_t digits = 4;
    constexpr unsigned bits_per_digit = 4;
    if (from + digits > raw.size()) {
        return std::nullopt;
    }
    char32_t code = 0;
    for (std::size_t digit = 0; digit < digits; ++digit) {
        if (!is_hex(raw[from + digit])) {
            return std::nullopt;
        }
        code = (code << bits_per_digit) | hex_value(raw[from + digit]);
    }
    return code;
}

constexpr char32_t high_surrogate_first = 0xD800;
constexpr char32_t high_surrogate_last = 0xDBFF;
constexpr char32_t low_surrogate_first = 0xDC00;
constexpr char32_t low_surrogate_last = 0xDFFF;
constexpr char32_t supplementary_base = 0x10000;
constexpr unsigned surrogate_bits = 10;
constexpr std::size_t unicode_escape_length = 6;
constexpr std::size_t unicode_digits = 4;

constexpr bool is_high_surrogate(char32_t const code) noexcept {
    return code >= high_surrogate_first && code <= high_surrogate_last;
}

constexpr bool is_low_surrogate(char32_t const code) noexcept {
    return code >= low_surrogate_first && code <= low_surrogate_last;
}

constexpr bool unicode_escape_at(std::string_view const raw, std::size_t const at) noexcept {
    return at + 1 < raw.size() && raw[at] == '\\' && raw[at + 1] == 'u';
}

constexpr std::optional<char32_t> low_surrogate_after(std::string_view const raw, std::size_t const last_digit) noexcept {
    if (!unicode_escape_at(raw, last_digit + 1)) {
        return std::nullopt;
    }
    auto const low = hex4(raw, last_digit + 3);
    if (low && is_low_surrogate(*low)) {
        return low;
    }
    return std::nullopt;
}

constexpr char32_t combine_surrogates(char32_t const high, char32_t const low) noexcept {
    return supplementary_base + ((high - high_surrogate_first) << surrogate_bits) + (low - low_surrogate_first);
}

constexpr std::size_t decode_unicode(std::string& out, std::string_view const raw, std::size_t at) {
    auto code = hex4(raw, at + 1);
    if (!code) {
        out.push_back('u');
        return at;
    }
    at += unicode_digits;
    if (is_high_surrogate(*code)) {
        if (auto const low = low_surrogate_after(raw, at)) {
            code = combine_surrogates(*code, *low);
            at += unicode_escape_length;
        }
    }
    append_utf8(out, *code);
    return at;
}

constexpr auto simple_escapes = std::to_array<std::pair<char, char>>({
    {'b', '\b'},
    {'f', '\f'},
    {'n', '\n'},
    {'r', '\r'},
    {'t', '\t'},
});

constexpr char unescaped(char const escape) noexcept {
    auto const match = std::ranges::find(simple_escapes, escape, &std::pair<char, char>::first);
    return match == simple_escapes.end() ? escape : match->second;
}

constexpr std::size_t decode_escape(std::string& out, std::string_view const raw, std::size_t const backslash) {
    auto const escape = raw[backslash + 1];
    if (escape == 'u') {
        return decode_unicode(out, raw, backslash + 1);
    }
    out.push_back(unescaped(escape));
    return backslash + 1;
}

constexpr std::expected<member, errc> read_member(cursor& in, std::string_view const text) {
    in.skip_space();
    auto const key_begin = in.position();
    if (!in.skip_string()) {
        return std::unexpected{errc::malformed};
    }
    auto const raw_key = text.substr(key_begin + 1, in.position() - key_begin - 2);
    in.skip_space();
    if (!in.consume(':')) {
        return std::unexpected{errc::malformed};
    }
    in.skip_space();
    auto const value_begin = in.position();
    auto const kind = skip_value(in, max_depth);
    if (!kind) {
        return std::unexpected{kind.error()};
    }
    return member{.raw_key = raw_key, .value_begin = value_begin, .value_end = in.position(), .kind = *kind};
}

constexpr std::expected<std::vector<member>, errc> read_members(cursor& in, std::string_view const text) {
    std::vector<member> members;
    do {
        auto item = read_member(in, text);
        if (!item) {
            return std::unexpected{item.error()};
        }
        members.push_back(*item);
        in.skip_space();
    } while (in.consume(','));
    if (!in.consume('}')) {
        return std::unexpected{errc::malformed};
    }
    return members;
}

constexpr std::expected<std::vector<member>, errc> end_of_object(cursor& in, std::vector<member> members) {
    in.skip_space();
    return in.done() ? std::expected<std::vector<member>, errc>{std::move(members)} : std::unexpected{errc::trailing_bytes};
}

constexpr void append_control_escape(std::string& out, char const c) {
    constexpr std::string_view digits = "0123456789abcdef";
    constexpr unsigned nibble_bits = 4;
    constexpr unsigned nibble_mask = 0xF;
    auto const code = static_cast<unsigned>(static_cast<unsigned char>(c));
    out += "\\u00";
    out.push_back(digits[code >> nibble_bits]);
    out.push_back(digits[code & nibble_mask]);
}

constexpr auto short_escapes = std::to_array<std::pair<char, std::string_view>>({
    {'"', "\\\""},
    {'\\', "\\\\"},
    {'\n', "\\n"},
    {'\r', "\\r"},
    {'\t', "\\t"},
});

constexpr unsigned char first_printable = 0x20;

constexpr void append_encoded(std::string& out, char const c) {
    auto const match = std::ranges::find(short_escapes, c, &std::pair<char, std::string_view>::first);
    if (match != short_escapes.end()) {
        out += match->second;
    } else if (static_cast<unsigned char>(c) < first_printable) {
        append_control_escape(out, c);
    } else {
        out.push_back(c);
    }
}
}  // namespace llmgate::jsonscan::detail

export namespace llmgate::jsonscan {

constexpr std::string decode_string(std::string_view const raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t at = 0; at < raw.size(); ++at) {
        if (raw[at] != '\\' || at + 1 >= raw.size()) {
            out.push_back(raw[at]);
        } else {
            at = detail::decode_escape(out, raw, at);
        }
    }
    return out;
}

constexpr std::expected<std::vector<member>, errc> top_level_members(std::string_view const text) {
    detail::cursor in{text};
    in.skip_space();
    if (!in.consume('{')) {
        return std::unexpected{errc::not_an_object};
    }
    in.skip_space();
    if (in.consume('}')) {
        return detail::end_of_object(in, {});
    }
    auto members = detail::read_members(in, text);
    if (!members) {
        return members;
    }
    return detail::end_of_object(in, std::move(*members));
}

constexpr std::vector<member> members_named(std::span<member const> const members, std::string_view const key) {
    std::vector<member> matches;
    for (auto const& item : members) {
        auto const plain = item.raw_key.find('\\') == std::string_view::npos;
        if (plain ? item.raw_key == key : decode_string(item.raw_key) == key) {
            matches.push_back(item);
        }
    }
    return matches;
}

constexpr std::string encode_string(std::string_view const value) {
    std::string out{'"'};
    for (auto const c : value) {
        detail::append_encoded(out, c);
    }
    out.push_back('"');
    return out;
}

constexpr std::expected<std::string, errc> replace_string_member(std::string_view const text, std::string_view const key,
                                                       std::string_view const replacement) {
    auto const members = top_level_members(text);
    if (!members) {
        return std::unexpected{members.error()};
    }
    auto const matches = members_named(*members, key);
    if (matches.empty()) {
        return std::unexpected{errc::member_missing};
    }
    if (!std::ranges::all_of(matches, [](member const& item) { return item.kind == value_kind::string; })) {
        return std::unexpected{errc::member_not_a_string};
    }
    auto const encoded = encode_string(replacement);
    std::string out;
    out.reserve(text.size() + encoded.size());
    std::size_t copied = 0;
    for (auto const& item : matches) {
        out.append(text.substr(copied, item.value_begin - copied));
        out += encoded;
        copied = item.value_end;
    }
    out.append(text.substr(copied));
    return out;
}

}  // namespace llmgate::jsonscan

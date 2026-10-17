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

constexpr void append_utf8(std::string& out, char32_t const code) {
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
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

constexpr std::expected<value_kind, errc> skip_value(cursor& in, std::size_t const depth_limit) {
    in.skip_space();
    if (in.done()) {
        return std::unexpected{errc::malformed};
    }
    auto const first = in.peek();
    if (first == '"') {
        return in.skip_string() ? std::expected<value_kind, errc>{value_kind::string} : std::unexpected{errc::malformed};
    }
    if (first != '{' && first != '[') {
        return in.skip_scalar() ? std::expected<value_kind, errc>{(first == '-' || (first >= '0' && first <= '9')) ? value_kind::number : value_kind::literal}
                                : std::unexpected{errc::malformed};
    }
    std::vector<char> closers;
    do {
        in.skip_space();
        if (in.done()) {
            return std::unexpected{errc::malformed};
        }
        auto const c = in.peek();
        if (c == '{' || c == '[') {
            if (closers.size() >= depth_limit) {
                return std::unexpected{errc::too_deep};
            }
            closers.push_back(c == '{' ? '}' : ']');
            in.advance();
        } else if (c == '}' || c == ']') {
            if (closers.empty() || closers.back() != c) {
                return std::unexpected{errc::malformed};
            }
            closers.pop_back();
            in.advance();
        } else if (c == '"') {
            if (!in.skip_string()) {
                return std::unexpected{errc::malformed};
            }
        } else if (c == ',' || c == ':') {
            in.advance();
        } else if (!in.skip_scalar()) {
            return std::unexpected{errc::malformed};
        }
    } while (!closers.empty());
    return first == '{' ? value_kind::object : value_kind::array;
}

}  // namespace llmgate::jsonscan::detail

export namespace llmgate::jsonscan {

constexpr std::string decode_string(std::string_view const raw) {
    std::string out;
    out.reserve(raw.size());
    auto const hex4 = [&raw](std::size_t const from) -> std::optional<char32_t> {
        if (from + 4 > raw.size()) {
            return std::nullopt;
        }
        char32_t code = 0;
        for (std::size_t digit = 0; digit < 4; ++digit) {
            if (!detail::is_hex(raw[from + digit])) {
                return std::nullopt;
            }
            code = (code << 4) | detail::hex_value(raw[from + digit]);
        }
        return code;
    };
    for (std::size_t at = 0; at < raw.size(); ++at) {
        if (raw[at] != '\\' || at + 1 >= raw.size()) {
            out.push_back(raw[at]);
            continue;
        }
        auto const escape = raw[++at];
        switch (escape) {
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                auto code = hex4(at + 1);
                if (!code) {
                    out.push_back('u');
                    break;
                }
                at += 4;
                if (*code >= 0xD800 && *code <= 0xDBFF && at + 2 < raw.size() && raw[at + 1] == '\\' && raw[at + 2] == 'u') {
                    auto const low = hex4(at + 3);
                    if (low && *low >= 0xDC00 && *low <= 0xDFFF) {
                        code = 0x10000 + ((*code - 0xD800) << 10) + (*low - 0xDC00);
                        at += 6;
                    }
                }
                detail::append_utf8(out, *code);
                break;
            }
            default: out.push_back(escape); break;
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
    std::vector<member> members;
    in.skip_space();
    if (in.consume('}')) {
        in.skip_space();
        return in.done() ? std::expected<std::vector<member>, errc>{std::move(members)} : std::unexpected{errc::trailing_bytes};
    }
    for (;;) {
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
        auto const kind = detail::skip_value(in, max_depth);
        if (!kind) {
            return std::unexpected{kind.error()};
        }
        members.push_back({.raw_key = raw_key, .value_begin = value_begin, .value_end = in.position(), .kind = *kind});
        in.skip_space();
        if (in.consume(',')) {
            continue;
        }
        if (!in.consume('}')) {
            return std::unexpected{errc::malformed};
        }
        in.skip_space();
        return in.done() ? std::expected<std::vector<member>, errc>{std::move(members)} : std::unexpected{errc::trailing_bytes};
    }
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
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    constexpr std::string_view digits = "0123456789abcdef";
                    auto const code = static_cast<unsigned>(static_cast<unsigned char>(c));
                    out += "\\u00";
                    out.push_back(digits[code >> 4]);
                    out.push_back(digits[code & 0xF]);
                } else {
                    out.push_back(c);
                }
        }
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

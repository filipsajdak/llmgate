export module llmgate.eventstream;

import std;
import llmgate.json;
import llmgate.reflect;

export namespace llmgate::eventstream {

constexpr std::size_t prelude_bytes = 12;
constexpr std::size_t trailer_bytes = 4;
constexpr std::size_t longest_frame = 16 * 1024 * 1024;
constexpr std::size_t longest_headers = 128 * 1024;

enum class failure { none, prelude_crc, message_crc, too_large, bad_headers };
static_assert(reflect::unique_values<failure>());

struct frame {
    std::string message_type{};
    std::string event_type{};
    std::string exception_type{};
    std::string error_code{};
    std::string error_message{};
    std::string payload{};
};

struct produced {
    std::string name{};
    std::string data{};
};

}  // namespace llmgate::eventstream

namespace llmgate::eventstream::detail {

constexpr std::uint32_t crc_polynomial = 0xEDB88320U;
constexpr std::uint32_t crc_start = 0xFFFFFFFFU;
constexpr unsigned byte_values = 256;
constexpr unsigned byte_bits = 8;
constexpr std::uint8_t header_string = 7;
constexpr std::array<std::size_t, 10> fixed_value_sizes{0, 0, 1, 2, 4, 8, 0, 0, 8, 16};
constexpr std::uint8_t header_bytes_type = 6;
constexpr std::string_view base64_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

constexpr std::array<std::uint32_t, byte_values> crc_table = [] {
    std::array<std::uint32_t, byte_values> table{};
    for (std::uint32_t index = 0; index < byte_values; ++index) {
        auto value = index;
        for (unsigned bit = 0; bit < byte_bits; ++bit) {
            value = (value & 1U) != 0 ? (value >> 1) ^ crc_polynomial : value >> 1;
        }
        table[index] = value;
    }
    return table;
}();

constexpr std::uint32_t crc32_of(std::string_view const bytes) {
    auto crc = crc_start;
    for (char const c : bytes) {
        crc = crc_table[(crc ^ static_cast<unsigned char>(c)) & 0xFFU] ^ (crc >> byte_bits);
    }
    return ~crc;
}

constexpr std::uint32_t read32(std::string_view const bytes, std::size_t const at) {
    std::uint32_t value = 0;
    for (std::size_t offset = 0; offset < 4; ++offset) {
        value = value << byte_bits | static_cast<unsigned char>(bytes[at + offset]);
    }
    return value;
}

constexpr std::uint16_t read16(std::string_view const bytes, std::size_t const at) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[at]) << byte_bits | static_cast<unsigned char>(bytes[at + 1]));
}

constexpr void put32(std::string& out, std::uint32_t const value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xFFU));
    }
}

constexpr void put_string_header(std::string& out, std::string_view const name, std::string_view const value) {
    out.push_back(static_cast<char>(name.size()));
    out += name;
    out.push_back(static_cast<char>(header_string));
    out.push_back(static_cast<char>(value.size() >> byte_bits));
    out.push_back(static_cast<char>(value.size() & 0xFFU));
    out += value;
}

struct header_view {
    std::string_view name{};
    std::string_view value{};
    std::size_t next = 0;
    bool ok = false;
};

struct value_span {
    std::size_t start = 0;
    std::size_t size = 0;
    bool ok = false;
};

constexpr value_span value_span_at(std::string_view const headers, std::size_t at, unsigned char const type) {
    if (type >= fixed_value_sizes.size()) {
        return {};
    }
    std::size_t size = fixed_value_sizes[type];
    if (type == header_string || type == header_bytes_type) {
        if (at + 2 > headers.size()) {
            return {};
        }
        size = read16(headers, at);
        at += 2;
    }
    return {.start = at, .size = size, .ok = at + size <= headers.size()};
}

constexpr header_view header_at(std::string_view const headers, std::size_t at) {
    if (at >= headers.size()) {
        return {};
    }
    auto const name_size = static_cast<unsigned char>(headers[at]);
    if (at + 2 + name_size > headers.size()) {
        return {};
    }
    header_view out{.name = headers.substr(at + 1, name_size)};
    at += 1 + name_size;
    auto const type = static_cast<unsigned char>(headers[at++]);
    auto const span = value_span_at(headers, at, type);
    if (!span.ok) {
        return {};
    }
    out.value = type == header_string ? headers.substr(span.start, span.size) : std::string_view{};
    out.next = span.start + span.size;
    out.ok = true;
    return out;
}

constexpr failure prelude_failure(std::string_view const window) {
    auto const total = read32(window, 0);
    auto const headers_size = read32(window, 4);
    if (crc32_of(window.substr(0, 8)) != read32(window, 8)) {
        return failure::prelude_crc;
    }
    bool const oversized = total > longest_frame || headers_size > longest_headers || prelude_bytes + std::size_t{headers_size} + trailer_bytes > total;
    return oversized ? failure::too_large : failure::none;
}

struct header_slot {
    std::string_view name;
    std::string frame::* member;
};

constexpr std::array<header_slot, 5> header_slots{{{":message-type", &frame::message_type},
                                                   {":event-type", &frame::event_type},
                                                   {":exception-type", &frame::exception_type},
                                                   {":error-code", &frame::error_code},
                                                   {":error-message", &frame::error_message}}};

constexpr void remember(frame& out, std::string_view const name, std::string_view const value) {
    if (auto const slot = std::ranges::find(header_slots, name, &header_slot::name); slot != header_slots.end()) {
        out.*(slot->member) = value;
    }
}

constexpr bool read_headers(std::string_view const headers, frame& out) {
    for (std::size_t at = 0; at < headers.size();) {
        auto const found = header_at(headers, at);
        if (!found.ok) {
            return false;
        }
        remember(out, found.name, found.value);
        at = found.next;
    }
    return true;
}

constexpr std::optional<std::string> base64_decoded(std::string_view const text) {
    std::string out;
    std::uint32_t block = 0;
    int bits = 0;
    for (char const c : text) {
        if (c == '=') {
            break;
        }
        auto const at = base64_alphabet.find(c);
        if (at == std::string_view::npos) {
            return std::nullopt;
        }
        block = block << 6 | static_cast<std::uint32_t>(at);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((block >> bits) & 0xFFU));
        }
    }
    return out;
}

constexpr auto exception_types = std::to_array<std::pair<std::string_view, std::string_view>>(
    {{"throttlingException", "rate_limit_error"}, {"serviceQuotaExceededException", "rate_limit_error"}, {"validationException", "invalid_request_error"}, {"accessDeniedException", "permission_error"}});

constexpr std::string_view error_type_of(std::string_view const exception) {
    auto const found = std::ranges::find(exception_types, exception, &std::pair<std::string_view, std::string_view>::first);
    return found == exception_types.end() ? "api_error" : found->second;
}

constexpr produced error_event(std::string_view const type, std::string_view const message) {
    return {.name = "error",
            .data = json::write(json::make_object({{"type", json::make_string("error")},
                                                   {"error", json::make_object({{"type", json::make_string(std::string{type})}, {"message", json::make_string(std::string{message})}})}}))};
}

constexpr produced chunk_event(frame const& each) {
    constexpr std::string_view broken = "Bedrock sent a stream chunk that is not a Messages event";
    auto const wrapper = json::parse(each.payload, {.jsonc = false});
    if (!wrapper) {
        return error_event("api_error", broken);
    }
    auto decoded = base64_decoded(json::text_at(*wrapper, {"bytes"}));
    if (!decoded) {
        return error_event("api_error", broken);
    }
    auto const inner = json::parse(*decoded, {.jsonc = false});
    if (!inner) {
        return error_event("api_error", broken);
    }
    auto name = json::text_at(*inner, {"type"});
    return {.name = name.empty() ? std::string{"message"} : std::move(name), .data = std::move(*decoded)};
}

}  // namespace llmgate::eventstream::detail

export namespace llmgate::eventstream {

constexpr std::uint32_t crc32(std::string_view const bytes) {
    return detail::crc32_of(bytes);
}

constexpr std::string encode(std::span<std::pair<std::string, std::string> const> const headers, std::string_view const payload) {
    std::string block;
    for (auto const& [name, value] : headers) {
        detail::put_string_header(block, name, value);
    }
    std::string out;
    detail::put32(out, static_cast<std::uint32_t>(prelude_bytes + block.size() + payload.size() + trailer_bytes));
    detail::put32(out, static_cast<std::uint32_t>(block.size()));
    detail::put32(out, crc32(out));
    out += block;
    out += payload;
    detail::put32(out, crc32(out));
    return out;
}

constexpr std::optional<produced> event_of(frame const& each) {
    if (each.message_type == "exception") {
        auto const parsed = json::parse(each.payload, {.jsonc = false});
        return detail::error_event(detail::error_type_of(each.exception_type), parsed ? json::text_at(*parsed, {"message"}) : std::string{each.exception_type});
    }
    if (each.message_type == "error") {
        return detail::error_event("api_error", each.error_message.empty() ? each.error_code : each.error_message);
    }
    return each.event_type == "chunk" ? std::optional{detail::chunk_event(each)} : std::nullopt;
}

class decoder {
public:
    template <class Sink>
        requires std::invocable<Sink&, frame const&>
    failure feed(std::span<std::byte const> const bytes, Sink&& sink) {
        if (failed_ != failure::none) {
            return failed_;
        }
        buffer_.append(reinterpret_cast<char const*>(bytes.data()), bytes.size());
        while (failed_ == failure::none && take(sink)) {
        }
        compact();
        return failed_;
    }

    template <class Sink>
        requires std::invocable<Sink&, frame const&>
    constexpr failure feed(std::string_view const bytes, Sink&& sink) {
        if (failed_ != failure::none) {
            return failed_;
        }
        buffer_ += bytes;
        while (failed_ == failure::none && take(sink)) {
        }
        compact();
        return failed_;
    }

    constexpr bool incomplete() const noexcept { return failed_ == failure::none && offset_ < buffer_.size(); }

private:
    constexpr void compact() {
        buffer_.erase(0, offset_);
        offset_ = 0;
    }

    template <class Sink>
        requires std::invocable<Sink&, frame const&>
    constexpr bool take(Sink& sink) {
        std::string_view const window{buffer_.data() + offset_, buffer_.size() - offset_};
        if (window.size() < prelude_bytes) {
            return false;
        }
        if (auto const broken = detail::prelude_failure(window); broken != failure::none) {
            return fail(broken);
        }
        auto const total = detail::read32(window, 0);
        auto const headers_size = detail::read32(window, 4);
        if (window.size() < total) {
            return false;
        }
        if (detail::crc32_of(window.substr(0, total - trailer_bytes)) != detail::read32(window, total - trailer_bytes)) {
            return fail(failure::message_crc);
        }
        frame each;
        if (!detail::read_headers(window.substr(prelude_bytes, headers_size), each)) {
            return fail(failure::bad_headers);
        }
        each.payload = window.substr(prelude_bytes + headers_size, total - prelude_bytes - headers_size - trailer_bytes);
        offset_ += total;
        sink(each);
        return true;
    }

    constexpr bool fail(failure const why) {
        failed_ = why;
        return false;
    }

    std::string buffer_{};
    std::size_t offset_ = 0;
    failure failed_ = failure::none;
};

}  // namespace llmgate::eventstream

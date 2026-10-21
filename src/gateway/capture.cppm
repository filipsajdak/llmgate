module;

export module llmgate.capture;

import std;
import llmgate.http;
import llmgate.json;

export namespace llmgate::capture {

constexpr std::size_t body_limit_bytes = std::size_t{2} * 1024 * 1024;
constexpr std::string_view redacted_value = "<redacted>";

struct exchange {
    std::int64_t time = 0;
    std::string session{};
    std::string provider{};
    std::string requested{};
    std::string kind{};
    std::string verdict{};
    int status = 0;
    std::map<std::string, std::string> request_headers{};
    std::string request_body{};
    bool request_truncated = false;
    std::map<std::string, std::string> reply_headers{};
    std::string reply_body{};
    bool reply_truncated = false;
};

}  // namespace llmgate::capture

namespace llmgate::capture::detail {

constexpr char lowered(char const c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

constexpr bool same_name(std::string_view const left, std::string_view const right) {
    return std::ranges::equal(left, right, [](char const a, char const b) { return lowered(a) == lowered(b); });
}

}  // namespace llmgate::capture::detail

export namespace llmgate::capture {

constexpr bool secret_header(std::string_view const name) {
    constexpr auto secret = std::to_array<std::string_view>({"authorization", "x-api-key", "proxy-authorization", "cookie", "set-cookie"});
    return std::ranges::any_of(secret, [name](std::string_view const item) { return detail::same_name(item, name); });
}

constexpr std::map<std::string, std::string> redacted_headers(http::header_map const& fields) {
    std::map<std::string, std::string> shown;
    for (auto const& field : fields.all()) {
        shown[field.name] = secret_header(field.name) ? std::string{redacted_value} : field.value;
    }
    return shown;
}

constexpr std::string bounded(std::string_view const body, std::size_t const limit, bool& truncated) {
    truncated = body.size() > limit;
    return std::string{body.substr(0, limit)};
}

constexpr std::string line_of(exchange const& seen) {
    auto line = json::write(json::to_value(seen));
    line += '\n';
    return line;
}

static_assert(secret_header("Authorization") && secret_header("X-API-KEY") && !secret_header("anthropic-beta"));

}  // namespace llmgate::capture

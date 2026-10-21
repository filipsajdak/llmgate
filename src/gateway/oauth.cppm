module;

export module llmgate.oauth;

import std;
import llmgate.json;
import llmgate.lenient;
import llmgate.proof;
import llmgate.text;

export namespace llmgate::oauth {

constexpr std::string_view default_issuer = "https://auth.openai.com";
constexpr std::string_view authorize_path = "/api/accounts/authorize";
constexpr std::string_view token_path = "/api/accounts/oauth/token";
constexpr std::string_view dynamic_client_id = "dynamic_agent_client";
constexpr std::string_view plan_scope = "chatgpt.tokens.use.direct";
constexpr std::string_view requested_scopes = "openid profile email offline_access resource.invoke chatgpt.tokens.use.direct";
constexpr std::string_view api_resource = "https://api.openai.com/v1";
constexpr std::string_view callback_path = "/auth/callback";
constexpr std::size_t random_token_bytes = 32;

constexpr std::string_view base64url_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

constexpr std::string base64url(std::span<std::byte const> const bytes) {
    std::string out;
    std::uint32_t buffer = 0;
    int held = 0;
    for (auto const byte : bytes) {
        buffer = (buffer << 8) | std::to_integer<std::uint32_t>(byte);
        held += 8;
        while (held >= 6) {
            held -= 6;
            out.push_back(base64url_alphabet[(buffer >> held) & 0x3F]);
        }
    }
    if (held > 0) {
        out.push_back(base64url_alphabet[(buffer << (6 - held)) & 0x3F]);
    }
    return out;
}

constexpr std::optional<std::vector<std::byte>> from_base64url(std::string_view const text) {
    if (text.size() % 4 == 1) {
        return std::nullopt;
    }
    std::vector<std::byte> out;
    std::uint32_t buffer = 0;
    int held = 0;
    for (auto const c : text) {
        auto const at = base64url_alphabet.find(c);
        if (at == std::string_view::npos) {
            return std::nullopt;
        }
        buffer = (buffer << 6) | static_cast<std::uint32_t>(at);
        held += 6;
        if (held >= 8) {
            held -= 8;
            out.push_back(static_cast<std::byte>((buffer >> held) & 0xFF));
        }
    }
    return out;
}

inline std::string code_challenge(std::string_view const verifier) {
    auto const digest = proof::sha256(verifier);
    return base64url(std::as_bytes(std::span{digest}));
}

inline std::expected<std::string, std::error_code> random_token() {
    return proof::random_hex(random_token_bytes);
}

constexpr std::string_view upper_hex_digits = "0123456789ABCDEF";

constexpr std::string_view unreserved_marks = ".~";

constexpr bool unreserved(char const c) {
    return base64url_alphabet.contains(c) || unreserved_marks.contains(c);
}

constexpr std::string percent_encode(std::string_view const text) {
    std::string out;
    for (auto const c : text) {
        if (unreserved(c)) {
            out.push_back(c);
        } else {
            auto const byte = static_cast<unsigned char>(c);
            out.push_back('%');
            out.push_back(upper_hex_digits[byte >> 4]);
            out.push_back(upper_hex_digits[byte & 0xF]);
        }
    }
    return out;
}

constexpr std::optional<int> hex_value(char const c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return c >= 'A' && c <= 'F' ? std::optional<int>{c - 'A' + 10} : std::nullopt;
}

constexpr std::optional<std::string> percent_decode(std::string_view const text) {
    std::string out;
    for (std::size_t at = 0; at < text.size(); ++at) {
        if (text[at] == '+') {
            out.push_back(' ');
        } else if (text[at] != '%') {
            out.push_back(text[at]);
        } else {
            auto const high = at + 2 < text.size() ? hex_value(text[at + 1]) : std::nullopt;
            auto const low = high ? hex_value(text[at + 2]) : std::nullopt;
            if (!high || !low) {
                return std::nullopt;
            }
            out.push_back(static_cast<char>(*high * 16 + *low));
            at += 2;
        }
    }
    return out;
}

struct authorization_request {
    std::string client_id{};
    std::string redirect_uri{};
    std::string state{};
    std::string nonce{};
    std::string code_challenge{};
    std::string agent_name_hint{};
    std::string host_id{};
};

constexpr std::string authorize_url(std::string_view const issuer, authorization_request const& request) {
    std::string out;
    text::append_all(out, issuer, authorize_path, "?response_type=code&client_id=", percent_encode(request.client_id), "&redirect_uri=", percent_encode(request.redirect_uri),
                     "&scope=", percent_encode(requested_scopes), "&state=", percent_encode(request.state), "&nonce=", percent_encode(request.nonce),
                     "&code_challenge=", percent_encode(request.code_challenge), "&code_challenge_method=S256&resource=", percent_encode(api_resource));
    if (!request.agent_name_hint.empty()) {
        text::append_all(out, "&agent_name_hint=", percent_encode(request.agent_name_hint));
    }
    text::append_all(out, "&ext_agent_host_id=", percent_encode(request.host_id));
    return out;
}

struct callback {
    std::string code{};
    std::string state{};
    std::string client_id{};
    std::string error{};
};

constexpr std::string query_param(std::string_view const query, std::string_view const name) {
    for (auto const piece : std::views::split(query, '&')) {
        std::string_view const pair{piece.begin(), piece.end()};
        auto const equals = pair.find('=');
        if (pair.substr(0, equals) == name && equals != std::string_view::npos) {
            return percent_decode(pair.substr(equals + 1)).value_or(std::string{});
        }
    }
    return {};
}

constexpr std::optional<callback> parse_callback(std::string_view const target) {
    auto const question = target.find('?');
    if (target.substr(0, question) != callback_path || question == std::string_view::npos) {
        return std::nullopt;
    }
    auto const query = target.substr(question + 1);
    callback out{.code = query_param(query, "code"), .state = query_param(query, "state"), .client_id = query_param(query, "client_id"), .error = query_param(query, "error")};
    if (out.code.empty() && out.error.empty()) {
        return std::nullopt;
    }
    return out;
}

constexpr std::optional<json::value> jwt_claims(std::string_view const token) {
    auto const first = token.find('.');
    auto const second = first == std::string_view::npos ? first : token.find('.', first + 1);
    if (second == std::string_view::npos) {
        return std::nullopt;
    }
    auto const payload = from_base64url(token.substr(first + 1, second - first - 1));
    if (!payload) {
        return std::nullopt;
    }
    auto parsed = json::parse(std::string_view{reinterpret_cast<char const*>(payload->data()), payload->size()}, {.jsonc = false});
    if (!parsed || std::get_if<json::object>(&parsed->data) == nullptr) {
        return std::nullopt;
    }
    return std::move(*parsed);
}

constexpr bool audience_includes(json::value const& claims, std::string_view const audience) {
    auto const* found = json::at(claims, {"aud"});
    if (found == nullptr) {
        return false;
    }
    if (auto const* one = std::get_if<std::string>(&found->data)) {
        return *one == audience;
    }
    auto const* many = std::get_if<json::array>(&found->data);
    return many != nullptr && std::ranges::any_of(*many, [audience](json::value const& item) {
        auto const* text = std::get_if<std::string>(&item.data);
        return text != nullptr && *text == audience;
    });
}

constexpr std::string id_token_problem(json::value const& claims, std::string_view const issuer, std::string_view const audience, std::string_view const nonce,
                                       std::int64_t const now) {
    if (json::text_at(claims, {"iss"}) != issuer) {
        return "the ID token has the wrong issuer";
    }
    if (!audience_includes(claims, audience)) {
        return "the ID token is not for this client (audience)";
    }
    if (json::text_at(claims, {"nonce"}) != nonce) {
        return "the ID token does not carry this sign-in's nonce";
    }
    auto const* expiry = json::at(claims, {"exp"});
    auto const* seconds = expiry == nullptr ? nullptr : std::get_if<std::int64_t>(&expiry->data);
    return seconds == nullptr || *seconds <= now ? "the ID token has expired" : std::string{};
}

constexpr std::string uuid_urn(std::array<std::byte, 16> bytes) {
    bytes[6] = static_cast<std::byte>((std::to_integer<unsigned>(bytes[6]) & 0x0F) | 0x40);
    bytes[8] = static_cast<std::byte>((std::to_integer<unsigned>(bytes[8]) & 0x3F) | 0x80);
    std::string out{"urn:uuid:"};
    for (std::size_t at = 0; at < bytes.size(); ++at) {
        auto const value = std::to_integer<unsigned>(bytes[at]);
        out.push_back(proof::hex_digits[value >> 4]);
        out.push_back(proof::hex_digits[value & 0xF]);
        if (at == 3 || at == 5 || at == 7 || at == 9) {
            out.push_back('-');
        }
    }
    return out;
}

constexpr bool refresh_token_unusable(std::string_view const error) {
    constexpr auto unusable = std::to_array<std::string_view>(
        {"invalid_grant", "invalid_refresh_token", "token_expired", "refresh_token_expired", "refresh_token_invalidated", "refresh_token_reused"});
    return std::ranges::contains(unusable, error);
}

constexpr bool grants_plan_use(std::string_view const scopes) {
    return std::ranges::any_of(std::views::split(scopes, ' '), [](auto const piece) { return std::string_view{piece.begin(), piece.end()} == plan_scope; });
}

struct token_reply {
    std::string access_token{};
    std::string refresh_token{};
    std::string id_token{};
    std::string token_type{};
    std::int64_t expires_in = 0;
    std::string scope{};
    std::int64_t earliest_refresh_at = 0;
};

constexpr std::expected<token_reply, std::string> parse_token_reply(std::string_view const body) {
    auto const parsed = json::parse(body, {.jsonc = false});
    if (!parsed) {
        return std::unexpected{std::string{"the token endpoint did not answer with JSON"}};
    }
    auto reply = lenient::read<token_reply>(*parsed).value_or(token_reply{});
    if (reply.access_token.empty()) {
        auto reason = json::text_at(*parsed, {"error"});
        return std::unexpected{reason.empty() ? std::string{"the token reply has no access token"} : std::move(reason)};
    }
    return reply;
}

}  // namespace llmgate::oauth

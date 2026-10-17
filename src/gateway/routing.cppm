export module llmgate.routing;

import std;
import llmgate.http;
import llmgate.jsonscan;

export namespace llmgate::routing {

enum class route { messages, count_tokens, models, health, statistics, unknown_path, wrong_method };

struct reply_body {
    int status;
    std::string body;
};

struct endpoint_url {
    bool secure;
    std::string host;
    std::uint16_t port;
    std::string base_path;
};

struct prepared_request {
    std::string body;
    bool stream;
};

constexpr std::uint16_t https_port = 443;
constexpr std::uint16_t http_port = 80;

constexpr std::string error_json(std::string_view const kind, std::string_view const message) {
    std::string out = R"({"type":"error","error":{"type":")";
    out += kind;
    out += R"(","message":)";
    out += jsonscan::encode_string(message);
    out += "}}";
    return out;
}

constexpr reply_body error_reply(int const status, std::string_view const kind, std::string_view const message) {
    return {.status = status, .body = error_json(kind, message)};
}

constexpr std::string path_of(std::string_view const target) {
    return std::string{target.substr(0, target.find('?'))};
}

struct route_entry {
    std::string_view method;
    std::string_view path;
    route which;
};

constexpr auto route_table = std::to_array<route_entry>({
    {"POST", "/anthropic/v1/messages", route::messages},
    {"POST", "/anthropic/v1/messages/count_tokens", route::count_tokens},
    {"GET", "/anthropic/v1/models", route::models},
    {"GET", "/healthz", route::health},
    {"GET", "/llmgate/stats", route::statistics},
});

constexpr route route_of(std::string_view const method, std::string_view const target) {
    auto const path = path_of(target);
    auto const known_path = std::ranges::find(route_table, std::string_view{path}, &route_entry::path);
    if (known_path == route_table.end()) {
        return route::unknown_path;
    }
    auto const exact = std::ranges::find_if(route_table, [&](route_entry const& item) { return item.path == path && item.method == method; });
    return exact == route_table.end() ? route::wrong_method : exact->which;
}

constexpr std::expected<endpoint_url, std::string> parse_url(std::string_view const url) {
    constexpr std::string_view https = "https://";
    constexpr std::string_view http = "http://";
    bool const secure = url.starts_with(https);
    if (!secure && !url.starts_with(http)) {
        return std::unexpected{std::string{"base_url must start with http:// or https://"}};
    }
    auto const rest = url.substr(secure ? https.size() : http.size());
    auto const slash = rest.find('/');
    auto const authority = rest.substr(0, slash);
    auto const base_path = slash == std::string_view::npos ? std::string{} : std::string{rest.substr(slash)};
    auto const colon = authority.rfind(':');
    std::uint16_t port = secure ? https_port : http_port;
    auto host = authority;
    if (colon != std::string_view::npos) {
        host = authority.substr(0, colon);
        auto const digits = authority.substr(colon + 1);
        auto const parsed = std::from_chars(digits.data(), digits.data() + digits.size(), port);
        if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()) {
            return std::unexpected{std::string{"base_url has an invalid port"}};
        }
    }
    if (host.empty()) {
        return std::unexpected{std::string{"base_url has no host"}};
    }
    return endpoint_url{.secure = secure, .host = std::string{host}, .port = port, .base_path = base_path};
}

constexpr auto hidden_headers = std::to_array<std::string_view>(
    {"connection", "keep-alive", "transfer-encoding", "content-length", "upgrade", "te", "trailer", "proxy-authenticate",
     "proxy-authorization", "host"});

constexpr char lowered(char const c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

constexpr bool hop_by_hop(std::string_view const name) {
    return std::ranges::any_of(hidden_headers, [name](std::string_view const item) {
        return std::ranges::equal(item, name, [](char const a, char const b) { return a == lowered(b); });
    });
}

constexpr http::header_map downstream_headers(http::header_map const& upstream) {
    http::header_map kept;
    for (auto const& field : upstream.all()) {
        if (!hop_by_hop(field.name)) {
            kept.add(field.name, field.value);
        }
    }
    return kept;
}

constexpr std::optional<std::string_view> agreed_value(std::string_view const body, std::span<jsonscan::member const> const matches) {
    if (matches.empty()) {
        return std::nullopt;
    }
    auto const first = body.substr(matches.front().value_begin, matches.front().value_end - matches.front().value_begin);
    auto const same = std::ranges::all_of(matches, [&](jsonscan::member const& item) {
        return body.substr(item.value_begin, item.value_end - item.value_begin) == first;
    });
    return same ? std::optional<std::string_view>{first} : std::nullopt;
}

template <class Lookup>
constexpr std::expected<prepared_request, reply_body> prepare(std::string_view const body, Lookup&& lookup) {
    auto const members = jsonscan::top_level_members(body);
    if (!members) {
        return std::unexpected{error_reply(400, "invalid_request_error", "the request body is not a JSON object")};
    }
    auto const models = jsonscan::members_named(*members, "model");
    auto const model_text = agreed_value(body, models);
    if (models.empty() || models.front().kind != jsonscan::value_kind::string) {
        return std::unexpected{error_reply(400, "invalid_request_error", "model is required and must be a string")};
    }
    if (!model_text) {
        return std::unexpected{error_reply(400, "invalid_request_error", "model appears more than once with different values")};
    }
    auto const streams = jsonscan::members_named(*members, "stream");
    auto const stream_text = agreed_value(body, streams);
    if (!streams.empty() && !stream_text) {
        return std::unexpected{error_reply(400, "invalid_request_error", "stream appears more than once with different values")};
    }
    auto text = std::string{body};
    auto const requested = jsonscan::decode_string(model_text->substr(1, model_text->size() - 2));
    if (auto const mapped = lookup(requested)) {
        auto replaced = jsonscan::replace_string_member(body, "model", *mapped);
        if (!replaced) {
            return std::unexpected{error_reply(400, "invalid_request_error", "model must be a string")};
        }
        text = std::move(*replaced);
    }
    return prepared_request{.body = std::move(text), .stream = stream_text == "true"};
}

static_assert(route_of("POST", "/anthropic/v1/messages?beta=true") == route::messages);
static_assert(route_of("GET", "/anthropic/v1/messages") == route::wrong_method);
static_assert(route_of("GET", "/nope") == route::unknown_path);
static_assert(parse_url("https://api.example.com").value().port == https_port);
static_assert(parse_url("http://127.0.0.1:9090/base").value().base_path == "/base");
static_assert(!parse_url("ftp://x").has_value());
static_assert(hop_by_hop("Transfer-Encoding") && !hop_by_hop("Content-Type"));

}  // namespace llmgate::routing

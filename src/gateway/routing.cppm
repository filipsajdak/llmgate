module;

#include <meta>

export module llmgate.routing;

import std;
import llmgate.http;
import llmgate.json;
import llmgate.jsonscan;
import llmgate.members;

export namespace llmgate::routing {

constexpr std::size_t method_capacity = 8;
constexpr std::size_t path_capacity = 48;

struct endpoint {
    char method[method_capacity]{};
    char path[path_capacity]{};

    consteval endpoint(std::string_view const verb, std::string_view const target) {
        if (verb.size() >= method_capacity || target.size() >= path_capacity) {
            throw "routing::endpoint: the method or path does not fit its capacity";
        }
        std::ranges::copy(verb, method);
        std::ranges::copy(target, path);
    }

    constexpr std::string_view verb() const noexcept { return method; }
    constexpr std::string_view target() const noexcept { return path; }
};

enum class route {
    messages [[= endpoint{"POST", "/anthropic/v1/messages"}]],
    count_tokens [[= endpoint{"POST", "/anthropic/v1/messages/count_tokens"}]],
    models [[= endpoint{"GET", "/anthropic/v1/models"}]],
    health [[= endpoint{"GET", "/healthz"}]],
    statistics [[= endpoint{"GET", "/llmgate/stats"}]],
    unknown_path,
    wrong_method,
};

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
    std::string requested{};
};

constexpr std::uint16_t https_port = 443;
constexpr std::uint16_t http_port = 80;

struct error_detail {
    std::string type{};
    std::string message{};
};

struct error_body {
    std::string type = "error";
    error_detail error{};
};

constexpr std::string error_json(std::string_view const kind, std::string_view const message) {
    return json::write(json::to_value(error_body{.error = {.type = std::string{kind}, .message = std::string{message}}}));
}

constexpr reply_body error_reply(int const status, std::string_view const kind, std::string_view const message) {
    return {.status = status, .body = error_json(kind, message)};
}

constexpr std::string_view path_of(std::string_view const target) noexcept {
    return target.substr(0, target.find('?'));
}

struct route_entry {
    endpoint where;
    route which;
};

consteval std::vector<route_entry> annotated_routes() {
    std::vector<route_entry> found;
    template for (constexpr auto enumerator : std::define_static_array(std::meta::enumerators_of(^^route))) {
        if constexpr (reflect::has_annotation<endpoint>(enumerator)) {
            found.push_back({.where = *reflect::annotation_of<endpoint>(enumerator), .which = [:enumerator:]});
        }
    }
    return found;
}

constexpr auto route_table = std::define_static_array(annotated_routes());

constexpr route route_of(std::string_view const method, std::string_view const target) {
    auto const path = path_of(target);
    auto const same_path = [&](route_entry const& item) { return item.where.target() == path; };
    if (!std::ranges::any_of(route_table, same_path)) {
        return route::unknown_path;
    }
    auto const exact = std::ranges::find_if(route_table, [&](route_entry const& item) { return same_path(item) && item.where.verb() == method; });
    return exact == route_table.end() ? route::wrong_method : exact->which;
}

struct host_port {
    std::string_view host;
    std::uint16_t port;
};

constexpr std::expected<host_port, std::string> split_authority(std::string_view const authority, bool const secure) {
    auto const colon = authority.rfind(':');
    host_port result{.host = authority, .port = secure ? https_port : http_port};
    if (colon != std::string_view::npos) {
        result.host = authority.substr(0, colon);
        auto const digits = authority.substr(colon + 1);
        auto const parsed = std::from_chars(digits.data(), digits.data() + digits.size(), result.port);
        if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()) {
            return std::unexpected{std::string{"base_url has an invalid port"}};
        }
    }
    if (result.host.empty()) {
        return std::unexpected{std::string{"base_url has no host"}};
    }
    return result;
}

constexpr std::expected<endpoint_url, std::string> parse_url(std::string_view const url) {
    constexpr std::string_view https = "https://";
    constexpr std::string_view http = "http://";
    bool const secure = url.starts_with(https);
    if (!secure && !url.starts_with(http)) {
        return std::unexpected{std::string{"base_url must start with http:// or https://"}};
    }
    auto const rest = url.substr(secure ? https.size() : http.size());
    auto const slash = std::min(rest.find('/'), rest.size());
    auto const where = split_authority(rest.substr(0, slash), secure);
    if (!where) {
        return std::unexpected{where.error()};
    }
    return endpoint_url{.secure = secure, .host = std::string{where->host}, .port = where->port, .base_path = std::string{rest.substr(slash)}};
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

constexpr bool secrets_equal(std::string_view const left, std::string_view const right) noexcept {
    std::size_t difference = left.size() ^ right.size();
    auto const longest = std::max(left.size(), right.size());
    for (std::size_t at = 0; at < longest; ++at) {
        auto const a = at < left.size() ? static_cast<unsigned char>(left[at]) : 0U;
        auto const b = at < right.size() ? static_cast<unsigned char>(right[at]) : 0U;
        difference |= a ^ b;
    }
    return difference == 0;
}

constexpr bool is_loopback_host(std::string_view const host) noexcept {
    return host == "localhost" || host == "::1" || host == "[::1]" || host.starts_with("127.");
}

constexpr std::string with_port(std::string_view const host, std::uint16_t const port) {
    std::array<char, 8> digits{};
    auto const end = std::to_chars(digits.data(), digits.data() + digits.size(), port).ptr;
    return std::string{host} + ":" + std::string{digits.data(), end};
}

constexpr std::vector<std::string> default_allowed_hosts(std::string_view const listen_host, std::uint16_t const port) {
    if (!is_loopback_host(listen_host)) {
        return {with_port(listen_host, port)};
    }
    return {with_port("127.0.0.1", port), with_port("localhost", port), with_port("[::1]", port)};
}

constexpr bool host_allowed(std::string_view const header, std::span<std::string const> const allowed) {
    return std::ranges::any_of(allowed, [header](std::string const& candidate) {
        return std::ranges::equal(candidate, header, [](char const a, char const b) { return lowered(a) == lowered(b); });
    });
}

constexpr std::string_view query_value(std::string_view const target, std::string_view const name) noexcept {
    auto const question = target.find('?');
    if (question == std::string_view::npos) {
        return {};
    }
    auto rest = target.substr(question + 1);
    while (!rest.empty()) {
        auto const end = rest.find('&');
        auto const pair = rest.substr(0, end);
        auto const equals = pair.find('=');
        if (pair.substr(0, equals) == name && equals != std::string_view::npos) {
            return pair.substr(equals + 1);
        }
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
    }
    return {};
}

constexpr std::size_t longest_challenge = 64;

constexpr bool valid_challenge(std::string_view const text) noexcept {
    return !text.empty() && text.size() <= longest_challenge &&
           std::ranges::all_of(text, [](char const c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); });
}

constexpr std::string_view bearer_value(std::string_view const authorization) noexcept {
    constexpr std::string_view prefix = "Bearer ";
    return authorization.starts_with(prefix) ? authorization.substr(prefix.size()) : std::string_view{};
}

constexpr std::expected<std::string_view, reply_body> model_field(std::string_view const body, std::span<jsonscan::member const> const members) {
    auto const models = jsonscan::members_named(members, "model");
    auto const model_text = agreed_value(body, models);
    if (models.empty() || models.front().kind != jsonscan::value_kind::string) {
        return std::unexpected{error_reply(400, "invalid_request_error", "model is required and must be a string")};
    }
    if (!model_text) {
        return std::unexpected{error_reply(400, "invalid_request_error", "model appears more than once with different values")};
    }
    return *model_text;
}

constexpr std::expected<bool, reply_body> stream_field(std::string_view const body, std::span<jsonscan::member const> const members) {
    auto const streams = jsonscan::members_named(members, "stream");
    auto const stream_text = agreed_value(body, streams);
    if (!streams.empty() && !stream_text) {
        return std::unexpected{error_reply(400, "invalid_request_error", "stream appears more than once with different values")};
    }
    return stream_text == "true";
}

template <class Lookup>
concept model_lookup = std::invocable<Lookup&, std::string const&> &&
                       std::convertible_to<std::invoke_result_t<Lookup&, std::string const&>, std::optional<std::string>>;

template <model_lookup Lookup>
constexpr std::expected<prepared_request, reply_body> prepare(std::string_view const body, Lookup&& lookup) {
    auto const members = jsonscan::top_level_members(body);
    if (!members) {
        return std::unexpected{error_reply(400, "invalid_request_error", "the request body is not a JSON object")};
    }
    auto const model_text = model_field(body, *members);
    if (!model_text) {
        return std::unexpected{model_text.error()};
    }
    auto const stream = stream_field(body, *members);
    if (!stream) {
        return std::unexpected{stream.error()};
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
    return prepared_request{.body = std::move(text), .stream = *stream, .requested = requested};
}

consteval bool route_table_holds() {
    auto const fallbacks = std::to_array({route::unknown_path, route::wrong_method});
    auto const own_route = [](route_entry const& item) { return route_of(item.where.verb(), item.where.target()) == item.which; };
    auto const unique = [](route_entry const& left) {
        return std::ranges::count_if(route_table, [&](route_entry const& right) { return left.where.verb() == right.where.verb() && left.where.target() == right.where.target(); }) == 1;
    };
    auto const answers_for_any_verb = [](route_entry const& item) { return route_of("PATCH", item.where.target()) == route::wrong_method; };
    return route_table.size() + fallbacks.size() == std::meta::enumerators_of(^^route).size() && std::ranges::all_of(route_table, own_route) &&
           std::ranges::all_of(route_table, unique) && std::ranges::all_of(route_table, answers_for_any_verb) &&
           std::ranges::none_of(route_table, [&](route_entry const& item) { return std::ranges::contains(fallbacks, item.which); });
}

static_assert(route_table_holds());
static_assert(route_of("POST", "/anthropic/v1/messages?beta=true") == route::messages);
static_assert(route_of("GET", "/anthropic/v1/messages") == route::wrong_method);
static_assert(route_of("GET", "/nope") == route::unknown_path);
static_assert(parse_url("https://api.example.com").value().port == https_port);
static_assert(parse_url("http://127.0.0.1:9090/base").value().base_path == "/base");
static_assert(!parse_url("ftp://x").has_value());
static_assert(secrets_equal("abc", "abc") && !secrets_equal("abc", "abd") && !secrets_equal("abc", "abcd") && secrets_equal("", ""));
static_assert(is_loopback_host("127.0.0.1") && is_loopback_host("localhost") && !is_loopback_host("0.0.0.0") && !is_loopback_host("10.0.0.5"));
static_assert(default_allowed_hosts("127.0.0.1", 9480).size() == 3 && default_allowed_hosts("10.0.0.5", 80).front() == "10.0.0.5:80");
static_assert(host_allowed("LocalHost:9480", default_allowed_hosts("127.0.0.1", 9480)) && !host_allowed("evil.example:9480", default_allowed_hosts("127.0.0.1", 9480)));
static_assert(query_value("/healthz?challenge=ab12&x=1", "challenge") == "ab12" && query_value("/h?x=1&challenge=cd", "challenge") == "cd");
static_assert(query_value("/healthz", "challenge").empty() && query_value("/h?challenge", "challenge").empty() && query_value("/h?a=1", "challenge").empty());
static_assert(valid_challenge("0aFf") && !valid_challenge("") && !valid_challenge("xyz") && !valid_challenge(std::string(65, 'a')));
static_assert(bearer_value("Bearer k") == "k" && bearer_value("Basic k").empty());
static_assert(hop_by_hop("Transfer-Encoding") && !hop_by_hop("Content-Type"));

}  // namespace llmgate::routing

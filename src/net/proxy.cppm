export module llmgate.proxy;

import std;
import llmgate.reflect;
import llmgate.text;

export namespace llmgate::proxy {

using reflect::make_error_code;

constexpr std::uint16_t default_port = 1080;
constexpr std::size_t longest_reply = 8192;
constexpr std::size_t longest_reason = 80;

struct proxy_url {
    std::string host{};
    std::uint16_t port = default_port;
    std::string authorization{};
};

struct policy {
    std::optional<proxy_url> via{};
    std::vector<std::string> bypass{};
};

enum class [[= reflect::error_enum]] errc {
    proxy_refused [[= reflect::meaning{"the proxy refused to open a tunnel to the provider: check network.proxy and what the proxy allows"}]] = 1,
    proxy_wants_credentials [[= reflect::meaning{"the proxy asks for credentials: put user:password@ in front of the host in network.proxy"}]],
    proxy_closed [[= reflect::meaning{"the proxy closed the connection before it answered"}]],
};
static_assert(reflect::unique_values<errc>() && reflect::starts_at_one<errc>());

enum class outcome { incomplete, tunnel, authentication_required, refused };

struct reply {
    outcome which = outcome::incomplete;
    int status = 0;
    std::size_t consumed = 0;
    std::string reason{};
};

}  // namespace llmgate::proxy

namespace llmgate::proxy::detail {

constexpr std::string_view http_scheme = "http://";
constexpr std::string_view blank_line = "\r\n\r\n";
constexpr std::string_view base64_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr std::size_t status_digits = 3;
constexpr int first_success = 200;
constexpr int first_redirect = 300;
constexpr int needs_credentials = 407;
constexpr std::size_t ipv4_bytes = 4;
constexpr std::size_t ipv6_bytes = 16;
constexpr std::size_t ipv6_groups = 8;
constexpr unsigned bits_per_byte = 8;
constexpr unsigned ipv4_bits = 32;
constexpr unsigned ipv6_bits = 128;
constexpr unsigned byte_limit = 255;
constexpr unsigned group_limit = 0xFFFF;
constexpr unsigned hex_base = 16;
constexpr unsigned decimal_base = 10;
constexpr std::size_t printable_first = 0x20;
constexpr std::size_t printable_last = 0x7E;

struct address {
    std::array<std::uint8_t, ipv6_bytes> bytes{};
    bool v4 = false;
};

constexpr char lowered(char const c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

constexpr std::string lower_text(std::string_view const text) {
    std::string out;
    out.reserve(text.size());
    for (char const c : text) {
        out.push_back(lowered(c));
    }
    return out;
}

constexpr std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

constexpr std::optional<unsigned> number_in(std::string_view const text, unsigned const base, unsigned const limit) {
    unsigned value = 0;
    auto const parsed = std::from_chars(text.data(), text.data() + text.size(), value, static_cast<int>(base));
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value > limit) {
        return std::nullopt;
    }
    return value;
}

constexpr std::optional<address> parse_ipv4(std::string_view const text) {
    address out{.v4 = true};
    std::size_t index = 0;
    for (auto const part : std::views::split(text, '.')) {
        auto const value = number_in(std::string_view{part.begin(), part.end()}, decimal_base, byte_limit);
        if (!value || index == ipv4_bytes) {
            return std::nullopt;
        }
        out.bytes[index++] = static_cast<std::uint8_t>(*value);
    }
    return index == ipv4_bytes ? std::optional{out} : std::nullopt;
}

constexpr std::optional<std::vector<unsigned>> groups_of(std::string_view const text) {
    std::vector<unsigned> groups;
    if (text.empty()) {
        return groups;
    }
    for (auto const part : std::views::split(text, ':')) {
        auto const value = number_in(std::string_view{part.begin(), part.end()}, hex_base, group_limit);
        if (!value) {
            return std::nullopt;
        }
        groups.push_back(*value);
    }
    return groups;
}

struct halves {
    std::vector<unsigned> head{};
    std::vector<unsigned> tail{};
    bool gapped = false;
};

constexpr std::optional<halves> halves_of(std::string_view const text) {
    auto const gap = text.find("::");
    auto head = groups_of(text.substr(0, gap));
    auto tail = gap == std::string_view::npos ? std::optional{std::vector<unsigned>{}} : groups_of(text.substr(gap + 2));
    bool const twice = gap != std::string_view::npos && text.find("::", gap + 1) != std::string_view::npos;
    if (!head || !tail || twice) {
        return std::nullopt;
    }
    return halves{.head = std::move(*head), .tail = std::move(*tail), .gapped = gap != std::string_view::npos};
}

constexpr address pack(std::span<unsigned const> const groups) {
    address out{};
    for (std::size_t index = 0; index < ipv6_groups; ++index) {
        out.bytes[index * 2] = static_cast<std::uint8_t>(groups[index] >> bits_per_byte);
        out.bytes[index * 2 + 1] = static_cast<std::uint8_t>(groups[index] & byte_limit);
    }
    return out;
}

constexpr std::optional<address> parse_ipv6(std::string_view const text) {
    auto const parts = halves_of(text);
    if (!parts) {
        return std::nullopt;
    }
    auto const used = parts->head.size() + parts->tail.size();
    if (parts->gapped ? used >= ipv6_groups : used != ipv6_groups) {
        return std::nullopt;
    }
    std::vector<unsigned> all = parts->head;
    all.resize(ipv6_groups - parts->tail.size(), 0);
    std::ranges::copy(parts->tail, std::back_inserter(all));
    return pack(all);
}

constexpr std::optional<address> parse_address(std::string_view const text) {
    return text.contains(':') ? parse_ipv6(text) : parse_ipv4(text);
}

constexpr bool bit_of(address const& each, unsigned const bit) {
    return (each.bytes[bit / bits_per_byte] >> (bits_per_byte - 1 - bit % bits_per_byte) & 1U) != 0;
}

constexpr bool within(address const& inside, address const& network, unsigned const prefix) {
    if (inside.v4 != network.v4 || prefix > (inside.v4 ? ipv4_bits : ipv6_bits)) {
        return false;
    }
    return std::ranges::all_of(std::views::iota(0U, prefix), [&](unsigned const bit) { return bit_of(inside, bit) == bit_of(network, bit); });
}

constexpr std::string_view bare_host(std::string_view host) {
    if (host.starts_with('[') && host.ends_with(']')) {
        host = host.substr(1, host.size() - 2);
    }
    if (host.ends_with('.')) {
        host.remove_suffix(1);
    }
    return host;
}

struct split_text {
    std::string_view host{};
    std::string_view digits{};
};

constexpr std::optional<split_text> port_split(std::string_view const hostport) {
    bool const bracketed = hostport.starts_with('[');
    auto const colon = bracketed ? hostport.find("]:") : hostport.rfind(':');
    if (colon == std::string_view::npos || (!bracketed && hostport.find(':') != colon)) {
        return std::nullopt;
    }
    auto const shift = bracketed ? 1U : 0U;
    return split_text{.host = hostport.substr(0, colon + shift), .digits = hostport.substr(colon + shift + 1)};
}

struct entry_parts {
    std::string_view host{};
    std::optional<std::uint16_t> port{};
};

constexpr std::optional<entry_parts> split_entry(std::string_view const entry) {
    auto const parts = port_split(entry);
    if (!parts) {
        return entry_parts{.host = bare_host(entry)};
    }
    auto const value = number_in(parts->digits, decimal_base, std::numeric_limits<std::uint16_t>::max());
    if (!value) {
        return std::nullopt;
    }
    return entry_parts{.host = bare_host(parts->host), .port = static_cast<std::uint16_t>(*value)};
}

constexpr bool network_matches(std::string_view const entry, std::string_view const host) {
    auto const slash = entry.find('/');
    auto const network = parse_address(bare_host(entry.substr(0, slash)));
    auto const inside = parse_address(host);
    auto const prefix = number_in(entry.substr(slash + 1), decimal_base, ipv6_bits);
    return network && inside && prefix && within(*inside, *network, *prefix);
}

constexpr bool domain_matches(std::string_view const wanted, std::string_view const host) {
    auto const name = wanted.starts_with('.') ? wanted.substr(1) : wanted;
    if (name.empty() || parse_address(host)) {
        return !name.empty() && name == host;
    }
    return host == name || (host.size() > name.size() && host.ends_with(name) && host[host.size() - name.size() - 1] == '.');
}

constexpr bool entry_matches(std::string_view const entry, std::string_view const host, std::uint16_t const port) {
    if (entry == "*") {
        return true;
    }
    if (entry.contains('/')) {
        return network_matches(entry, host);
    }
    auto const parts = split_entry(entry);
    if (!parts || (parts->port && *parts->port != port)) {
        return false;
    }
    return domain_matches(parts->host, host);
}

constexpr bool loopback(std::string_view const host) {
    if (host == "localhost" || host.ends_with(".localhost")) {
        return true;
    }
    auto const found = parse_address(host);
    if (!found) {
        return false;
    }
    return found->v4 ? found->bytes[0] == 127 : within(*found, *parse_ipv6("::1"), ipv6_bits);
}

constexpr std::expected<std::string, std::string> percent_decoded(std::string_view const text) {
    std::string out;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '%') {
            out.push_back(text[index]);
            continue;
        }
        auto const value = index + 2 < text.size() ? number_in(text.substr(index + 1, 2), hex_base, byte_limit) : std::nullopt;
        if (!value) {
            return std::unexpected{std::string{"the proxy credentials hold a bad %-escape"}};
        }
        out.push_back(static_cast<char>(*value));
        index += 2;
    }
    return out;
}

constexpr std::string base64_of(std::string_view const bytes) {
    std::string out;
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        auto const left = std::min<std::size_t>(3, bytes.size() - index);
        std::uint32_t block = 0;
        for (std::size_t offset = 0; offset < 3; ++offset) {
            block = block << bits_per_byte | (offset < left ? static_cast<unsigned char>(bytes[index + offset]) : 0U);
        }
        for (std::size_t digit = 0; digit < 4; ++digit) {
            out.push_back(digit <= left ? base64_alphabet[block >> (18 - 6 * digit) & 0x3F] : '=');
        }
    }
    return out;
}

struct authority_parts {
    std::string_view userinfo{};
    std::string_view hostport{};
};

constexpr std::optional<authority_parts> authority_of(std::string_view const text) {
    if (!text.starts_with(http_scheme)) {
        return std::nullopt;
    }
    auto const rest = text.substr(http_scheme.size());
    auto const authority = rest.substr(0, rest.find('/'));
    auto const at = authority.rfind('@');
    if (at == std::string_view::npos) {
        return authority_parts{.hostport = authority};
    }
    return authority_parts{.userinfo = authority.substr(0, at), .hostport = authority.substr(at + 1)};
}

constexpr std::expected<std::pair<std::string, std::uint16_t>, std::string> host_and_port(std::string_view const hostport) {
    auto host = hostport;
    std::uint16_t port = default_port;
    if (auto const parts = port_split(hostport)) {
        auto const value = number_in(parts->digits, decimal_base, std::numeric_limits<std::uint16_t>::max());
        if (!value) {
            return std::unexpected{std::string{"the proxy URL has an invalid port"}};
        }
        port = static_cast<std::uint16_t>(*value);
        host = parts->host;
    }
    host = bare_host(host);
    if (host.empty()) {
        return std::unexpected{std::string{"the proxy URL has no host"}};
    }
    return std::pair{std::string{host}, port};
}

constexpr std::string printable(std::string_view const text) {
    std::string out;
    for (char const c : text) {
        auto const code = static_cast<unsigned char>(c);
        if (code >= printable_first && code <= printable_last && out.size() < longest_reason) {
            out.push_back(c);
        }
    }
    return out;
}

constexpr std::optional<int> status_of(std::string_view const line) {
    auto const space = line.find(' ');
    if (!line.starts_with("HTTP/1.") || space == std::string_view::npos || line.size() < space + 1 + status_digits) {
        return std::nullopt;
    }
    auto const value = number_in(line.substr(space + 1, status_digits), decimal_base, 999);
    return value ? std::optional{static_cast<int>(*value)} : std::nullopt;
}

}  // namespace llmgate::proxy::detail

export namespace llmgate::proxy {

constexpr std::expected<proxy_url, std::string> parse(std::string_view const text) {
    auto const parts = detail::authority_of(text);
    if (!parts) {
        return std::unexpected{std::string{"the proxy must be an http:// URL (https proxies are not supported)"}};
    }
    auto where = detail::host_and_port(parts->hostport);
    if (!where) {
        return std::unexpected{std::move(where.error())};
    }
    proxy_url out{.host = std::move(where->first), .port = where->second};
    if (!parts->userinfo.empty()) {
        auto decoded = detail::percent_decoded(parts->userinfo);
        if (!decoded) {
            return std::unexpected{std::move(decoded.error())};
        }
        out.authorization = "Basic ";
        out.authorization += detail::base64_of(*decoded);
    }
    return out;
}

constexpr std::string mask(std::string_view const text) {
    auto const parts = detail::authority_of(text);
    auto const colon = parts ? parts->userinfo.find(':') : std::string_view::npos;
    if (colon == std::string_view::npos) {
        return std::string{text};
    }
    std::string out{detail::http_scheme};
    text::append_all(out, parts->userinfo.substr(0, colon), ":***@", text.substr(text.find(parts->hostport, detail::http_scheme.size())));
    return out;
}

constexpr std::vector<std::string> split_list(std::string_view const text) {
    std::vector<std::string> out;
    for (auto const part : std::views::split(text, ',')) {
        auto const entry = detail::trimmed(std::string_view{part.begin(), part.end()});
        if (!entry.empty()) {
            out.push_back(detail::lower_text(entry));
        }
    }
    return out;
}

constexpr bool bypasses(std::span<std::string const> const entries, std::string_view const host, std::uint16_t const port) {
    auto const name = detail::lower_text(detail::bare_host(host));
    return std::ranges::any_of(entries, [&](std::string const& entry) { return detail::entry_matches(entry, name, port); });
}

constexpr std::optional<proxy_url> route_for(policy const& chosen, std::string_view const host, std::uint16_t const port) {
    if (!chosen.via || detail::loopback(detail::lower_text(detail::bare_host(host))) || bypasses(chosen.bypass, host, port)) {
        return std::nullopt;
    }
    return chosen.via;
}

constexpr std::string connect_request(std::string_view const host, std::uint16_t const port, std::string_view const authorization) {
    std::string target = host.contains(':') ? "[" : "";
    text::append_all(target, host, host.contains(':') ? "]" : "", ":", text::decimal(port));
    std::string out;
    text::append_all(out, "CONNECT ", target, " HTTP/1.1\r\nHost: ", target, "\r\n");
    if (!authorization.empty()) {
        text::append_all(out, "Proxy-Authorization: ", authorization, "\r\n");
    }
    text::append_all(out, "Proxy-Connection: keep-alive\r\n\r\n");
    return out;
}

constexpr reply parse_reply(std::string_view const bytes) {
    auto const end = bytes.find(detail::blank_line);
    if (end == std::string_view::npos) {
        return bytes.size() > longest_reply ? reply{.which = outcome::refused, .reason = "the proxy's answer is too long"} : reply{};
    }
    auto const line = bytes.substr(0, bytes.find("\r\n"));
    auto const status = detail::status_of(line);
    if (!status) {
        return {.which = outcome::refused, .reason = "the proxy did not answer with HTTP"};
    }
    auto const reason = detail::printable(detail::trimmed(line.substr(line.find(' ') + 1 + detail::status_digits)));
    auto const consumed = end + detail::blank_line.size();
    if (*status >= detail::first_success && *status < detail::first_redirect) {
        return {.which = outcome::tunnel, .status = *status, .consumed = consumed, .reason = reason};
    }
    return {.which = *status == detail::needs_credentials ? outcome::authentication_required : outcome::refused, .status = *status, .consumed = consumed, .reason = reason};
}

static_assert(parse("http://proxy.example:3128").value().host == "proxy.example" && parse("http://proxy.example:3128").value().port == 3128);
static_assert(parse("http://proxy.example").value().port == default_port && parse("http://[::1]:8080/path").value().host == "::1" && parse("http://[::1]:8080/path").value().port == 8080);
static_assert(parse("http://user:p%40ss@proxy.example:8080").value().authorization == "Basic dXNlcjpwQHNz");
static_assert(parse("http://user@proxy.example").value().authorization == "Basic dXNlcg==");
static_assert(!parse("https://proxy.example").has_value() && !parse("proxy.example:3128").has_value() && !parse("http://").has_value());
static_assert(!parse("http://proxy.example:99999").has_value() && !parse("http://u:%zz@proxy.example").has_value());
static_assert(mask("http://user:secret@proxy.example:8080") == "http://user:***@proxy.example:8080" && mask("http://proxy.example") == "http://proxy.example");

constexpr bool names_match() {
    std::vector<std::string> const corpus{"example.com", ".internal.test", "host.test:8443"};
    return bypasses(corpus, "example.com", 443) && bypasses(corpus, "API.Example.Com", 443) && bypasses(corpus, "example.com.", 80) && !bypasses(corpus, "notexample.com", 443) &&
           !bypasses(corpus, "example.org", 443) && bypasses(corpus, "a.b.internal.test", 443) && bypasses(corpus, "internal.test", 443);
}
static_assert(names_match());

constexpr bool ports_match() {
    std::vector<std::string> const corpus{"host.test:8443", "[2001:db8::1]:443"};
    return bypasses(corpus, "host.test", 8443) && !bypasses(corpus, "host.test", 443) && bypasses(corpus, "2001:db8::1", 443) && !bypasses(corpus, "2001:db8::1", 80) &&
           bypasses(corpus, "[2001:db8::1]", 443);
}
static_assert(ports_match());

constexpr bool networks_match() {
    std::vector<std::string> const corpus{"10.0.0.0/8", "fd00::/8", "192.168.1.7"};
    return bypasses(corpus, "10.20.30.40", 80) && !bypasses(corpus, "11.0.0.1", 80) && bypasses(corpus, "fd12::1", 80) && !bypasses(corpus, "fe80::1", 80) &&
           bypasses(corpus, "192.168.1.7", 1) && !bypasses(corpus, "192.168.1.8", 1) && !bypasses(corpus, "x.1.7", 1);
}
static_assert(networks_match());

constexpr bool star_matches() {
    std::vector<std::string> const star{"*"};
    return bypasses(star, "anything.test", 1) && !bypasses(std::span<std::string const>{}, "anything.test", 1);
}
static_assert(star_matches());
static_assert(split_list(" Example.COM , ,10.0.0.0/8 ").size() == 2 && split_list(" Example.COM , ").front() == "example.com" && split_list("").empty());

constexpr bool routing_holds() {
    policy const through{.via = proxy_url{.host = "p", .port = 3128}, .bypass = {"direct.test"}};
    return route_for(through, "api.test", 443).has_value() && !route_for(through, "direct.test", 443).has_value() && !route_for(through, "localhost", 80).has_value() &&
           !route_for(through, "127.0.0.1", 80).has_value() && !route_for(through, "::1", 80).has_value() && !route_for(through, "[::1]", 80).has_value() && !route_for(policy{}, "api.test", 443).has_value();
}
static_assert(routing_holds());

static_assert(connect_request("api.test", 443, "") == "CONNECT api.test:443 HTTP/1.1\r\nHost: api.test:443\r\nProxy-Connection: keep-alive\r\n\r\n");
static_assert(connect_request("::1", 8, "Basic eA==") == "CONNECT [::1]:8 HTTP/1.1\r\nHost: [::1]:8\r\nProxy-Authorization: Basic eA==\r\nProxy-Connection: keep-alive\r\n\r\n");

static_assert(parse_reply("HTTP/1.1 200 Connection established\r\n\r\n").which == outcome::tunnel && parse_reply("HTTP/1.1 200 Connection established\r\n\r\n").consumed == 39);
static_assert(parse_reply("HTTP/1.0 200 OK\r\nVia: x\r\n\r\nrest").consumed == 27 && parse_reply("HTTP/1.0 200 OK\r\nVia: x\r\n\r\nrest").which == outcome::tunnel);
static_assert(parse_reply("HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Basic\r\n\r\n").which == outcome::authentication_required);
static_assert(parse_reply("HTTP/1.1 502 Bad Gateway\r\n\r\n").which == outcome::refused && parse_reply("HTTP/1.1 502 Bad Gateway\r\n\r\n").status == 502 && parse_reply("HTTP/1.1 502 Bad Gateway\r\n\r\n").reason == "Bad Gateway");
static_assert(parse_reply("HTTP/1.1 200 OK\r\n").which == outcome::incomplete && parse_reply("").which == outcome::incomplete);
static_assert(parse_reply("SSH-2.0-OpenSSH\r\n\r\n").which == outcome::refused && parse_reply("HTTP/1.1 20 OK\r\n\r\n").which == outcome::refused);
static_assert(parse_reply("HTTP/1.1 302 Found\r\nLocation: x\r\n\r\n").which == outcome::refused);

}  // namespace llmgate::proxy

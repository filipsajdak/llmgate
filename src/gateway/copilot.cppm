module;

#include <cstdlib>
#include <meta>

export module llmgate.copilot;

import std;
import llmgate.async;
import llmgate.fetch;
import llmgate.gateway;
import llmgate.http;
import llmgate.httpio;
import llmgate.json;
import llmgate.members;
import llmgate.reflect;
import llmgate.tls;

export namespace llmgate::copilot {

using std::chrono::milliseconds;
using std::chrono::seconds;
using system_clock = std::chrono::system_clock;

constexpr std::size_t github_error_capacity = 24;

struct github_error {
    char text[github_error_capacity]{};

    consteval github_error(std::string_view const word) {
        if (word.size() >= github_error_capacity) {
            throw "copilot::github_error: the word does not fit github_error_capacity";
        }
        std::ranges::copy(word, text);
    }
};

enum class [[=reflect::error_enum]] errc {
    login_required = 1,
    login_denied [[= github_error{"access_denied"}]],
    login_expired [[= github_error{"expired_token"}]],
    github_unreachable,
    github_rejected,
    bad_reply
};
static_assert(reflect::unique_values<errc>() && reflect::starts_at_one<errc>());

using reflect::make_error_code;

struct device_code {
    [[= json::required]] std::string device_code{};
    [[= json::required]] std::string user_code{};
    [[= json::required]] std::string verification_uri{};
    std::int64_t expires_in = 900;
    std::int64_t interval = 5;
};

struct oauth_reply {
    std::string access_token{};
    std::string error{};
};

struct token_endpoints {
    std::string api{};
};

struct token_reply {
    [[= json::required]] std::string token{};
    [[= json::required]] std::int64_t expires_at = 0;
    std::int64_t refresh_in = 0;
    token_endpoints endpoints{};
};

struct stored_token {
    [[= json::required]] std::string oauth_token{};
};

constexpr seconds refresh_margin{300};
constexpr milliseconds github_limit = std::chrono::seconds{15};
constexpr milliseconds slow_down_step = std::chrono::seconds{5};
constexpr std::string_view editor_version = "vscode/1.99.0";
constexpr std::string_view integration_id = "vscode-chat";
constexpr std::string_view user_target = "/copilot_internal/user";
constexpr std::string_view device_grant = "urn:ietf:params:oauth:grant-type:device_code";

}  // namespace llmgate::copilot

namespace llmgate::copilot::detail {

constexpr bool unreserved(char const c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || std::string_view{"-._~"}.contains(c);
}

constexpr std::optional<errc> errc_for_github_error(std::string_view const word) {
    std::optional<errc> found;
    template for (constexpr auto enumerator : std::define_static_array(std::meta::enumerators_of(^^errc))) {
        if constexpr (reflect::has_annotation<github_error>(enumerator)) {
            constexpr auto note = *reflect::annotation_of<github_error>(enumerator);
            if (word == std::string_view{note.text}) {
                found = [:enumerator:];
            }
        }
    }
    return found;
}

static_assert(errc_for_github_error("access_denied") == errc::login_denied && errc_for_github_error("expired_token") == errc::login_expired);
static_assert(!errc_for_github_error("authorization_pending") && !errc_for_github_error("") && !errc_for_github_error("access_denied "));

void append_percent(std::string& out, char const c) {
    std::format_to(std::back_inserter(out), "%{:02X}", static_cast<unsigned char>(c));
}

std::string url_encode(std::string_view const text) {
    std::string out;
    for (auto const c : text) {
        if (unreserved(c)) {
            out.push_back(c);
        } else {
            append_percent(out, c);
        }
    }
    return out;
}

std::string form(std::initializer_list<std::pair<std::string_view, std::string_view>> const fields) {
    std::string body;
    for (auto const& [name, value] : fields) {
        std::format_to(std::back_inserter(body), "{}{}={}", body.empty() ? "" : "&", url_encode(name), url_encode(value));
    }
    return body;
}

std::string expand_home(std::string_view const path) {
    if (!path.starts_with("~/")) {
        return std::string{path};
    }
    auto const* home = std::getenv("HOME");
    return (home == nullptr ? std::string{} : std::string{home}) + std::string{path.substr(1)};
}

template <class T>
std::expected<T, std::error_code> parse_reply(std::string const& body) {
    auto parsed = json::read_text<T>(body, {.jsonc = false}, {.allow_unknown_keys = true});
    if (!parsed) {
        return std::unexpected{make_error_code(errc::bad_reply)};
    }
    return std::move(*parsed);
}

httpio::request_spec github_post(std::string path, std::string body) {
    http::header_map fields;
    fields.add("Accept", "application/json");
    fields.add("Content-Type", "application/x-www-form-urlencoded");
    fields.add("User-Agent", "llmgate/0.0");
    return {.method = "POST", .target = std::move(path), .host = {}, .fields = std::move(fields), .body = std::move(body)};
}

}  // namespace llmgate::copilot::detail

export namespace llmgate::copilot {

std::expected<std::string, std::error_code> load_oauth_token(std::string_view const path) {
    std::ifstream in{detail::expand_home(path)};
    if (!in) {
        return std::unexpected{make_error_code(errc::login_required)};
    }
    std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    auto stored = json::read_text<stored_token>(text, {.jsonc = false}, {.allow_unknown_keys = true});
    if (!stored || stored->oauth_token.empty()) {
        return std::unexpected{make_error_code(errc::login_required)};
    }
    return std::move(stored->oauth_token);
}

std::expected<void, std::error_code> save_oauth_token(std::string_view path, std::string_view token);

}  // namespace llmgate::copilot

export namespace llmgate::copilot {

async::task<std::expected<device_code, std::error_code>> request_device_code(async::reactor& loop, tls::client_context const& tls_context,
                                                                             gateway::copilot_config settings);

async::task<std::expected<std::string, std::error_code>> wait_for_authorisation(async::reactor& loop, tls::client_context const& tls_context,
                                                                                gateway::copilot_config settings, device_code code);

async::task<std::expected<std::string, std::error_code>> fetch_user(async::reactor& loop, tls::client_context const& tls_context,
                                                                    gateway::copilot_config settings);

async::task<std::error_code> check_login(async::reactor& loop, tls::client_context const& tls_context, gateway::copilot_config settings);

class copilot_credential final : public gateway::credential {
public:
    using clock_function = std::function<system_clock::time_point()>;

    copilot_credential(async::reactor& loop, tls::client_context const& tls_context, gateway::copilot_config settings,
                       clock_function clock = [] { return system_clock::now(); })
        : loop_{&loop}, tls_context_{&tls_context}, settings_{std::move(settings)}, clock_{std::move(clock)}, refresh_lock_{loop} {}

    async::task<std::expected<std::string, std::error_code>> bearer() override {
        if (auto const fresh = usable()) {
            co_return *fresh;
        }
        auto const turn = co_await refresh_lock_.lock();
        if (auto const fresh = usable()) {
            co_return *fresh;
        }
        co_return co_await exchange();
    }

    void rejected() noexcept override { cached_.reset(); }

    std::optional<std::string> endpoint() const override {
        if (!cached_ || cached_->api.empty()) {
            return std::nullopt;
        }
        return cached_->api;
    }

    int exchanges() const noexcept { return exchanges_; }

private:
    struct cached_token {
        std::string token;
        std::string api;
        system_clock::time_point refresh_at;
    };

    std::optional<std::string> usable() const {
        if (cached_ && clock_() < cached_->refresh_at) {
            return cached_->token;
        }
        return std::nullopt;
    }

    async::task<std::expected<std::string, std::error_code>> exchange() {
        if (login_rejected_) {
            co_return std::unexpected{make_error_code(errc::login_required)};
        }
        auto oauth = load_oauth_token(settings_.token_file);
        if (!oauth) {
            co_return std::unexpected{oauth.error()};
        }
        http::header_map fields;
        fields.add("Authorization", "token " + *oauth);
        fields.add("Accept", "application/json");
        fields.add("Editor-Version", std::string{editor_version});
        fields.add("User-Agent", "llmgate/0.0");
        ++exchanges_;
        auto const answer = co_await fetch::request(*loop_, *tls_context_, settings_.api_url,
                                                    {.method = "GET", .target = "/copilot_internal/v2/token", .host = {}, .fields = std::move(fields)},
                                                    github_limit);
        if (!answer) {
            co_return std::unexpected{make_error_code(errc::github_unreachable)};
        }
        if (answer->status == 401) {
            login_rejected_ = true;
            co_return std::unexpected{make_error_code(errc::login_required)};
        }
        if (answer->status != 200) {
            co_return std::unexpected{make_error_code(errc::github_rejected)};
        }
        auto reply = detail::parse_reply<token_reply>(answer->body);
        if (!reply) {
            co_return std::unexpected{reply.error()};
        }
        auto const expires = system_clock::time_point{seconds{reply->expires_at}};
        auto refresh_at = expires - refresh_margin;
        if (reply->refresh_in > 0) {
            refresh_at = std::min(refresh_at, clock_() + seconds{reply->refresh_in});
        }
        cached_ = cached_token{.token = reply->token, .api = reply->endpoints.api, .refresh_at = refresh_at};
        co_return reply->token;
    }

    async::reactor* loop_;
    tls::client_context const* tls_context_;
    gateway::copilot_config settings_;
    clock_function clock_;
    async::mutex refresh_lock_;
    std::optional<cached_token> cached_;
    bool login_rejected_ = false;
    int exchanges_ = 0;
};

}  // namespace llmgate::copilot

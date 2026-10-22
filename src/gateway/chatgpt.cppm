module;

#include <meta>

export module llmgate.chatgpt;

import std;
import llmgate.async;
import llmgate.fetch;
import llmgate.gateway;
import llmgate.http;
import llmgate.httpio;
import llmgate.json;
import llmgate.oauth;
import llmgate.net;
import llmgate.process;
import llmgate.proof;
import llmgate.reflect;
import llmgate.text;
import llmgate.tls;

export namespace llmgate::chatgpt {

using std::chrono::seconds;
using system_clock = std::chrono::system_clock;

constexpr seconds refresh_margin{60};
constexpr std::chrono::milliseconds auth_limit{15000};
constexpr mode_t private_file_mode = 0600;

enum class [[=reflect::error_enum]] errc {
    login_required = 1,
    plan_not_enabled,
    refresh_rejected,
    auth_unreachable,
    bad_reply,
    sign_in_denied,
    sign_in_timeout,
    state_mismatch,
    bad_id_token,
    port_busy,
    exchange_rejected
};
static_assert(reflect::unique_values<errc>() && reflect::starts_at_one<errc>());

using reflect::make_error_code;

struct tokens {
    std::string client_id{};
    std::string host_id{};
    std::string access_token{};
    std::string refresh_token{};
    std::string scope{};
    std::string account{};
    std::int64_t expires_at = 0;
    std::int64_t earliest_refresh_at = 0;
};

inline std::expected<tokens, std::error_code> load_tokens(std::string_view const path) {
    std::ifstream in{process::expand_home(path)};
    if (!in) {
        return std::unexpected{make_error_code(errc::login_required)};
    }
    std::string const text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    auto stored = json::read_text<tokens>(text, {.jsonc = false}, {.allow_unknown_keys = true});
    if (!stored || stored->refresh_token.empty() || stored->client_id.empty()) {
        return std::unexpected{make_error_code(errc::login_required)};
    }
    return std::move(*stored);
}

inline std::expected<void, std::error_code> save_tokens(std::string_view const path, tokens const& held) {
    auto const content = json::write(json::to_value(held)) + "\n";
    return process::write_file_atomic(process::expand_home(path), content, private_file_mode, true);
}

}  // namespace llmgate::chatgpt

namespace llmgate::chatgpt::detail {

inline httpio::request_spec token_request(std::string body) {
    http::header_map fields;
    fields.add("Accept", "application/json");
    fields.add("Content-Type", "application/x-www-form-urlencoded");
    fields.add("User-Agent", "llmgate/0.0");
    return {.method = "POST", .target = std::string{oauth::token_path}, .host = {}, .fields = std::move(fields), .body = std::move(body)};
}

inline std::string refresh_form(tokens const& held) {
    std::string out;
    text::append_all(out, "grant_type=refresh_token&refresh_token=", oauth::percent_encode(held.refresh_token), "&client_id=", oauth::percent_encode(held.client_id));
    return out;
}

inline std::error_code refusal(std::string_view const error) {
    return make_error_code(oauth::refresh_token_unusable(error) ? errc::login_required : errc::refresh_rejected);
}

inline tokens rotated(tokens held, oauth::token_reply reply, std::int64_t const now) {
    held.access_token = std::move(reply.access_token);
    held.refresh_token = reply.refresh_token.empty() ? held.refresh_token : std::move(reply.refresh_token);
    held.scope = reply.scope.empty() ? held.scope : std::move(reply.scope);
    held.expires_at = now + reply.expires_in;
    held.earliest_refresh_at = reply.earliest_refresh_at;
    return held;
}

}  // namespace llmgate::chatgpt::detail

export namespace llmgate::chatgpt {

class chatgpt_credential final : public gateway::credential {
public:
    using clock_function = std::function<system_clock::time_point()>;

    chatgpt_credential(async::reactor& loop, tls::client_context const& tls_context, gateway::chatgpt_config settings,
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
        co_return co_await refresh();
    }

    void rejected() noexcept override { stale_ = true; }

    bool configured() const override { return load_tokens(settings_.token_file).has_value(); }

    int refreshes() const noexcept { return refreshes_; }

private:
    std::int64_t unix_now() const { return std::chrono::duration_cast<seconds>(clock_().time_since_epoch()).count(); }

    bool fresh(tokens const& held) const { return unix_now() + refresh_margin.count() < held.expires_at; }

    std::optional<std::string> usable() const {
        if (cached_ && !stale_ && fresh(*cached_)) {
            return cached_->access_token;
        }
        return std::nullopt;
    }

    std::expected<std::string, std::error_code> adopt(tokens held) {
        if (!oauth::grants_plan_use(held.scope)) {
            return std::unexpected{make_error_code(errc::plan_not_enabled)};
        }
        cached_ = std::move(held);
        stale_ = false;
        return cached_->access_token;
    }

    async::task<std::expected<std::string, std::error_code>> refresh() {
        if (login_rejected_) {
            co_return std::unexpected{make_error_code(errc::login_required)};
        }
        auto stored = load_tokens(settings_.token_file);
        if (!stored) {
            co_return std::unexpected{stored.error()};
        }
        if (!stale_ && fresh(*stored)) {
            co_return adopt(std::move(*stored));
        }
        co_return co_await exchange(std::move(*stored));
    }

    std::error_code reject(std::string_view const error) {
        auto const code = detail::refusal(error);
        login_rejected_ = code == make_error_code(errc::login_required);
        return code;
    }

    async::task<std::expected<std::string, std::error_code>> exchange(tokens stored) {
        ++refreshes_;
        auto const answer = co_await fetch::request(*loop_, *tls_context_, settings_.issuer, detail::token_request(detail::refresh_form(stored)), auth_limit);
        if (!answer) {
            co_return std::unexpected{make_error_code(errc::auth_unreachable)};
        }
        auto reply = oauth::parse_token_reply(answer->body);
        if (!reply || answer->status != 200) {
            co_return std::unexpected{reject(reply ? std::string_view{} : std::string_view{reply.error()})};
        }
        auto next = detail::rotated(std::move(stored), std::move(*reply), unix_now());
        (void)save_tokens(settings_.token_file, next);
        co_return adopt(std::move(next));
    }

    async::reactor* loop_;
    tls::client_context const* tls_context_;
    gateway::chatgpt_config settings_;
    clock_function clock_;
    async::mutex refresh_lock_;
    std::optional<tokens> cached_;
    bool stale_ = false;
    bool login_rejected_ = false;
    int refreshes_ = 0;
};


inline system_clock::time_point wall_clock() {
    return system_clock::now();
}

inline std::string advice(std::error_code const failure) {
    if (failure == make_error_code(errc::port_busy)) {
        return "the callback port is in use: stop the program using it or set chatgpt.callback_port";
    }
    if (failure == make_error_code(errc::sign_in_timeout)) {
        return "no sign-in arrived in time: run llmgate login --provider again and finish in the browser";
    }
    return failure == make_error_code(errc::sign_in_denied) ? std::string{"access was not granted in the browser"} : failure.message();
}

struct signed_in {
    std::string account{};
    bool plan_enabled = false;
};

struct sign_in_options {
    std::function<void(std::string const&)> announce{};
    std::chrono::milliseconds wait{std::chrono::minutes{5}};
};

}  // namespace llmgate::chatgpt

export namespace llmgate::chatgpt {

async::task<std::expected<signed_in, std::error_code>> sign_in(async::reactor& loop, tls::client_context const& tls_context, gateway::chatgpt_config settings,
                                                               sign_in_options options, std::function<system_clock::time_point()> clock = wall_clock);

}  // namespace llmgate::chatgpt

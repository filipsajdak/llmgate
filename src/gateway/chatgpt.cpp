module;

#include <meta>

module llmgate.chatgpt;

import std;
import llmgate.async;
import llmgate.fetch;
import llmgate.gateway;
import llmgate.http;
import llmgate.httpio;
import llmgate.json;
import llmgate.net;
import llmgate.oauth;
import llmgate.process;
import llmgate.proof;
import llmgate.routing;
import llmgate.text;
import llmgate.tls;

namespace llmgate::chatgpt::detail {

constexpr std::chrono::milliseconds poll_step{20};
constexpr std::string_view loopback_host = "127.0.0.1";
constexpr std::string_view returned_page = "<!doctype html><title>llmgate</title><p>Back in llmgate. You can close this window; the terminal shows whether the sign-in worked.</p>";
constexpr std::string_view not_completed_page = "<!doctype html><title>llmgate</title><p>The sign-in was not completed. Return to the terminal.</p>";
constexpr std::size_t uuid_bytes = 16;

struct callback_box {
    std::optional<oauth::callback> got{};
};

struct callback_handler {
    std::shared_ptr<callback_box> box;
    std::string expected_state;
    std::string expected_host;

    bool genuine(httpio::request const& incoming, oauth::callback const& arrived) const {
        auto const host = incoming.head.fields.find("host");
        return host && *host == expected_host && routing::secrets_equal(arrived.state, expected_state) && !box->got;
    }

    async::task<net::result_void> operator()(httpio::request const& incoming, httpio::response_writer<net::tcp_stream>& out) {
        auto const parsed = oauth::parse_callback(incoming.head.target);
        http::header_map fields;
        fields.add("Content-Type", "text/html; charset=utf-8");
        if (!parsed || !genuine(incoming, *parsed)) {
            co_return co_await out.send(parsed ? 400 : 404, fields, "not found");
        }
        box->got = *parsed;
        co_return co_await out.send(200, fields, parsed->error.empty() ? returned_page : not_completed_page);
    }
};

async::task<void> accept_callbacks(std::shared_ptr<net::listener> listening, std::shared_ptr<callback_handler> handler) {
    co_await httpio::serve(*listening, *handler, httpio::limits{});
}

inline std::string new_host_id() {
    auto const hex = proof::random_hex(uuid_bytes);
    std::array<std::byte, uuid_bytes> bytes{};
    for (std::size_t at = 0; hex && at < bytes.size(); ++at) {
        bytes[at] = static_cast<std::byte>(oauth::hex_value(hex->at(at * 2)).value_or(0) * 16 + oauth::hex_value(hex->at(at * 2 + 1)).value_or(0));
    }
    return oauth::uuid_urn(bytes);
}

inline std::string redirect_uri(gateway::chatgpt_config const& settings) {
    return std::format("http://{}:{}{}", loopback_host, settings.callback_port, oauth::callback_path);
}

inline std::string code_form(std::string_view const client_id, std::string_view const code, std::string_view const verifier, std::string_view const redirect) {
    std::string out;
    text::append_all(out, "grant_type=authorization_code&client_id=", oauth::percent_encode(client_id), "&code=", oauth::percent_encode(code), "&code_verifier=",
                     oauth::percent_encode(verifier), "&redirect_uri=", oauth::percent_encode(redirect), "&resource=", oauth::percent_encode(oauth::api_resource));
    return out;
}

inline std::string issued_client_id(oauth::callback const& arrived, oauth::token_reply const& reply, std::string_view const requested) {
    if (!arrived.client_id.empty()) {
        return arrived.client_id;
    }
    auto const claims = oauth::jwt_claims(reply.access_token);
    auto const from_token = claims ? json::text_at(*claims, {"client_id"}) : std::string{};
    return from_token.empty() && requested != oauth::dynamic_client_id ? std::string{requested} : from_token;
}

struct attempt {
    std::string client_id{};
    std::string host_id{};
    std::string state{};
    std::string nonce{};
    std::string verifier{};
};

inline std::expected<attempt, std::error_code> new_attempt(std::optional<tokens> const& previous) {
    auto const state = oauth::random_token();
    auto const nonce = oauth::random_token();
    auto const verifier = oauth::random_token();
    if (!state || !nonce || !verifier) {
        return std::unexpected{make_error_code(errc::bad_reply)};
    }
    return attempt{.client_id = previous ? previous->client_id : std::string{oauth::dynamic_client_id}, .host_id = previous ? previous->host_id : new_host_id(),
                   .state = *state, .nonce = *nonce, .verifier = *verifier};
}

inline std::optional<std::error_code> callback_problem(oauth::callback const& arrived, attempt const& mine, std::string_view const requested_client) {
    if (!arrived.error.empty()) {
        return make_error_code(errc::sign_in_denied);
    }
    bool const other_client = requested_client != oauth::dynamic_client_id && !arrived.client_id.empty() && arrived.client_id != requested_client;
    return arrived.state != mine.state || other_client ? std::optional<std::error_code>{make_error_code(errc::state_mismatch)} : std::nullopt;
}

inline std::expected<tokens, std::error_code> tokens_from(oauth::token_reply reply, oauth::callback const& arrived, attempt const& mine, std::string_view const issuer,
                                                         std::string_view const requested_client, std::int64_t const now) {
    auto const client_id = issued_client_id(arrived, reply, requested_client);
    auto const claims = oauth::jwt_claims(reply.id_token);
    if (client_id.empty() || !claims || !oauth::id_token_problem(*claims, issuer, client_id, mine.nonce, now).empty()) {
        return std::unexpected{make_error_code(client_id.empty() ? errc::bad_reply : errc::bad_id_token)};
    }
    return tokens{.client_id = client_id, .host_id = mine.host_id, .access_token = std::move(reply.access_token), .refresh_token = std::move(reply.refresh_token),
                  .scope = std::move(reply.scope), .account = json::text_at(*claims, {"sub"}), .expires_at = now + reply.expires_in,
                  .earliest_refresh_at = reply.earliest_refresh_at};
}

}  // namespace llmgate::chatgpt::detail

namespace llmgate::chatgpt {

async::task<std::expected<oauth::callback, std::error_code>> wait_for_callback(async::reactor& loop, std::shared_ptr<detail::callback_box> box,
                                                                                    std::chrono::milliseconds wait) {
    for (std::chrono::milliseconds waited{0}; waited < wait; waited += detail::poll_step) {
        if (box->got) {
            co_return *box->got;
        }
        co_await loop.sleep(detail::poll_step);
    }
    co_return std::unexpected{make_error_code(errc::sign_in_timeout)};
}

async::task<std::expected<oauth::token_reply, std::error_code>> exchange_code(async::reactor& loop, tls::client_context const& tls_context, gateway::chatgpt_config settings,
                                                                                    std::string form) {
    auto const answer = co_await fetch::request(loop, tls_context, settings.issuer, detail::token_request(std::move(form)), auth_limit);
    if (!answer) {
        co_return std::unexpected{make_error_code(errc::auth_unreachable)};
    }
    auto reply = oauth::parse_token_reply(answer->body);
    if (!reply || answer->status != 200) {
        co_return std::unexpected{make_error_code(errc::exchange_rejected)};
    }
    co_return std::move(*reply);
}

async::task<std::expected<signed_in, std::error_code>> finish_sign_in(async::reactor& loop, tls::client_context const& tls_context, gateway::chatgpt_config settings,
                                                                      detail::attempt mine, oauth::callback arrived, std::function<system_clock::time_point()> clock) {
    auto const redirect = detail::redirect_uri(settings);
    auto const client = arrived.client_id.empty() ? mine.client_id : arrived.client_id;
    auto reply = co_await exchange_code(loop, tls_context, settings, detail::code_form(client, arrived.code, mine.verifier, redirect));
    if (!reply) {
        co_return std::unexpected{reply.error()};
    }
    auto held = detail::tokens_from(std::move(*reply), arrived, mine, settings.issuer, mine.client_id, std::chrono::duration_cast<seconds>(clock().time_since_epoch()).count());
    if (!held) {
        co_return std::unexpected{held.error()};
    }
    if (auto const saved = save_tokens(settings.token_file, *held); !saved) {
        co_return std::unexpected{saved.error()};
    }
    co_return signed_in{.account = held->account, .plan_enabled = oauth::grants_plan_use(held->scope)};
}

async::task<std::expected<signed_in, std::error_code>> sign_in(async::reactor& loop, tls::client_context const& tls_context, gateway::chatgpt_config settings,
                                                               sign_in_options options, std::function<system_clock::time_point()> clock) {
    auto bound = net::listener::bind(loop, std::string{detail::loopback_host}, settings.callback_port);
    if (!bound) {
        co_return std::unexpected{make_error_code(errc::port_busy)};
    }
    auto const stored = load_tokens(settings.token_file);
    std::optional<tokens> const previous = stored ? std::optional<tokens>{*stored} : std::nullopt;
    auto mine = detail::new_attempt(previous);
    if (!mine) {
        co_return std::unexpected{mine.error()};
    }
    auto box = std::make_shared<detail::callback_box>();
    loop.spawn(detail::accept_callbacks(std::make_shared<net::listener>(std::move(*bound)),
                                        std::make_shared<detail::callback_handler>(box, mine->state, std::format("{}:{}", detail::loopback_host, settings.callback_port))));
    options.announce(oauth::authorize_url(settings.issuer, {.client_id = mine->client_id, .redirect_uri = detail::redirect_uri(settings), .state = mine->state, .nonce = mine->nonce,
                                                            .code_challenge = oauth::code_challenge(mine->verifier),
                                                            .agent_name_hint = previous ? std::string{} : settings.agent_name, .host_id = mine->host_id}));
    auto arrived = co_await wait_for_callback(loop, box, options.wait);
    if (!arrived) {
        co_return std::unexpected{arrived.error()};
    }
    if (auto const problem = detail::callback_problem(*arrived, *mine, mine->client_id)) {
        co_return std::unexpected{*problem};
    }
    co_return co_await finish_sign_in(loop, tls_context, std::move(settings), std::move(*mine), std::move(*arrived), std::move(clock));
}

}  // namespace llmgate::chatgpt

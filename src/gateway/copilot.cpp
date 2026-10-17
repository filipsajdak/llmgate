module;

#include <cerrno>
#include <cstdio>
#include <cstdlib>

#include <fcntl.h>
#include <unistd.h>

module llmgate.copilot;

import std;
import llmgate.fetch;
import llmgate.json;
import llmgate.process;

namespace llmgate::copilot {

std::expected<void, std::error_code> save_oauth_token(std::string_view const path, std::string_view const token) {
    constexpr mode_t private_file_mode = 0600;
    auto const content = json::write(json::to_value(stored_token{.oauth_token = std::string{token}})) + "\n";
    return process::write_file_atomic(detail::expand_home(path), content, private_file_mode, true);
}

async::task<std::expected<device_code, std::error_code>> request_device_code(async::reactor& loop, tls::client_context const& tls_context,
                                                                             gateway::copilot_config settings) {
    auto spec = detail::github_post("/login/device/code", detail::form({{"client_id", settings.client_id}, {"scope", "read:user"}}));
    auto const answer = co_await fetch::request(loop, tls_context, settings.github_url, std::move(spec), github_limit);
    if (!answer) {
        co_return std::unexpected{make_error_code(errc::github_unreachable)};
    }
    if (answer->status != 200) {
        co_return std::unexpected{make_error_code(errc::github_rejected)};
    }
    co_return detail::parse_reply<device_code>(answer->body);
}

namespace {

std::error_code poll_failure(std::string_view const error) {
    if (error == "authorization_pending") {
        return {};
    }
    return make_error_code(detail::errc_for_github_error(error).value_or(errc::github_rejected));
}

std::optional<std::expected<std::string, std::error_code>> settle_poll(oauth_reply const& reply, milliseconds& interval) {
    if (!reply.access_token.empty()) {
        return reply.access_token;
    }
    if (reply.error == "slow_down") {
        interval += slow_down_step;
        return std::nullopt;
    }
    if (auto const failure = poll_failure(reply.error)) {
        return std::unexpected{failure};
    }
    return std::nullopt;
}

}  // namespace

async::task<std::expected<std::string, std::error_code>> wait_for_authorisation(async::reactor& loop, tls::client_context const& tls_context,
                                                                                gateway::copilot_config settings, device_code code) {
    auto interval = settings.poll_interval.value_or(milliseconds{code.interval * 1000});
    auto const deadline = std::chrono::steady_clock::now() + seconds{code.expires_in};
    while (std::chrono::steady_clock::now() < deadline) {
        co_await loop.sleep(interval);
        auto spec = detail::github_post("/login/oauth/access_token", detail::form({{"client_id", settings.client_id},
                                                                                    {"device_code", code.device_code},
                                                                                    {"grant_type", device_grant}}));
        auto const answer = co_await fetch::request(loop, tls_context, settings.github_url, std::move(spec), github_limit);
        if (!answer) {
            co_return std::unexpected{make_error_code(errc::github_unreachable)};
        }
        auto const reply = detail::parse_reply<oauth_reply>(answer->body);
        if (!reply) {
            co_return std::unexpected{reply.error()};
        }
        if (auto settled = settle_poll(*reply, interval)) {
            co_return std::move(*settled);
        }
    }
    co_return std::unexpected{make_error_code(errc::login_expired)};
}

async::task<std::expected<std::string, std::error_code>> fetch_user(async::reactor& loop, tls::client_context const& tls_context,
                                                                    gateway::copilot_config settings) {
    auto oauth = load_oauth_token(settings.token_file);
    if (!oauth) {
        co_return std::unexpected{oauth.error()};
    }
    http::header_map fields;
    fields.add("Authorization", "token " + *oauth);
    fields.add("Accept", "application/json");
    fields.add("Editor-Version", std::string{editor_version});
    fields.add("User-Agent", "llmgate/0.0");
    auto const answer = co_await fetch::request(loop, tls_context, settings.api_url,
                                                {.method = "GET", .target = std::string{user_target}, .host = {}, .fields = std::move(fields)}, github_limit);
    if (!answer) {
        co_return std::unexpected{make_error_code(errc::github_unreachable)};
    }
    if (answer->status == 401) {
        co_return std::unexpected{make_error_code(errc::login_required)};
    }
    if (answer->status != 200) {
        co_return std::unexpected{make_error_code(errc::github_rejected)};
    }
    co_return answer->body;
}

async::task<std::error_code> check_login(async::reactor& loop, tls::client_context const& tls_context, gateway::copilot_config settings) {
    copilot_credential credential{loop, tls_context, std::move(settings)};
    auto const token = co_await credential.bearer();
    co_return token ? std::error_code{} : token.error();
}

}  // namespace llmgate::copilot

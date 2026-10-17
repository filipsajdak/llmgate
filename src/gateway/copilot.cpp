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
import llmgate.jsonscan;

namespace llmgate::copilot {

std::expected<void, std::error_code> save_oauth_token(std::string_view const path, std::string_view const token) {
    constexpr mode_t private_file_mode = 0600;
    auto const content = R"({"oauth_token":)" + jsonscan::encode_string(token) + "}\n";
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
        if (!reply->access_token.empty()) {
            co_return reply->access_token;
        }
        if (reply->error == "slow_down") {
            interval += slow_down_step;
        } else if (reply->error == "access_denied") {
            co_return std::unexpected{make_error_code(errc::login_denied)};
        } else if (reply->error == "expired_token") {
            co_return std::unexpected{make_error_code(errc::login_expired)};
        } else if (reply->error != "authorization_pending") {
            co_return std::unexpected{make_error_code(errc::github_rejected)};
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

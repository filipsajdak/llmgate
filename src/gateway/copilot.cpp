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
import llmgate.jsonscan;

namespace llmgate::copilot {

namespace {

constexpr mode_t private_file_mode = 0600;

std::error_code write_all(int const fd, std::string_view text) {
    while (!text.empty()) {
        auto const written = ::write(fd, text.data(), text.size());
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return {errno, std::system_category()};
        }
        text.remove_prefix(static_cast<std::size_t>(written));
    }
    return {};
}

}  // namespace

std::expected<void, std::error_code> save_oauth_token(std::string_view const path, std::string_view const token) {
    std::filesystem::path const target{detail::expand_home(path)};
    auto const directory = target.parent_path();
    std::error_code failure;
    if (std::filesystem::create_directories(directory, failure)) {
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, failure);
    }
    auto const temporary = directory / std::format(".{}.{}.tmp", target.filename().string(), ::getpid());
    auto const descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, private_file_mode);
    if (descriptor < 0) {
        return std::unexpected{std::error_code{errno, std::system_category()}};
    }
    auto const content = R"({"oauth_token":)" + jsonscan::encode_string(token) + "}\n";
    auto error = write_all(descriptor, content);
    if (!error && ::fsync(descriptor) < 0) {
        error = {errno, std::system_category()};
    }
    ::close(descriptor);
    if (!error && ::rename(temporary.c_str(), target.c_str()) < 0) {
        error = {errno, std::system_category()};
    }
    if (error) {
        ::unlink(temporary.c_str());
        return std::unexpected{error};
    }
    return {};
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

}  // namespace llmgate::copilot

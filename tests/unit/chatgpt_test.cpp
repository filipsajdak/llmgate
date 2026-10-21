#include <sys/stat.h>
#include <unistd.h>

#include <boost/ut.hpp>

import llmgate.async;
import llmgate.chatgpt;
import llmgate.gateway;
import llmgate.http;
import llmgate.httpio;
import llmgate.net;
import llmgate.oauth;
import llmgate.tls;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate;

using writer = httpio::response_writer<net::tcp_stream>;
using std::chrono::seconds;
using system_clock = std::chrono::system_clock;

constexpr std::string_view loopback = "127.0.0.1";
constexpr std::int64_t start_time = 2'000'000'000;
constexpr std::string_view plan_scopes = "openid offline_access chatgpt.tokens.use.direct";

struct fake_auth {
    int refreshes = 0;
    int status = 200;
    std::string failure{};
    std::string scope = std::string{plan_scopes};
    std::int64_t lifetime = 3600;
    std::vector<std::string> bodies;

    async::task<net::result_void> operator()(httpio::request const& incoming, writer& out) {
        http::header_map fields;
        fields.add("Content-Type", "application/json");
        if (incoming.head.target != "/api/accounts/oauth/token") {
            co_return co_await out.send(404, fields, "{}");
        }
        ++refreshes;
        bodies.push_back(incoming.body);
        if (!failure.empty()) {
            co_return co_await out.send(status, fields, std::format(R"({{"error":"{}"}})", failure));
        }
        co_return co_await out.send(
            200, fields, std::format(R"({{"access_token":"at-{}","refresh_token":"rt-{}","id_token":"it","token_type":"Bearer","expires_in":{},"scope":"{}"}})", refreshes, refreshes, lifetime, scope));
    }
};

struct world {
    async::reactor loop;
    net::listener listener;
    fake_auth auth;
    tls::client_context tls_context;
    gateway::chatgpt_config settings;
    std::filesystem::path directory;
    system_clock::time_point now{seconds{start_time}};

    world()
        : listener{std::move(*net::listener::bind(loop, std::string{loopback}, 0))},
          tls_context{std::move(*tls::client_context::make(tls::group_policy::classical_only))},
          directory{std::filesystem::temp_directory_path() / std::format("llmgate-chatgpt-{}-{}", ::getpid(), reinterpret_cast<std::uintptr_t>(this))} {
        settings = {.token_file = (directory / "token.json").string(), .issuer = std::format("http://127.0.0.1:{}", listener.port())};
        loop.spawn(httpio::serve(listener, auth, httpio::limits{}));
    }

    ~world() { std::filesystem::remove_all(directory); }

    void store(std::int64_t const expires_in, std::string_view const scope = plan_scopes) {
        chatgpt::tokens held{.client_id = "issued_1", .host_id = "urn:uuid:h", .access_token = "at-stored", .refresh_token = "rt-stored", .scope = std::string{scope},
                             .expires_at = start_time + expires_in};
        expect(chatgpt::save_tokens(settings.token_file, held).has_value());
    }

    chatgpt::chatgpt_credential credential() { return chatgpt::chatgpt_credential{loop, tls_context, settings, [this] { return now; }}; }
};

async::task<std::string> bearer_text(chatgpt::chatgpt_credential& credential) {
    auto const token = co_await credential.bearer();
    co_return token ? *token : token.error().message();
}

async::task<void> collect(chatgpt::chatgpt_credential& credential, std::vector<std::string>& results) {
    results.push_back(co_await bearer_text(credential));
}

async::task<std::vector<std::string>> in_parallel(world& w, chatgpt::chatgpt_credential& credential, int const callers) {
    std::vector<std::string> results;
    for (int caller = 0; caller < callers; ++caller) {
        w.loop.spawn(collect(credential, results));
    }
    for (int tick = 0; tick < 2000 && results.size() < static_cast<std::size_t>(callers); ++tick) {
        co_await w.loop.sleep(std::chrono::milliseconds{1});
    }
    co_return results;
}

const suite chatgpt_suite = [] {
    "capability_a_stored_access_token_that_is_still_valid_is_used_without_any_request"_test = [] {
        world w;
        w.store(3600);
        auto credential = w.credential();

        auto const text = w.loop.run(bearer_text(credential));

        expect(text == "at-stored" && w.auth.refreshes == 0);
    };

    "capability_an_expired_token_is_refreshed_once_with_the_stored_refresh_token_and_client_id"_test = [] {
        world w;
        w.store(-10);
        auto credential = w.credential();

        auto const first = w.loop.run(bearer_text(credential));
        auto const second = w.loop.run(bearer_text(credential));

        expect(first == "at-1" && second == "at-1" && w.auth.refreshes == 1);
        expect(w.auth.bodies.at(0).contains("grant_type=refresh_token") && w.auth.bodies.at(0).contains("refresh_token=rt-stored") && w.auth.bodies.at(0).contains("client_id=issued_1"));
    };

    "capability_a_token_close_to_its_end_is_refreshed_before_it_expires"_test = [] {
        world w;
        w.store(30);
        auto credential = w.credential();

        expect(w.loop.run(bearer_text(credential)) == "at-1");
    };

    "capability_the_rotated_tokens_are_saved_together_privately_and_survive_a_restart"_test = [] {
        world w;
        w.store(-10);
        auto credential = w.credential();
        (void)w.loop.run(bearer_text(credential));

        auto const saved = chatgpt::load_tokens(w.settings.token_file);

        expect(saved.has_value() && saved->access_token == "at-1" && saved->refresh_token == "rt-1" && saved->client_id == "issued_1" && saved->host_id == "urn:uuid:h");
        expect(saved->expires_at == start_time + 3600);
        struct stat info{};
        expect(::stat(w.settings.token_file.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);
        auto again = w.credential();
        expect(w.loop.run(bearer_text(again)) == "at-1" && w.auth.refreshes == 1);
    };

    "capability_callers_that_arrive_together_cause_one_refresh"_test = [] {
        world w;
        w.store(-10);
        auto credential = w.credential();

        auto const results = w.loop.run(in_parallel(w, credential, 5));

        expect(results.size() == 5_u && std::ranges::all_of(results, [](std::string const& text) { return text == "at-1"; }));
        expect(w.auth.refreshes == 1);
    };

    "capability_a_token_the_upstream_rejected_is_refreshed_on_the_next_call"_test = [] {
        world w;
        w.store(3600);
        auto credential = w.credential();
        expect(w.loop.run(bearer_text(credential)) == "at-stored");

        credential.rejected();

        expect(w.loop.run(bearer_text(credential)) == "at-1" && w.auth.refreshes == 1);
    };

    "limit_no_token_file_means_login_required_and_no_request"_test = [] {
        world w;
        auto credential = w.credential();

        expect(w.loop.run(bearer_text(credential)) == "login_required" && w.auth.refreshes == 0);
    };

    "limit_an_unusable_refresh_token_means_login_required_and_is_not_tried_again"_test = [] {
        for (std::string_view const code : {"invalid_grant", "refresh_token_reused", "refresh_token_expired", "invalid_refresh_token", "token_expired", "refresh_token_invalidated"}) {
            world w;
            w.store(-10);
            w.auth.status = 400;
            w.auth.failure = std::string{code};
            auto credential = w.credential();

            expect(w.loop.run(bearer_text(credential)) == "login_required") << code;
            expect(w.loop.run(bearer_text(credential)) == "login_required" && w.auth.refreshes == 1) << code;
        }
    };

    "limit_a_temporary_failure_keeps_the_credentials_and_is_tried_again"_test = [] {
        world w;
        w.store(-10);
        w.auth.status = 503;
        w.auth.failure = "temporarily_unavailable";
        auto credential = w.credential();

        expect(w.loop.run(bearer_text(credential)) == "refresh_rejected");
        w.auth.failure.clear();
        expect(w.loop.run(bearer_text(credential)) == "at-2" && w.auth.refreshes == 2);
        expect(chatgpt::load_tokens(w.settings.token_file)->refresh_token == "rt-2");
    };

    "limit_a_reply_without_the_plan_scope_means_plan_use_is_not_enabled"_test = [] {
        world w;
        w.store(-10);
        w.auth.scope = "openid offline_access";
        auto credential = w.credential();

        expect(w.loop.run(bearer_text(credential)) == "plan_not_enabled");
        expect(chatgpt::load_tokens(w.settings.token_file)->refresh_token == "rt-1");
    };

    "limit_stored_tokens_that_lack_the_plan_scope_are_not_used"_test = [] {
        world w;
        w.store(3600, "openid offline_access");
        auto credential = w.credential();

        expect(w.loop.run(bearer_text(credential)) == "plan_not_enabled" && w.auth.refreshes == 0);
    };

    "limit_an_unreachable_sign_in_server_is_reported_and_keeps_the_file"_test = [] {
        world w;
        w.store(-10);
        w.settings.issuer = "http://127.0.0.1:1";
        auto credential = w.credential();

        expect(w.loop.run(bearer_text(credential)) == "auth_unreachable");
        expect(chatgpt::load_tokens(w.settings.token_file)->refresh_token == "rt-stored");
    };

    "limit_a_damaged_token_file_means_login_required"_test = [] {
        world w;
        std::filesystem::create_directories(w.directory);
        std::ofstream{w.settings.token_file} << "{ not json";
        auto credential = w.credential();

        expect(w.loop.run(bearer_text(credential)) == "login_required");
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}

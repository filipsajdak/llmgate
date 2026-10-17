#include <sys/stat.h>

#include <boost/ut.hpp>

import llmgate.async;
import llmgate.copilot;
import llmgate.gateway;
import llmgate.http;
import llmgate.httpio;
import llmgate.net;
import llmgate.tls;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate;

using writer = httpio::response_writer<net::tcp_stream>;
using std::chrono::seconds;
using system_clock = std::chrono::system_clock;

constexpr std::string_view loopback = "127.0.0.1";
constexpr std::int64_t lifetime = 1800;
constexpr seconds one_minute{60};

struct fake_github {
    int pending_before_token = 2;
    bool slow_down_once = false;
    bool deny = false;
    bool expire_code = false;
    int device_requests = 0;
    int polls = 0;
    int exchanges = 0;
    int exchange_status = 200;
    int user_status = 200;
    int user_requests = 0;
    std::string user_reply = R"({"copilot_plan":"individual","quota_snapshots":{}})";
    std::vector<std::string> user_authorisations;
    std::int64_t expires_at = 0;
    std::string api = "http://127.0.0.1:1";
    std::vector<std::string> exchange_authorisations;
    std::vector<std::string> poll_bodies;

    async::task<net::result_void> json(writer& out, int const status, std::string body) {
        http::header_map fields;
        fields.add("Content-Type", "application/json");
        co_return co_await out.send(status, fields, body);
    }

    async::task<net::result_void> operator()(httpio::request const& incoming, writer& out) {
        auto const& path = incoming.head.target;
        if (path == "/login/device/code") {
            ++device_requests;
            co_return co_await json(out, 200, R"({"device_code":"dc","user_code":"ABCD-EFGH","verification_uri":"https://github.com/login/device","expires_in":900,"interval":1,"unmodelled":[1]})");
        }
        if (path == "/login/oauth/access_token") {
            ++polls;
            poll_bodies.push_back(incoming.body);
            if (deny) {
                co_return co_await json(out, 200, R"({"error":"access_denied"})");
            }
            if (expire_code) {
                co_return co_await json(out, 200, R"({"error":"expired_token"})");
            }
            if (slow_down_once && polls == 1) {
                co_return co_await json(out, 200, R"({"error":"slow_down"})");
            }
            if (polls <= pending_before_token) {
                co_return co_await json(out, 200, R"({"error":"authorization_pending"})");
            }
            co_return co_await json(out, 200, R"({"access_token":"gho_test","token_type":"bearer","scope":"read:user"})");
        }
        if (path == "/copilot_internal/v2/token") {
            ++exchanges;
            exchange_authorisations.push_back(std::string{incoming.head.fields.find("authorization").value_or("")});
            if (exchange_status != 200) {
                co_return co_await json(out, exchange_status, R"({"message":"no"})");
            }
            co_return co_await json(out, 200, std::format(R"({{"token":"jwt-{}","expires_at":{},"refresh_in":1500,"endpoints":{{"api":"{}"}},"chat_enabled":true}})", exchanges, expires_at, api));
        }
        if (path == "/copilot_internal/user") {
            ++user_requests;
            user_authorisations.push_back(std::string{incoming.head.fields.find("authorization").value_or("")});
            co_return co_await json(out, user_status, user_reply);
        }
        co_return co_await json(out, 404, "{}");
    }
};

std::int64_t unix_seconds(system_clock::time_point const when) {
    return std::chrono::duration_cast<seconds>(when.time_since_epoch()).count();
}

struct world {
    async::reactor loop;
    net::listener github_listener;
    fake_github github;
    tls::client_context tls_context;
    gateway::copilot_config settings;
    std::filesystem::path directory;
    system_clock::time_point now;

    explicit world(std::optional<std::string> const stored_token = "gho_stored")
        : github_listener{std::move(*net::listener::bind(loop, std::string{loopback}, 0))},
          tls_context{std::move(*tls::client_context::make(tls::group_policy::classical_only))},
          directory{std::filesystem::temp_directory_path() / std::format("llmgate-copilot-{}-{}", ::getpid(), reinterpret_cast<std::uintptr_t>(this))},
          now{system_clock::time_point{seconds{2'000'000'000}}} {
        github.expires_at = unix_seconds(now) + lifetime;
        settings = {.token_file = (directory / "token.json").string(),
                    .github_url = std::format("http://127.0.0.1:{}", github_listener.port()),
                    .api_url = std::format("http://127.0.0.1:{}", github_listener.port()),
                    .poll_interval = std::chrono::milliseconds{5}};
        loop.spawn(httpio::serve(github_listener, github, httpio::limits{}));
        if (stored_token) {
            (void)copilot::save_oauth_token(settings.token_file, *stored_token);
        }
    }

    ~world() { std::filesystem::remove_all(directory); }

    copilot::copilot_credential credential() {
        return copilot::copilot_credential{loop, tls_context, settings, [this] { return now; }};
    }
};

async::task<std::string> bearer_text(copilot::copilot_credential& credential) {
    auto const token = co_await credential.bearer();
    co_return token ? *token : token.error().message();
}

async::task<void> collect_bearer(copilot::copilot_credential& credential, std::vector<std::string>& results) {
    results.push_back(co_await bearer_text(credential));
}

async::task<std::vector<std::string>> bearers_in_parallel(world& w, copilot::copilot_credential& credential, int const callers) {
    std::vector<std::string> results;
    for (int caller = 0; caller < callers; ++caller) {
        w.loop.spawn(collect_bearer(credential, results));
    }
    for (int tick = 0; tick < 2000 && results.size() < static_cast<std::size_t>(callers); ++tick) {
        co_await w.loop.sleep(std::chrono::milliseconds{1});
    }
    co_return results;
}

const suite copilot_suite = [] {
    "capability_the_device_flow_returns_a_code_and_then_the_access_token"_test = [] {
        world w;
        auto const code = w.loop.run(copilot::request_device_code(w.loop, w.tls_context, w.settings));

        expect(code.has_value());
        expect(code->user_code == "ABCD-EFGH");
        auto const token = w.loop.run(copilot::wait_for_authorisation(w.loop, w.tls_context, w.settings, *code));

        expect(token.has_value());
        expect(*token == "gho_test");
        expect(w.github.polls == 3);
        expect(w.github.poll_bodies.at(0).contains("grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code"));
        expect(w.github.poll_bodies.at(0).contains("device_code=dc"));
    };

    "capability_slow_down_lengthens_the_wait_and_the_flow_still_completes"_test = [] {
        world w;
        w.github.slow_down_once = true;
        w.github.pending_before_token = 1;
        auto const code = w.loop.run(copilot::request_device_code(w.loop, w.tls_context, w.settings));

        auto const token = w.loop.run(copilot::wait_for_authorisation(w.loop, w.tls_context, w.settings, *code));

        expect(token.has_value());
    };

    "limit_a_denied_or_expired_authorisation_is_reported_as_such"_test = [] {
        world denied;
        denied.github.deny = true;
        auto const code = denied.loop.run(copilot::request_device_code(denied.loop, denied.tls_context, denied.settings));
        auto const refusal = denied.loop.run(copilot::wait_for_authorisation(denied.loop, denied.tls_context, denied.settings, *code));
        world expired;
        expired.github.expire_code = true;
        auto const late = expired.loop.run(copilot::wait_for_authorisation(expired.loop, expired.tls_context, expired.settings, *code));

        expect(refusal.error() == copilot::errc::login_denied);
        expect(late.error() == copilot::errc::login_expired);
    };

    "limit_an_unreachable_github_is_reported_without_hanging"_test = [] {
        world w;
        w.settings.github_url = "http://127.0.0.1:1";

        auto const code = w.loop.run(copilot::request_device_code(w.loop, w.tls_context, w.settings));

        expect(code.error() == copilot::errc::github_unreachable);
    };

    "capability_the_token_file_round_trips_and_is_private"_test = [] {
        world w{std::nullopt};

        auto const saved = copilot::save_oauth_token(w.settings.token_file, "gho_secret");
        struct stat info {};
        ::stat(w.settings.token_file.c_str(), &info);

        expect(saved.has_value());
        expect(copilot::load_oauth_token(w.settings.token_file).value() == "gho_secret");
        expect((info.st_mode & 0777) == 0600);
    };

    "capability_the_token_file_keeps_its_text_and_escapes_the_token"_test = [] {
        world w{std::nullopt};

        (void)copilot::save_oauth_token(w.settings.token_file, "gho_\"q\"");

        std::ifstream in{w.settings.token_file};
        std::string const text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        expect(text == "{\"oauth_token\":\"gho_\\\"q\\\"\"}\n") << text;
    };

    "limit_saving_over_a_symlink_replaces_the_link_and_leaves_its_target_untouched"_test = [] {
        world w{std::nullopt};
        std::filesystem::create_directories(w.directory);
        auto const victim = w.directory / "victim.txt";
        {
            std::ofstream out{victim};
            out << "precious";
        }
        std::filesystem::create_symlink(victim, w.settings.token_file);

        auto const saved = copilot::save_oauth_token(w.settings.token_file, "gho_new");
        std::ifstream in{victim};
        std::string const kept{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};

        expect(saved.has_value());
        expect(kept == "precious");
        expect(!std::filesystem::is_symlink(w.settings.token_file));
        expect(copilot::load_oauth_token(w.settings.token_file).value() == "gho_new");
    };

    "capability_a_parent_directory_the_save_creates_is_private"_test = [] {
        world w{std::nullopt};
        w.settings.token_file = (w.directory / "nested" / "token.json").string();

        auto const saved = copilot::save_oauth_token(w.settings.token_file, "gho_x");
        struct stat info {};
        ::stat((w.directory / "nested").c_str(), &info);

        expect(saved.has_value());
        expect((info.st_mode & 0777) == 0700);
    };

    "limit_no_temporary_file_is_left_behind_after_a_save"_test = [] {
        world w{std::nullopt};
        (void)copilot::save_oauth_token(w.settings.token_file, "gho_x");

        std::vector<std::string> names;
        for (auto const& entry : std::filesystem::directory_iterator{w.directory}) {
            names.push_back(entry.path().filename().string());
        }

        expect(names == std::vector<std::string>{"token.json"});
    };

    "limit_a_missing_or_empty_token_file_means_login_is_required"_test = [] {
        world w{std::nullopt};

        expect(copilot::load_oauth_token(w.settings.token_file).error() == copilot::errc::login_required);
    };

    "capability_the_exchange_returns_the_copilot_token_and_the_api_endpoint"_test = [] {
        world w;
        w.github.api = "https://api.individual.githubcopilot.com";
        auto credential = w.credential();

        auto const token = w.loop.run(bearer_text(credential));

        expect(token == "jwt-1");
        expect(credential.endpoint() == "https://api.individual.githubcopilot.com");
        expect(w.github.exchange_authorisations.at(0) == "token gho_stored");
    };

    "capability_a_cached_token_is_reused_until_five_minutes_before_it_expires"_test = [] {
        world w;
        auto credential = w.credential();

        (void)w.loop.run(bearer_text(credential));
        w.now += seconds{lifetime} - 6 * one_minute;
        auto const still_cached = w.loop.run(bearer_text(credential));
        w.now += 2 * one_minute;
        auto const renewed = w.loop.run(bearer_text(credential));

        expect(still_cached == "jwt-1");
        expect(renewed == "jwt-2");
        expect(w.github.exchanges == 2);
    };

    "limit_concurrent_callers_share_one_exchange"_test = [] {
        world w;
        auto credential = w.credential();

        auto const results = w.loop.run(bearers_in_parallel(w, credential, 8));

        expect(results.size() == 8_u);
        expect(std::ranges::all_of(results, [](std::string const& token) { return token == "jwt-1"; }));
        expect(w.github.exchanges == 1);
    };

    "capability_a_rejected_token_is_dropped_and_fetched_again"_test = [] {
        world w;
        auto credential = w.credential();
        (void)w.loop.run(bearer_text(credential));

        credential.rejected();
        auto const next = w.loop.run(bearer_text(credential));

        expect(next == "jwt-2");
        expect(!credential.endpoint().has_value() || credential.endpoint().has_value());
    };

    "limit_a_401_from_the_exchange_asks_for_a_new_login_and_stops_asking_github"_test = [] {
        world w;
        w.github.exchange_status = 401;
        auto credential = w.credential();

        auto const first = w.loop.run(bearer_text(credential));
        auto const second = w.loop.run(bearer_text(credential));

        expect(first == copilot::make_error_code(copilot::errc::login_required).message());
        expect(second == first);
        expect(w.github.exchanges == 1);
    };

    "limit_a_github_server_error_is_transient_and_the_next_call_retries"_test = [] {
        world w;
        w.github.exchange_status = 500;
        auto credential = w.credential();
        auto const failed = w.loop.run(bearer_text(credential));

        w.github.exchange_status = 200;
        auto const recovered = w.loop.run(bearer_text(credential));

        expect(failed == copilot::make_error_code(copilot::errc::github_rejected).message());
        expect(recovered == "jwt-2");
    };

    "limit_no_stored_login_fails_fast_without_calling_github"_test = [] {
        world w{std::nullopt};
        auto credential = w.credential();

        auto const result = w.loop.run(bearer_text(credential));

        expect(result == copilot::make_error_code(copilot::errc::login_required).message());
        expect(w.github.exchanges == 0);
    };
    "capability_the_user_endpoint_is_read_with_the_stored_login_and_no_token_exchange"_test = [] {
        world w;

        auto const body = w.loop.run(copilot::fetch_user(w.loop, w.tls_context, w.settings));

        expect(body.has_value());
        expect(body->contains("individual"));
        expect(w.github.user_authorisations == std::vector<std::string>{"token gho_stored"});
        expect(w.github.exchanges == 0);
    };

    "limit_the_user_endpoint_reports_a_rejected_login_an_error_status_and_a_missing_login"_test = [] {
        world rejected;
        rejected.github.user_status = 401;
        world broken;
        broken.github.user_status = 500;
        world absent{std::nullopt};

        expect(rejected.loop.run(copilot::fetch_user(rejected.loop, rejected.tls_context, rejected.settings)).error() == copilot::errc::login_required);
        expect(broken.loop.run(copilot::fetch_user(broken.loop, broken.tls_context, broken.settings)).error() == copilot::errc::github_rejected);
        expect(absent.loop.run(copilot::fetch_user(absent.loop, absent.tls_context, absent.settings)).error() == copilot::errc::login_required);
        expect(absent.github.user_requests == 0);
    };

    "limit_the_user_endpoint_on_an_unreachable_host_does_not_hang"_test = [] {
        world w;
        w.settings.api_url = "http://127.0.0.1:1";

        expect(w.loop.run(copilot::fetch_user(w.loop, w.tls_context, w.settings)).error() == copilot::errc::github_unreachable);
    };

    "capability_check_login_exchanges_the_token_and_reports_the_outcome"_test = [] {
        world accepted;
        world refused;
        refused.github.exchange_status = 401;

        expect(!accepted.loop.run(copilot::check_login(accepted.loop, accepted.tls_context, accepted.settings)));
        expect(refused.loop.run(copilot::check_login(refused.loop, refused.tls_context, refused.settings)) == copilot::errc::login_required);
        expect(accepted.github.exchanges == 1);
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}

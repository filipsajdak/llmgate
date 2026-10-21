#include <sys/stat.h>
#include <unistd.h>

#include <boost/ut.hpp>

import llmgate.async;
import llmgate.fetch;
import llmgate.chatgpt;
import llmgate.gateway;
import llmgate.http;
import llmgate.httpio;
import llmgate.json;
import llmgate.net;
import llmgate.oauth;
import llmgate.proof;
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

std::string access_token_with_client(std::string_view const client) {
    auto const as_bytes = [](std::string_view const value) { return std::span{reinterpret_cast<std::byte const*>(value.data()), value.size()}; };
    auto const claims = json::write(json::make_object({{"client_id", json::make_string(std::string{client})}}));
    return oauth::base64url(as_bytes(R"({"alg":"RS256"})")) + "." + oauth::base64url(as_bytes(claims)) + ".sig";
}

struct fake_auth {
    int refreshes = 0;
    int status = 200;
    std::string failure{};
    std::string scope = std::string{plan_scopes};
    std::int64_t lifetime = 3600;
    std::string id_token = "it";
    std::string access_client{};
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
        auto const text = [](std::string value) { return json::make_string(std::move(value)); };
        co_return co_await out.send(200, fields,
                                    json::write(json::make_object({{"access_token", text(access_client.empty() ? std::format("at-{}", refreshes) : access_token_with_client(access_client))},
                                                                   {"refresh_token", text(std::format("rt-{}", refreshes))},
                                                                   {"id_token", text(id_token)},
                                                                   {"token_type", text("Bearer")},
                                                                   {"expires_in", json::value{lifetime}},
                                                                   {"scope", text(scope)}})));
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


std::string query_of(std::string_view const url, std::string_view const name) {
    auto const question = url.find('?');
    return oauth::query_param(question == std::string_view::npos ? std::string_view{} : url.substr(question + 1), name);
}

std::string id_token_for(std::string_view const issuer, std::string_view const audience, std::string_view const nonce, std::int64_t const exp) {
    auto const text = [](std::string_view const value) { return json::make_string(std::string{value}); };
    auto const claims = json::write(json::make_object({{"iss", text(issuer)}, {"aud", text(audience)}, {"nonce", text(nonce)}, {"exp", json::value{exp}}, {"sub", text("user-1")}}));
    auto const as_bytes = [](std::string_view const value) { return std::span{reinterpret_cast<std::byte const*>(value.data()), value.size()}; };
    return oauth::base64url(as_bytes(R"({"alg":"RS256"})")) + "." + oauth::base64url(as_bytes(claims)) + ".sig";
}

async::task<void> forged_host_visit(world& w, std::string query) {
    auto stream = co_await net::tcp_stream::connect(w.loop, std::string{loopback}, w.settings.callback_port, std::chrono::seconds{5});
    if (!stream) {
        co_return;
    }
    http::header_map fields;
    httpio::request_spec spec{.method = "GET", .target = "/auth/callback?" + query, .host = "evil.example", .fields = std::move(fields)};
    (void)co_await httpio::exchange(*stream, std::move(spec), [](http::event const&) {}, httpio::limits{.read_idle = std::chrono::seconds{5}});
}

async::task<void> browser_visit(world& w, std::string_view const query) {
    (void)co_await fetch::request(w.loop, w.tls_context, std::format("http://127.0.0.1:{}", w.settings.callback_port), {.method = "GET", .target = std::format("/auth/callback?{}", query), .host = {}}, std::chrono::seconds{5});
}

struct sign_in_run {
    std::string url{};
    std::expected<chatgpt::signed_in, std::error_code> result = std::unexpected{std::error_code{}};
};

enum class visit { good, bare, wrong_state, forged_then_good, forged_denial, wrong_host, denied, wrong_nonce, none };

sign_in_run signed_in_with(world& w, visit const how, std::string_view const issued_client = "issued_9", std::chrono::milliseconds const wait = std::chrono::seconds{10}) {
    {
        auto const probe = net::listener::bind(w.loop, std::string{loopback}, 0);
        w.settings.callback_port = probe->port();
    }
    sign_in_run run;
    chatgpt::sign_in_options options{.announce = [&](std::string const& url) {
        run.url = url;
        auto const state = query_of(url, "state");
        auto const nonce = query_of(url, "nonce");
        w.auth.id_token = id_token_for(w.settings.issuer, issued_client, how == visit::wrong_nonce ? "stolen" : nonce, start_time + 600);
        if (how == visit::bare) {
            w.auth.access_client = std::string{issued_client};
        }
        switch (how) {
            case visit::bare: w.loop.spawn(browser_visit(w, std::format("code=the-code&state={}", state))); break;
            case visit::good:
            case visit::wrong_nonce: w.loop.spawn(browser_visit(w, std::format("code=the-code&state={}&client_id={}", state, issued_client))); break;
            case visit::wrong_state: w.loop.spawn(browser_visit(w, std::format("code=the-code&state=forged&client_id={}", issued_client))); break;
            case visit::forged_then_good:
                w.loop.spawn(browser_visit(w, "code=evil&state=forged&client_id=attacker"));
                w.loop.spawn(browser_visit(w, std::format("code=the-code&state={}&client_id={}", state, issued_client)));
                break;
            case visit::forged_denial:
                w.loop.spawn(browser_visit(w, "error=access_denied&state=forged"));
                w.loop.spawn(browser_visit(w, std::format("code=the-code&state={}&client_id={}", state, issued_client)));
                break;
            case visit::wrong_host:
                w.loop.spawn(forged_host_visit(w, std::format("code=evil&state={}&client_id=attacker", state)));
                break;
            case visit::denied: w.loop.spawn(browser_visit(w, std::format("error=access_denied&state={}", state))); break;
            case visit::none: break;
        }
    }, .wait = wait};
    run.result = w.loop.run(chatgpt::sign_in(w.loop, w.tls_context, w.settings, std::move(options), [&w] { return w.now; }));
    return run;
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


    "capability_a_first_sign_in_registers_the_tool_exchanges_the_code_and_saves_the_tokens_privately"_test = [] {
        world w;

        auto const run = signed_in_with(w, visit::good);

        expect(run.result.has_value() && run.result->plan_enabled && run.result->account == "user-1") << (run.result ? "" : run.result.error().message());
        expect(run.url.starts_with(w.settings.issuer + "/api/accounts/authorize?") && query_of(run.url, "client_id") == "dynamic_agent_client");
        expect(query_of(run.url, "agent_name_hint") == "llmgate" && query_of(run.url, "code_challenge_method") == "S256" && query_of(run.url, "ext_agent_host_id").starts_with("urn:uuid:"));
        expect(query_of(run.url, "redirect_uri") == std::format("http://127.0.0.1:{}/auth/callback", w.settings.callback_port));
        expect(w.auth.bodies.size() == 1_u);
        auto const& body = w.auth.bodies.at(0);
        expect(body.contains("grant_type=authorization_code") && body.contains("code=the-code") && body.contains("client_id=issued_9") && body.contains("resource=https%3A%2F%2Fapi.openai.com%2Fv1"));
        auto const verifier = oauth::query_param(body, "code_verifier");
        expect(oauth::code_challenge(verifier) == query_of(run.url, "code_challenge"));
        auto const saved = chatgpt::load_tokens(w.settings.token_file);
        expect(saved.has_value() && saved->client_id == "issued_9" && saved->access_token == "at-1" && saved->refresh_token == "rt-1" && saved->host_id == query_of(run.url, "ext_agent_host_id"));
        struct stat info{};
        expect(::stat(w.settings.token_file.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);
    };

    "capability_a_returning_user_reuses_the_issued_client_and_host_ids_without_a_new_registration"_test = [] {
        world w;
        w.store(3600);

        auto const run = signed_in_with(w, visit::good, "issued_1");

        expect(run.result.has_value());
        expect(query_of(run.url, "client_id") == "issued_1" && query_of(run.url, "agent_name_hint").empty() && query_of(run.url, "ext_agent_host_id") == "urn:uuid:h");
        expect(chatgpt::load_tokens(w.settings.token_file)->host_id == "urn:uuid:h");
    };

    "capability_a_callback_without_a_client_id_falls_back_to_the_access_token_claim_for_a_new_user"_test = [] {
        world w;

        auto const run = signed_in_with(w, visit::bare, "issued_claim");

        expect(run.result.has_value()) << (run.result ? "" : run.result.error().message());
        expect(chatgpt::load_tokens(w.settings.token_file)->client_id == "issued_claim");
    };

    "capability_a_callback_without_a_client_id_keeps_the_known_one_for_a_returning_user"_test = [] {
        world w;
        w.store(3600);

        auto const run = signed_in_with(w, visit::bare, "issued_1");

        expect(run.result.has_value());
        expect(chatgpt::load_tokens(w.settings.token_file)->client_id == "issued_1");
    };

    "limit_a_new_user_whose_callback_and_token_name_no_client_is_refused"_test = [] {
        world w;

        auto const run = signed_in_with(w, visit::good, "");

        expect(!run.result.has_value());
    };

    "capability_the_sign_in_advice_names_the_fix_for_the_common_failures"_test = [] {
        expect(chatgpt::advice(make_error_code(chatgpt::errc::port_busy)).contains("callback_port"));
        expect(chatgpt::advice(make_error_code(chatgpt::errc::sign_in_timeout)).contains("llmgate login"));
        expect(chatgpt::advice(make_error_code(chatgpt::errc::sign_in_denied)).contains("not granted"));
        expect(chatgpt::advice(make_error_code(chatgpt::errc::bad_id_token)) == "bad_id_token");
    };

    "limit_a_callback_with_the_wrong_state_is_ignored_and_the_sign_in_times_out_without_exchanging_anything"_test = [] {
        world w;

        auto const run = signed_in_with(w, visit::wrong_state, "issued_9", std::chrono::milliseconds{400});

        expect(!run.result.has_value() && run.result.error() == make_error_code(chatgpt::errc::sign_in_timeout) && w.auth.refreshes == 0);
        expect(!chatgpt::load_tokens(w.settings.token_file).has_value());
    };

    "capability_a_forged_callback_does_not_stop_the_real_one_from_completing_the_sign_in"_test = [] {
        world w;

        auto const run = signed_in_with(w, visit::forged_then_good);

        expect(run.result.has_value()) << (run.result ? "" : run.result.error().message());
        expect(w.auth.bodies.size() == 1_u && w.auth.bodies.at(0).contains("code=the-code") && !w.auth.bodies.at(0).contains("evil"));
        expect(chatgpt::load_tokens(w.settings.token_file)->client_id == "issued_9");
    };

    "capability_a_forged_denial_without_the_right_state_cannot_abort_the_sign_in"_test = [] {
        world w;

        auto const run = signed_in_with(w, visit::forged_denial);

        expect(run.result.has_value()) << (run.result ? "" : run.result.error().message());
    };

    "limit_a_callback_that_names_another_host_is_refused_even_with_the_right_state"_test = [] {
        world w;

        auto const run = signed_in_with(w, visit::wrong_host, "issued_9", std::chrono::milliseconds{400});

        expect(!run.result.has_value() && run.result.error() == make_error_code(chatgpt::errc::sign_in_timeout) && w.auth.refreshes == 0);
    };

    "limit_a_user_who_denies_access_is_reported_as_denied"_test = [] {
        world w;

        auto const run = signed_in_with(w, visit::denied);

        expect(!run.result.has_value() && run.result.error() == make_error_code(chatgpt::errc::sign_in_denied) && w.auth.refreshes == 0);
    };

    "limit_an_id_token_with_someone_elses_nonce_is_refused_and_nothing_is_saved"_test = [] {
        world w;

        auto const run = signed_in_with(w, visit::wrong_nonce);

        expect(!run.result.has_value() && run.result.error() == make_error_code(chatgpt::errc::bad_id_token));
        expect(!chatgpt::load_tokens(w.settings.token_file).has_value());
    };

    "limit_a_sign_in_that_grants_no_plan_scope_is_saved_but_reported_as_plan_use_not_enabled"_test = [] {
        world w;
        w.auth.scope = "openid offline_access";

        auto const run = signed_in_with(w, visit::good);

        expect(run.result.has_value() && !run.result->plan_enabled);
        expect(chatgpt::load_tokens(w.settings.token_file).has_value());
    };

    "limit_no_callback_before_the_deadline_ends_the_sign_in_with_a_timeout"_test = [] {
        world w;

        auto const run = signed_in_with(w, visit::none, "issued_9", std::chrono::milliseconds{300});

        expect(!run.result.has_value() && run.result.error() == make_error_code(chatgpt::errc::sign_in_timeout));
    };

    "limit_a_callback_port_that_is_already_in_use_is_reported"_test = [] {
        world w;
        auto busy = net::listener::bind(w.loop, std::string{loopback}, 0);
        w.settings.callback_port = busy->port();
        chatgpt::sign_in_options options{.announce = [](std::string const&) {}, .wait = std::chrono::milliseconds{200}};

        auto const result = w.loop.run(chatgpt::sign_in(w.loop, w.tls_context, w.settings, std::move(options), [&w] { return w.now; }));

        expect(!result.has_value() && result.error() == make_error_code(chatgpt::errc::port_busy));
    };

    "limit_a_rejected_code_exchange_is_reported_and_saves_nothing"_test = [] {
        world w;
        w.auth.status = 400;
        w.auth.failure = "invalid_grant";

        auto const run = signed_in_with(w, visit::good);

        expect(!run.result.has_value() && !chatgpt::load_tokens(w.settings.token_file).has_value());
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

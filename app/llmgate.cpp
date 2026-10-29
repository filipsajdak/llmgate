#include <cstdio>
#include <cstdlib>

#include <termios.h>
#include <unistd.h>

import std;
import llmgate.arguments;
import llmgate.async;
import llmgate.aws_credentials;
import llmgate.chatgpt;
import llmgate.check;
import llmgate.claude;
import llmgate.commands;
import llmgate.config_docs;
import llmgate.config_edit;
import llmgate.copilot;
import llmgate.discovery;
import llmgate.model_list;
import llmgate.offer;
import llmgate.text;
import llmgate.gateway;
import llmgate.httpio;
import llmgate.json;
import llmgate.jsonedit;
import llmgate.net;
import llmgate.network;
import llmgate.process;
import llmgate.recipes;
import llmgate.reflect;
import llmgate.routing;
import llmgate.shutdown;
import llmgate.tls;
import llmgate.wizard;

namespace {

using namespace llmgate;

constexpr std::string_view version_text = "llmgate " LLMGATE_VERSION;
constexpr int exit_usage = 2;
constexpr int exit_config = 3;
constexpr int exit_runtime = 1;
constexpr int http_ok = 200;

std::expected<gateway::config, std::string> load_config(std::string_view const path) {
    std::ifstream in{process::expand_home(path)};
    if (!in) {
        return std::unexpected{std::format("cannot read {}", path)};
    }
    std::string const text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    auto loaded = json::read_text<gateway::config>(text);
    if (!loaded) {
        return std::unexpected{std::format("{}: {} ({})", path, loaded.error().message, loaded.error().path)};
    }
    return std::move(*loaded);
}

using credential_store = std::map<std::string, std::unique_ptr<gateway::credential>>;

gateway::secret_readers process_secret_readers() {
    return {.env = [](std::string const& name) -> std::optional<std::string> {
                auto const* value = std::getenv(name.c_str());
                return value == nullptr ? std::nullopt : std::optional<std::string>{value};
            },
            .file = [](std::string const& path) -> std::optional<std::string> {
                std::ifstream in{process::expand_home(path)};
                if (!in) {
                    return std::nullopt;
                }
                return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
            }};
}

std::unique_ptr<gateway::credential> make_credential(async::reactor& loop, tls::client_context const& tls_context, gateway::provider_config const& provider) {
    if (provider.copilot) {
        return std::make_unique<copilot::copilot_credential>(loop, tls_context, *provider.copilot);
    }
    if (provider.chatgpt) {
        return std::make_unique<chatgpt::chatgpt_credential>(loop, tls_context, *provider.chatgpt);
    }
    if (provider.auth.kind == gateway::auth_kind::sigv4) {
        return std::make_unique<aws::aws_credential>(aws::process_sources(), provider.auth.profile, provider.region,
                                                    [] { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); });
    }
    return std::make_unique<gateway::static_credential>(gateway::secret_of(provider, process_secret_readers()));
}

credential_store make_credentials(async::reactor& loop, tls::client_context const& tls_context, gateway::config const& settings) {
    credential_store store;
    for (auto const& [name, provider] : gateway::effective_providers(settings)) {
        store.emplace(name, make_credential(loop, tls_context, provider));
    }
    return store;
}

gateway::credential_map borrowed(credential_store const& store) {
    gateway::credential_map all;
    for (auto const& [name, owned] : store) {
        all.emplace(name, owned.get());
    }
    return all;
}

int fail(int const code, std::string_view const message) {
    std::println(stderr, "llmgate: {}", message);
    return code;
}

void attach_ledger(gateway::proxy& proxy, gateway::config const& settings) {
    auto const sink = commands::ledger_sink(settings.usage.ledger_file);
    if (!sink) {
        return;
    }
    auto history = commands::read_ledger(settings.usage.ledger_file);
    if (history.skipped != 0) {
        std::println(stderr, "llmgate: {} unreadable usage ledger lines were skipped", history.skipped);
    }
    proxy.use_ledger(std::move(history.loaded));
    proxy.persist_usage_with(sink);
}

void attach_capture(gateway::proxy& proxy, gateway::config const& settings) {
    if (auto sink = commands::capture_sink(settings.debug)) {
        std::println(stderr, "llmgate: capturing request and reply bodies to {} (limit {} MiB); the file holds your conversations", settings.debug.capture_file,
                     settings.debug.capture_limit_mib);
        proxy.capture_with(std::move(sink));
    }
}

void report_abandoned(std::size_t const abandoned, std::chrono::milliseconds const drain) {
    if (abandoned != 0) {
        std::println(stderr, "llmgate: stopped with {} requests still running after the {} s drain", abandoned,
                     std::chrono::duration_cast<std::chrono::seconds>(drain).count());
    }
}

std::expected<gateway::reloaded, std::string> reload_config(async::reactor& loop, tls::client_context const& tls_context, std::string const& config_path,
                                                            std::string const& host, std::uint16_t const port, std::deque<credential_store>& kept) {
    return load_config(config_path).and_then([&](gateway::config fresh) {
        return commands::make_policy(fresh, host, port).transform([&](gateway::access_policy policy) {
            auto const& store = kept.emplace_back(make_credentials(loop, tls_context, fresh));
            return gateway::reloaded{.settings = std::move(fresh), .credentials = borrowed(store), .access = std::move(policy)};
        });
    });
}

int serve_on(async::reactor& loop, gateway::config const& settings, std::string const& host, tls::client_context const& tls_context,
             net::listener& listening, std::string const& config_path) {
    auto policy = commands::make_policy(settings, host, listening.port());
    if (!policy) {
        return fail(exit_config, policy.error());
    }
    if (auto const problem = gateway::config_problem(settings); !problem.empty()) {
        return fail(exit_config, problem);
    }
    auto credentials = make_credentials(loop, tls_context, settings);
    gateway::proxy proxy{loop, settings, std::move(*policy), borrowed(credentials), tls_context};
    std::deque<credential_store> reloaded_credentials;
    proxy.reload_with([&] { return reload_config(loop, tls_context, config_path, host, listening.port(), reloaded_credentials); });
    attach_ledger(proxy, settings);
    attach_capture(proxy, settings);
    proxy.use_overrides(commands::read_overrides());
    proxy.persist_overrides_with(commands::overrides_sink());
    auto stop = shutdown::stop_signal::make();
    if (!stop || stop->install()) {
        return fail(exit_runtime, "cannot install the stop signal handlers");
    }
    std::println("llmgate listening on {}:{}", host, listening.port());
    std::fflush(stdout);
    report_abandoned(loop.run(httpio::serve_until(listening, proxy, httpio::limits{}, stop->descriptor(), settings.time.drain)), settings.time.drain);
    commands::release_pid_file();
    std::println("llmgate stopped");
    return 0;
}

int serve(gateway::config settings, std::string const& config_path) {
    auto const address = arguments::split_listen(settings.listen);
    if (!address) {
        return fail(exit_config, "listen must look like 127.0.0.1:9480");
    }
    async::reactor loop;
    auto tls_context = commands::tls_context(commands::dial_of(settings));
    if (!tls_context) {
        return fail(exit_runtime, std::format("cannot start: {}", tls_context.error()));
    }
    auto listening = net::listener::bind(loop, address->host, address->port);
    if (!listening) {
        return fail(exit_runtime, std::format("cannot start: {}", listening.error().message()));
    }
    return serve_on(loop, settings, address->host, *tls_context, *listening, config_path);
}

void emit_event(commands::login_event const& item) {
    std::println("{}", commands::login_event_line(item));
    std::fflush(stdout);
}

int login_failed(bool const json, int const code, std::string const& message) {
    if (!json) {
        return fail(code, message);
    }
    emit_event({.event = "failed", .detail = message});
    return code;
}

int save_login(gateway::copilot_config const& target, std::string const& token, bool const json) {
    if (auto const saved = copilot::save_oauth_token(target.token_file, token); !saved) {
        return login_failed(json, exit_runtime, std::format("cannot save the token: {}", saved.error().message()));
    }
    if (json) {
        emit_event({.event = "done", .detail = std::format("the token is stored in {}", target.token_file)});
    } else {
        std::println("Logged in. The token is stored in {}", target.token_file);
    }
    return 0;
}

void announce_code(copilot::device_code const& code, bool const json) {
    if (json) {
        emit_event({.event = "link", .url = code.verification_uri, .code = code.user_code});
        return;
    }
    std::println("Open {} and enter the code {}", code.verification_uri, code.user_code);
    std::println("Waiting for you to authorise...");
    std::fflush(stdout);
}

int await_login(async::reactor& loop, tls::client_context const& tls_context, gateway::copilot_config const& target, copilot::device_code const& code, bool const json) {
    announce_code(code, json);
    auto const token = loop.run(copilot::wait_for_authorisation(loop, tls_context, target, code));
    if (!token) {
        return login_failed(json, exit_runtime, std::format("login failed: {}", token.error().message()));
    }
    return save_login(target, *token, json);
}

int request_login(async::reactor& loop, tls::client_context const& tls_context, gateway::copilot_config const& target, bool const json) {
    auto const code = loop.run(copilot::request_device_code(loop, tls_context, target));
    if (!code) {
        return login_failed(json, exit_runtime, std::format("cannot reach GitHub: {}", code.error().message()));
    }
    return await_login(loop, tls_context, target, *code, json);
}

int login_with(gateway::config const& settings, gateway::copilot_config const& target, bool const json) {
    async::reactor loop;
    auto tls_context = commands::tls_context(commands::dial_of(settings));
    if (!tls_context) {
        return fail(exit_runtime, tls_context.error());
    }
    return request_login(loop, *tls_context, target, json);
}

constexpr std::string_view no_browser_variable = "LLMGATE_NO_BROWSER";

void open_in_browser(std::string const& url) {
#if defined(__APPLE__)
    if (std::getenv(no_browser_variable.data()) == nullptr) {
        (void)process::run_and_wait("/usr/bin/open", {url});
    }
#else
    static_cast<void>(url);
#endif
}

void announce_browser(std::string const& url, bool const json) {
    if (json) {
        emit_event({.event = "link", .url = url});
    } else {
        std::println("Sign in with ChatGPT in your browser (it opens by itself on macOS); if it does not, open this address:\n\n  {}\n", url);
        std::println("Waiting for the browser to come back...");
        std::fflush(stdout);
    }
    open_in_browser(url);
}

int chatgpt_sign_in(gateway::config const& settings, gateway::chatgpt_config const& target, bool const json) {
    async::reactor loop;
    auto tls_context = commands::tls_context(commands::dial_of(settings));
    if (!tls_context) {
        return fail(exit_runtime, tls_context.error());
    }
    chatgpt::sign_in_options options{.announce = [json](std::string const& url) { announce_browser(url, json); }};
    auto const done = loop.run(chatgpt::sign_in(loop, *tls_context, target, std::move(options)));
    if (!done) {
        return login_failed(json, exit_runtime, std::format("sign-in failed: {}", chatgpt::advice(done.error())));
    }
    if (!done->plan_enabled) {
        return login_failed(json, exit_runtime, "ChatGPT plan use is not enabled for this account: enable it in ChatGPT settings or use an OpenAI API key instead");
    }
    if (json) {
        emit_event({.event = "done", .detail = std::format("signed in as {}; tokens saved to {}", done->account, target.token_file)});
    } else {
        std::println("Signed in as {}. Tokens saved to {}.", done->account, target.token_file);
    }
    return 0;
}

int login(gateway::config const& settings, std::string_view const provider, bool const json) {
    auto const target = commands::login_choice(settings, provider);
    if (!target) {
        return login_failed(json, exit_config, target.error());
    }
    if (auto const* chatgpt_target = std::get_if<gateway::chatgpt_config>(&*target)) {
        return chatgpt_sign_in(settings, *chatgpt_target, json);
    }
    return login_with(settings, std::get<gateway::copilot_config>(*target), json);
}

std::expected<std::string, std::string> fetch_listing(commands::dial_settings const& dial, gateway::provider_config const& provider) {
    async::reactor loop;
    auto tls_context = commands::tls_context(dial);
    if (!tls_context) {
        return std::unexpected{std::format("cannot start TLS: {}", tls_context.error())};
    }
    auto const credential = make_credential(loop, *tls_context, provider);
    auto const listed = loop.run(discovery::fetch_models(loop, *tls_context, provider, *credential));
    if (!listed) {
        return std::unexpected{listed.error()};
    }
    if (listed->status != http_ok) {
        return std::unexpected{std::format("the provider answered {}: {}", listed->status, model_list::printable(listed->body, model_list::longest_text))};
    }
    return listed->body;
}

commands::model_lister provider_lister(commands::dial_settings dial) {
    return [dial = std::move(dial)](std::string const&, gateway::provider_config const& provider) -> std::expected<std::vector<model_list::entry>, std::string> {
        auto const body = fetch_listing(dial, provider);
        if (!body) {
            return std::unexpected{body.error()};
        }
        return model_list::parse(*body);
    };
}

bool write_atomically(std::filesystem::path const& target, std::string_view const text) {
    auto const temporary = std::filesystem::path{target}.concat(".new");
    {
        std::ofstream out{temporary, std::ios::binary | std::ios::trunc};
        out << text;
        if (!out) {
            return false;
        }
    }
    std::error_code failed;
    std::filesystem::permissions(temporary, std::filesystem::status(target, failed).permissions(), failed);
    std::filesystem::rename(temporary, target, failed);
    return !failed;
}

std::expected<std::string, std::string> offers_of_options(arguments::options const& chosen) {
    auto offer = chosen.offer;
    if (chosen.offer_from.empty()) {
        return offer;
    }
    std::ifstream aliases{process::expand_home(chosen.offer_from)};
    if (!aliases) {
        return std::unexpected{std::format("cannot read {}", chosen.offer_from)};
    }
    for (auto const& name : config_edit::alias_names(std::string{std::istreambuf_iterator<char>{aliases}, std::istreambuf_iterator<char>{}}, chosen.offer_filter)) {
        offer += (offer.empty() ? "" : ",") + name;
    }
    return offer;
}

std::expected<config_edit::change, std::string> planned_edit(arguments::options const& chosen) {
    auto const group = reflect::enum_from_name<config_edit::group>(reflect::enum_name(chosen.which));
    auto offer = offers_of_options(chosen);
    if (!group || !offer) {
        return std::unexpected{group ? offer.error() : std::string{"not an edit command"}};
    }
    return config_edit::plan({.which = *group,
                              .words = chosen.passthrough,
                              .preset = chosen.preset,
                              .labels = chosen.labels,
                              .base_url = chosen.base_url,
                              .token_env = chosen.token_env,
                              .auth = chosen.auth,
                              .secret = chosen.secret,
                              .ghe_host = chosen.ghe_host,
                              .offer = std::move(*offer),
                              .description = chosen.description,
                              .prompt = chosen.prompt});
}

void refresh_picker(commands::context const& where, std::filesystem::path const& config_file) {
    auto fresh = load_config(config_file.string());
    if (!fresh) {
        return;
    }
    auto changed = where;
    changed.settings = std::move(*fresh);
    if (!commands::refresh_profile(changed)) {
        std::println("the /model list in the llmgate profile could not be refreshed; run llmgate setup");
    }
}

int edit_command(commands::context const& where) {
    auto const& chosen = where.options;
    auto const planned = planned_edit(chosen);
    if (!planned) {
        return fail(exit_usage, planned.error());
    }
    auto const path = std::filesystem::path{process::expand_home(chosen.config_path)};
    std::ifstream in{path};
    std::string const text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    auto const edited = config_edit::apply(text, *planned);
    if (!edited) {
        return fail(exit_config, edited.error());
    }
    std::print("{}", edited->diff);
    if (!chosen.yes) {
        std::println("not written: add --yes to write {}", path.string());
        return 0;
    }
    if (!write_atomically(path, edited->text)) {
        return fail(exit_runtime, std::format("cannot write {}", path.string()));
    }
    std::println("written to {}", path.string());
    refresh_picker(where, path);
    if (where.reload_after_edit && commands::run_reload(where) != 0) {
        std::println("the gateway did not reload (is it running?); it picks the change up when it starts");
    }
    return 0;
}

std::string read_secret_line(std::string_view const name) {
    bool const interactive = ::isatty(STDIN_FILENO) != 0;
    termios saved{};
    if (interactive) {
        std::print(stderr, "Secret for {} (hidden, Enter to finish): ", name);
        std::fflush(stderr);
        ::tcgetattr(STDIN_FILENO, &saved);
        termios hidden = saved;
        hidden.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        ::tcsetattr(STDIN_FILENO, TCSANOW, &hidden);
    }
    std::string line;
    char c = 0;
    while (::read(STDIN_FILENO, &c, 1) == 1 && c != '\n') {
        line.push_back(c);
    }
    if (interactive) {
        ::tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        std::println(stderr, "");
    }
    line.erase(std::ranges::find(line, '\r'), line.end());
    return line;
}

std::expected<std::string, std::string> store_secret(std::string_view const name, std::string_view const value) {
    auto const location = config_edit::secret_file(name);
    if (!location) {
        return std::unexpected{location.error()};
    }
    if (auto const stored = process::write_file_atomic(process::expand_home(*location), std::string{value} + "\n", 0600, true); !stored) {
        return std::unexpected{std::format("cannot store the secret: {}", stored.error().message())};
    }
    return *location;
}

int secret_command(commands::context const& where) {
    auto const& words = where.options.passthrough;
    if (words.size() != 2 || words.front() != "set") {
        return fail(exit_usage, "usage: llmgate secret set NAME (the value is read from the keyboard, hidden, or from a pipe)");
    }
    auto const value = read_secret_line(words.back());
    if (value.empty()) {
        return fail(exit_usage, "no value was given; nothing was stored");
    }
    auto const location = store_secret(words.back(), value);
    if (!location) {
        return fail(exit_usage, location.error());
    }
    std::println("stored as {} (mode 0600); use it with --secret file:{}", *location, *location);
    return 0;
}

std::string recipe_text(recipes::recipe const& item) {
    std::string out = std::format("{}  {}\n  {}\n  needs: ", item.id, item.title, item.summary);
    for (auto const kind : item.needs) {
        out += std::format("{} ", recipes::name_of(kind));
    }
    out += std::format("  tokens: {}\n", item.tokens);
    for (auto const& pro : item.pros) {
        out += std::format("  + {}\n", pro);
    }
    for (auto const& con : item.cons) {
        out += std::format("  - {}\n", con);
    }
    return out;
}

constexpr std::string_view recipe_usage = "usage: llmgate recipe list [--json] | recipe show NAME | recipe apply NAME [--yes]";

int report_missing(recipes::recipe const& item, std::vector<std::string> const& missing) {
    std::print(stderr, "llmgate: the recipe {} needs providers of kind", item.id);
    for (auto const& kind : missing) {
        std::print(stderr, " {}", kind);
    }
    std::println(stderr, " (add them with provider add)");
    return exit_config;
}

std::expected<std::string, std::string> edited_text(std::string text, std::vector<config_edit::change> const& edits) {
    for (auto const& step : edits) {
        auto const done = config_edit::apply(text, step);
        if (!done) {
            return std::unexpected{done.error()};
        }
        text = done->text;
    }
    return text;
}

int commit_edits(commands::context const& where, std::vector<config_edit::change> const& edits) {
    auto const path = std::filesystem::path{process::expand_home(where.options.config_path)};
    std::ifstream in{path};
    std::string const before{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    auto const text = edited_text(before, edits);
    if (!text) {
        return fail(exit_config, text.error());
    }
    std::print("{}", jsonedit::diff(before, *text));
    if (!where.options.yes) {
        std::println("not written: add --yes to write {}", path.string());
        return 0;
    }
    if (!write_atomically(path, *text)) {
        return fail(exit_runtime, std::format("cannot write {}", path.string()));
    }
    std::println("written to {}", path.string());
    refresh_picker(where, path);
    if (where.reload_after_edit && commands::run_reload(where) != 0) {
        std::println("the gateway did not reload (is it running?); it picks the change up when it starts");
    }
    return 0;
}

int apply_recipe(commands::context const& where, recipes::recipe const& item) {
    auto const planned = recipes::plan(item, where.settings);
    if (!planned.missing.empty()) {
        return report_missing(item, planned.missing);
    }
    auto const changes = planned.edits | std::views::transform([](recipes::edit const& step) { return config_edit::change{.path = step.path, .value = step.value}; }) | std::ranges::to<std::vector>();
    return commit_edits(where, changes);
}

int list_recipes(commands::context const& where) {
    if (where.options.as_json) {
        std::println("{}", recipes::list_json());
        return 0;
    }
    for (auto const& item : recipes::all()) {
        std::print("{}\n", recipe_text(item));
    }
    return 0;
}

int recipe_command(commands::context const& where) {
    auto const& words = where.options.passthrough;
    if (words.size() == 1 && words.front() == "list") {
        return list_recipes(where);
    }
    auto const item = words.size() == 2 ? recipes::find(words.back()) : std::nullopt;
    if (!item || !std::ranges::contains(std::array<std::string_view, 2>{"show", "apply"}, words.front())) {
        return fail(exit_usage, recipe_usage);
    }
    if (words.front() == "show") {
        std::print("{}", recipe_text(*item));
        return 0;
    }
    return apply_recipe(where, *item);
}

check::outcome skipped_outcome(check::job const& item) {
    return {.provider = item.provider, .step = "all", .skipped = true, .detail = item.skipped};
}

check::outcome run_step(commands::context const& where, check::job const& item, check::step const which) {
    auto const began = std::chrono::steady_clock::now();
    auto const answer = commands::gateway_request(where, "/anthropic/v1/messages", check::request_body(which, item.target), check::headers_of(which));
    auto const took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
    return {.provider = item.provider,
            .step = check::step_name(which),
            .detail = answer ? check::judge(which, answer->status, answer->body) : answer.error(),
            .milliseconds = took};
}

std::vector<check::outcome> run_job(commands::context const& where, check::job const& item, std::span<check::step const> const steps) {
    if (!item.skipped.empty()) {
        return {skipped_outcome(item)};
    }
    std::vector<check::outcome> rows;
    for (auto const which : steps) {
        rows.push_back(run_step(where, item, which));
        if (!rows.back().detail.empty()) {
            break;
        }
    }
    return rows;
}

std::vector<check::outcome> check_all(commands::context const& where, std::vector<check::job> const& jobs) {
    auto const& chosen = where.options;
    auto const provider_steps = chosen.quick ? std::vector{check::step::ping} : std::vector{check::step::ping, check::step::tool, check::step::context};
    std::vector<check::outcome> rows;
    for (auto const& item : jobs) {
        std::ranges::move(run_job(where, item, provider_steps), std::back_inserter(rows));
    }
    if (chosen.provider.empty() && !chosen.quick) {
        for (auto const& item : check::agent_jobs(where.settings)) {
            std::ranges::move(run_job(where, item, std::array{check::step::agent}), std::back_inserter(rows));
        }
    }
    return rows;
}

int check_command(commands::context const& where) {
    auto const& chosen = where.options;
    auto const jobs = check::plan(where.settings, chosen.provider);
    if (jobs.empty()) {
        return fail(exit_config, chosen.provider.empty() ? "no provider is configured" : std::format("no provider named {}", chosen.provider));
    }
    auto const rows = check_all(where, jobs);
    std::print("{}", chosen.as_json ? check::to_json(rows) + "\n" : check::render(rows));
    return check::all_passed(rows) ? 0 : exit_runtime;
}

constexpr std::string_view offer_usage = "usage: llmgate models offer PROVIDER MODEL... | all  (llmgate models --provider PROVIDER lists the models)";
constexpr std::string_view offer_all = "all";

std::expected<std::vector<model_list::entry>, std::string> listed_models(commands::context const& where, gateway::provider_config const& provider) {
    auto const body = fetch_listing(commands::dial_of(where.settings), provider);
    if (!body) {
        return std::unexpected{body.error()};
    }
    return model_list::parse(*body);
}

std::vector<std::string> offered_ids(std::span<std::string const> const words, std::vector<model_list::entry> const& listed, gateway::wire_protocol const provider) {
    if (words.size() == 1 && words.front() == offer_all) {
        return listed | std::views::filter([&](model_list::entry const& each) { return offer::recommended(each, provider); }) | std::views::transform(&model_list::entry::id) |
               std::ranges::to<std::vector>();
    }
    return {words.begin(), words.end()};
}

int offer_command(commands::context const& where) {
    auto const& words = where.options.passthrough;
    if (words.size() < 3) {
        return fail(exit_usage, offer_usage);
    }
    auto const chosen = commands::models_choice(where.settings, words[1]);
    if (!chosen) {
        return fail(exit_config, chosen.error());
    }
    auto const listed = listed_models(where, chosen->second);
    if (!listed) {
        return fail(exit_runtime, listed.error());
    }
    auto const planned = offer::plan(chosen->first, chosen->second, *listed, offered_ids(std::span{words}.subspan(2), *listed, chosen->second.protocol));
    return planned ? commit_edits(where, *planned) : fail(exit_config, planned.error());
}

int print_listing(commands::context const& where, std::string const& body) {
    if (where.options.as_json) {
        auto const document = json::parse(body, {.jsonc = false});
        if (!document) {
            return fail(exit_runtime, "the provider's reply is not JSON");
        }
        std::println("{}", json::write(*document));
        return 0;
    }
    auto const models = model_list::parse(body);
    if (!models) {
        return fail(exit_runtime, models.error());
    }
    std::print("{}", model_list::render(*models));
    return 0;
}

constexpr std::string_view list_usage = "usage: llmgate models list PROVIDER [--json]";

std::string candidates_text(std::vector<offer::candidate> const& rows) {
    std::string out;
    for (auto const& row : rows) {
        text::append_all(out, row.offered ? "[x] " : "[ ] ", model_list::printable(row.id, model_list::longest_text), "  ", row.protocol, "  context ", text::decimal(row.context), "  ", row.policy, row.recommended ? "  recommended" : "", "\n");
    }
    return out;
}

int list_command(commands::context const& where) {
    auto const& words = where.options.passthrough;
    if (words.size() != 2) {
        return fail(exit_usage, list_usage);
    }
    auto const chosen = commands::models_choice(where.settings, words[1]);
    if (!chosen) {
        return fail(exit_config, chosen.error());
    }
    auto const listed = listed_models(where, chosen->second);
    if (!listed) {
        return fail(exit_runtime, listed.error());
    }
    auto const rows = offer::candidates(chosen->second, *listed);
    std::print("{}", where.options.as_json ? offer::candidates_json(rows) + "\n" : candidates_text(rows));
    return 0;
}

int models_subcommand(commands::context const& where) {
    auto const& first = where.options.passthrough.front();
    if (first == "offer") {
        return offer_command(where);
    }
    return first == "list" ? list_command(where) : edit_command(where);
}

int models_command(commands::context const& where) {
    if (!where.options.passthrough.empty()) {
        return models_subcommand(where);
    }
    auto const chosen = commands::models_choice(where.settings, where.options.provider);
    if (!chosen) {
        return fail(exit_config, chosen.error());
    }
    auto const body = fetch_listing(commands::dial_of(where.settings), chosen->second);
    return body ? print_listing(where, *body) : fail(exit_runtime, body.error());
}

constexpr std::string_view init_interrupted = "the input ended before init finished; what was written stays, run llmgate init again to continue";
constexpr std::string_view init_greeting = "llmgate init: connect Claude Code to the providers you have.\nWhich of these do you have? Type numbers separated by commas.\n";
constexpr std::size_t question_attempts = 3;

std::optional<std::string> read_line_from_input() {
    std::string line;
    char c = 0;
    bool any = false;
    while (::read(STDIN_FILENO, &c, 1) == 1) {
        any = true;
        if (c == '\n') {
            break;
        }
        line.push_back(c);
    }
    if (!any) {
        return std::nullopt;
    }
    line.erase(std::ranges::find(line, '\r'), line.end());
    return line;
}

std::optional<std::string> ask_text(std::string_view const question, std::string_view const fallback) {
    std::print("{}{} ", question, fallback.empty() ? std::string{} : std::format(" [{}]", fallback));
    std::fflush(stdout);
    auto line = read_line_from_input();
    if (!line) {
        return std::nullopt;
    }
    return line->empty() ? std::string{fallback} : std::move(*line);
}

std::optional<bool> ask_yes_no(std::string_view const question, bool const fallback) {
    for (std::size_t attempt = 0; attempt < question_attempts; ++attempt) {
        auto const line = ask_text(std::format("{} (y/n)", question), fallback ? "y" : "n");
        if (!line) {
            return std::nullopt;
        }
        if (auto const answer = wizard::yes_no(*line, fallback)) {
            return answer;
        }
        std::println("please answer y or n");
    }
    return fallback;
}

std::optional<std::vector<wizard::kind>> ask_kinds() {
    std::print("{}{}", init_greeting, wizard::menu());
    for (std::size_t attempt = 0; attempt < question_attempts; ++attempt) {
        auto const line = ask_text("Your choice", "");
        if (!line) {
            return std::nullopt;
        }
        auto chosen = wizard::choose(*line);
        if (chosen) {
            return std::move(*chosen);
        }
        std::println("{}", chosen.error());
    }
    return std::nullopt;
}

std::string auth_for_choice(std::string_view const choice) {
    if (choice == "2") {
        return "header:api-key";
    }
    return choice == "3" ? std::string{reflect::enum_name(gateway::auth_kind::none)} : std::string{reflect::enum_name(gateway::auth_kind::bearer)};
}

std::optional<std::string> ask_key_into_file(std::string_view const name) {
    auto const value = read_secret_line(name);
    if (value.empty()) {
        std::println("no key was given");
        return std::nullopt;
    }
    auto const location = store_secret(name, value);
    if (!location) {
        std::println("{}", location.error());
        return std::nullopt;
    }
    return "file:" + *location;
}

std::optional<wizard::answers> ask_host(wizard::answers given) {
    if (given.which != wizard::kind::copilot) {
        return given;
    }
    auto host = ask_text("GitHub host for an enterprise account (blank for github.com)", "");
    if (!host) {
        return std::nullopt;
    }
    given.host = std::move(*host);
    return given;
}

std::optional<wizard::answers> ask_url(wizard::answers given) {
    if (!wizard::asks_for_url(given.which)) {
        return given;
    }
    auto url = ask_text("Address of the server", given.which == wizard::kind::local ? wizard::default_local_url : "");
    if (!url) {
        return std::nullopt;
    }
    given.base_url = std::move(*url);
    if (given.which == wizard::kind::local) {
        given.auth = std::string{reflect::enum_name(gateway::auth_kind::none)};
    }
    return given;
}

std::optional<wizard::answers> ask_auth(wizard::answers given) {
    if (given.which != wizard::kind::gateway) {
        return given;
    }
    auto const choice = ask_text("How does it expect the key? 1) Authorization: Bearer  2) api-key header  3) no key", "1");
    if (!choice) {
        return std::nullopt;
    }
    given.auth = auth_for_choice(*choice);
    if (wizard::takes_a_key(given)) {
        given.secret = ask_key_into_file(given.name).value_or("");
        if (given.secret.empty()) {
            return std::nullopt;
        }
    }
    return given;
}

std::optional<wizard::answers> ask_answers(wizard::kind const which, std::string name) {
    auto host = ask_host({.which = which, .name = std::move(name)});
    auto url = host ? ask_url(std::move(*host)) : std::nullopt;
    return url ? ask_auth(std::move(*url)) : std::nullopt;
}

std::filesystem::path config_path_of(commands::context const& where) {
    return std::filesystem::path{process::expand_home(where.options.config_path)};
}

bool ensure_config_file(commands::context const& where) {
    auto const path = config_path_of(where);
    std::error_code failed;
    if (std::filesystem::exists(path, failed)) {
        return true;
    }
    return process::write_file_atomic(path, std::string{wizard::minimal_config}, 0600, true).has_value();
}

commands::context reloaded(commands::context where) {
    if (auto fresh = load_config(where.options.config_path)) {
        where.settings = std::move(*fresh);
        where.config_error.clear();
    }
    return where;
}

bool provider_exists(commands::context const& where, std::string const& name) {
    return where.settings.providers.contains(name);
}

int add_provider(commands::context& where, wizard::answers const& given) {
    auto edits = std::vector<config_edit::change>{};
    auto planned = config_edit::plan(wizard::provider_request(given));
    if (!planned) {
        return fail(exit_config, planned.error());
    }
    edits.push_back(std::move(*planned));
    if (auto const credential = wizard::credential_edit(given.which)) {
        edits.push_back(*credential);
    }
    if (auto const done = commit_edits(where, edits); done != 0) {
        return done;
    }
    where = reloaded(std::move(where));
    return 0;
}

void sign_in_after_add(commands::context const& where, wizard::answers const& given) {
    if (given.which == wizard::kind::claude) {
        std::println("Start Claude Code with: llmgate claude, then type /login once and pick your Claude account.");
        return;
    }
    if (wizard::signs_in(given.which) && login(where.settings, given.name, false) != 0) {
        std::println("Sign-in did not finish; run: llmgate login --provider {}", given.name);
    }
}

void offer_after_add(commands::context where, wizard::answers const& given) {
    if (!wizard::can_list_models(given.which)) {
        return;
    }
    auto const wanted = ask_yes_no(std::format("Offer the recommended models of {} in Claude Code's /model list?", given.name), true);
    if (!wanted.value_or(false)) {
        return;
    }
    where.options.passthrough = {"offer", given.name, "all"};
    if (offer_command(where) != 0) {
        std::println("The models could not be listed now; later run: llmgate models offer {} all", given.name);
    }
}

std::optional<int> add_one(commands::context& where, wizard::kind const which) {
    auto name = ask_text(std::format("Name for this {} provider", wizard::default_name(which)), wizard::default_name(which));
    if (!name) {
        return std::nullopt;
    }
    if (provider_exists(where, *name)) {
        std::println("A provider named {} already exists; skipping it.", *name);
        return 0;
    }
    auto given = ask_answers(which, std::move(*name));
    if (!given) {
        return std::nullopt;
    }
    if (add_provider(where, *given) != 0) {
        return 0;
    }
    sign_in_after_add(where, *given);
    offer_after_add(where, *given);
    return 0;
}

std::vector<recipes::recipe> applicable_recipes(gateway::config const& settings) {
    return recipes::all() | std::views::filter([&settings](recipes::recipe const& item) { return recipes::plan(item, settings).missing.empty(); }) | std::ranges::to<std::vector>();
}

std::optional<std::size_t> ask_recipe(std::vector<recipes::recipe> const& choices) {
    std::println("Pick a setup for how the models are used (0 to keep things as they are):");
    for (std::size_t index = 0; index < choices.size(); ++index) {
        std::println("  {}) {}  {}", index + 1, choices[index].id, choices[index].summary);
    }
    auto const line = ask_text("Your choice", "1");
    if (!line) {
        return std::nullopt;
    }
    std::size_t number = 0;
    auto const parsed = std::from_chars(line->data(), line->data() + line->size(), number);
    if (parsed.ec != std::errc{} || number > choices.size()) {
        std::println("not a choice; keeping things as they are");
        return 0;
    }
    return number;
}

std::optional<int> recipe_step(commands::context& where) {
    auto const choices = applicable_recipes(where.settings);
    if (choices.empty()) {
        return 0;
    }
    auto const picked = ask_recipe(choices);
    if (!picked) {
        return std::nullopt;
    }
    if (*picked != 0 && apply_recipe(where, choices[*picked - 1]) == 0) {
        where = reloaded(std::move(where));
    }
    return 0;
}

int finish_init(commands::context const& where) {
    if (commands::run_setup(where) != 0) {
        return exit_runtime;
    }
    std::println("Check everything with: llmgate doctor    Add more later with: llmgate init");
    return 0;
}

int init_interactive(commands::context where) {
    auto const kinds = ask_kinds();
    if (!kinds) {
        return fail(exit_runtime, init_interrupted);
    }
    for (auto const which : *kinds) {
        if (!add_one(where, which)) {
            return fail(exit_runtime, init_interrupted);
        }
    }
    if (!recipe_step(where)) {
        return fail(exit_runtime, init_interrupted);
    }
    return finish_init(where);
}

int init_unattended(commands::context where) {
    if (!where.options.recipe.empty()) {
        auto const item = recipes::find(where.options.recipe);
        if (!item) {
            return fail(exit_usage, std::format("no recipe named {} (llmgate recipe list)", where.options.recipe));
        }
        if (apply_recipe(where, *item) != 0) {
            return exit_config;
        }
        where = reloaded(std::move(where));
    }
    if (where.settings.providers.empty()) {
        std::println("No providers yet: run llmgate init on a terminal, or llmgate provider add.");
    }
    return finish_init(where);
}

int init_command(commands::context const& given) {
    auto where = given;
    where.options.yes = true;
    where.reload_after_edit = false;
    if (!ensure_config_file(where)) {
        return fail(exit_runtime, std::format("cannot create {}", config_path_of(where).string()));
    }
    where = reloaded(std::move(where));
    return given.options.yes ? init_unattended(std::move(where)) : init_interactive(std::move(where));
}

int serve_command(commands::context const& where) {
    return serve(where.settings, where.options.config_path);
}

int login_command(commands::context const& where) {
    if (where.options.detach) {
        return commands::run_login_detached(where);
    }
    if (where.options.progress) {
        return commands::run_login_progress(where);
    }
    return login(where.settings, where.options.provider, where.options.as_json);
}

int statusline_command(commands::context const& where) {
    return commands::run_statusline(where, process::read_all(STDIN_FILENO).value_or(std::string{}));
}

using command_handler = int (*)(commands::context const&);

constexpr auto handlers = std::to_array<std::pair<arguments::command, command_handler>>({
    {arguments::command::serve, &serve_command},
    {arguments::command::login, &login_command},
    {arguments::command::claude, &commands::run_claude},
    {arguments::command::env, &commands::run_env},
    {arguments::command::setup, &commands::run_setup},
    {arguments::command::status, &commands::run_status},
    {arguments::command::stop, &commands::run_stop},
    {arguments::command::usage, &commands::run_usage},
    {arguments::command::doctor, &commands::run_doctor},
    {arguments::command::quota, &commands::run_quota},
    {arguments::command::use, &commands::run_use},
    {arguments::command::targets, &commands::run_targets},
    {arguments::command::route, &commands::run_route},
    {arguments::command::models, &models_command},
    {arguments::command::reload, &commands::run_reload},
    {arguments::command::provider, &edit_command},
    {arguments::command::routes, &edit_command},
    {arguments::command::config, &edit_command},
    {arguments::command::secret, &secret_command},
    {arguments::command::recipe, &recipe_command},
    {arguments::command::agents, &edit_command},
    {arguments::command::rules, &edit_command},
    {arguments::command::check, &check_command},
    {arguments::command::init, &init_command},
    {arguments::command::mod, &commands::run_mod},
    {arguments::command::statusline, &statusline_command},
});

constexpr bool handlers_cover_every_command() {
    return handlers.size() == reflect::enum_count<arguments::command> &&
           std::ranges::all_of(reflect::enum_values<arguments::command>, [](arguments::command const which) {
               return std::ranges::count(handlers, which, &std::pair<arguments::command, command_handler>::first) == 1;
           });
}
static_assert(handlers_cover_every_command());

int dispatch(commands::context const& where) {
    return std::ranges::find(handlers, where.options.which, &std::pair<arguments::command, command_handler>::first)->second(where);
}

int check_config(std::expected<gateway::config, std::string> const& settings) {
    auto const problem = settings ? gateway::config_problem(*settings) : std::string{};
    if (!problem.empty()) {
        return fail(exit_config, problem);
    }
    std::println("configuration is valid");
    return 0;
}

using document_printer = std::string (*)();

constexpr auto printers = std::to_array<std::pair<arguments::document, document_printer>>({
    {arguments::document::example, &config_docs::gateway_example},
    {arguments::document::schema, &config_docs::gateway_schema},
    {arguments::document::config_table, &config_docs::gateway_table},
});

constexpr bool printers_cover_every_document() {
    return printers.size() == reflect::enum_count<arguments::document> &&
           std::ranges::all_of(reflect::enum_values<arguments::document>, [](arguments::document const which) {
               return std::ranges::count(printers, which, &std::pair<arguments::document, document_printer>::first) == 1;
           });
}
static_assert(printers_cover_every_document());

int print_document(arguments::document const which) {
    std::print("{}", std::ranges::find(printers, which, &std::pair<arguments::document, document_printer>::first)->second());
    return 0;
}

commands::context make_context(arguments::options const& parsed, std::expected<gateway::config, std::string> settings, char const* const program) {
    auto lister = provider_lister(commands::dial_of(settings ? *settings : gateway::config{}));
    return {.options = parsed,
            .settings = settings ? std::move(*settings) : gateway::config{},
            .binary = process::current_executable().value_or(std::filesystem::path{program}),
            .config_error = settings ? std::string{} : settings.error(),
            .list_models = std::move(lister)};
}

std::optional<int> apply_profile(arguments::options const& parsed, std::expected<gateway::config, std::string>& settings) {
    if (parsed.profile.empty()) {
        return std::nullopt;
    }
    auto directory = claude::profile_directory(parsed.profile);
    if (directory.empty()) {
        return fail(exit_usage, "--profile takes letters, digits, - and _ only");
    }
    if (settings) {
        settings->claude.profile_dir = std::move(directory);
    }
    return std::nullopt;
}

int run(arguments::options const& parsed, char const* const program) {
    auto settings = load_config(parsed.config_path);
    if (!settings && !arguments::command_marked<arguments::tolerant>(parsed.which)) {
        return fail(exit_config, settings.error());
    }
    if (auto const refused = apply_profile(parsed, settings)) {
        return *refused;
    }
    if (parsed.check_config) {
        return check_config(settings);
    }
    return dispatch(make_context(parsed, std::move(settings), program));
}

}  // namespace

int main(int argc, char** argv) {
    auto const parsed = arguments::parse(std::vector<std::string>(argv + 1, argv + argc));
    if (!parsed) {
        std::println(stderr, "llmgate: {}\n{}", parsed.error(), arguments::usage_text);
        return exit_usage;
    }
    if (parsed->show_version) {
        std::println("{}", version_text);
        return 0;
    }
    if (auto const which = arguments::requested_document(*parsed)) {
        return print_document(*which);
    }
    return run(*parsed, argv[0]);
}

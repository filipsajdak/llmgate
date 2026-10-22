#include <boost/ut.hpp>

import llmgate.config_edit;
import llmgate.jsonedit;
import std;

namespace {

using namespace boost::ut;
namespace ce = llmgate::config_edit;

constexpr std::string_view base = R"({
  "listen": "127.0.0.1:9480",
  "providers": {
    "a": { "base_url": "http://127.0.0.1:1", "offer": ["m"] }
  }
})";

ce::request ask(ce::group which, std::vector<std::string> words, ce::request extra = {}) {
    extra.which = which;
    extra.words = std::move(words);
    return extra;
}

const suite config_edit_suite = [] {
    "capability_the_edit_words_become_a_path_and_a_json_value"_test = [] {
        auto const routes = ce::plan(ask(ce::group::routes, {"set", "sonnet", "a,b"}));
        auto const models = ce::plan(ask(ce::group::models, {"set", "a", "claude-x", "up-x"}));
        auto const number = ce::plan(ask(ce::group::config, {"set", "fallback.failures", "3"}));
        auto const text = ce::plan(ask(ce::group::config, {"set", "listen", "127.0.0.1:9"}));
        auto const unset = ce::plan(ask(ce::group::config, {"unset", "routes.opus"}));
        auto const cleared = ce::plan(ask(ce::group::routes, {"clear", "opus"}));
        auto const removed = ce::plan(ask(ce::group::provider, {"remove", "a"}));

        expect(routes.has_value() && routes->path == std::vector<std::string>{"routes", "sonnet"} && routes->value == R"(["a","b"])");
        expect(models.has_value() && models->path == std::vector<std::string>{"providers", "a", "models", "claude-x"} && models->value == R"("up-x")");
        expect(number.has_value() && number->value == "3");
        expect(text.has_value() && text->value == R"("127.0.0.1:9")");
        expect(unset.has_value() && unset->path == std::vector<std::string>{"routes", "opus"} && !unset->value);
        expect(cleared.has_value() && cleared->path == std::vector<std::string>{"routes", "opus"} && !cleared->value);
        expect(removed.has_value() && removed->path == std::vector<std::string>{"providers", "a"} && !removed->value);
    };

    "capability_provider_add_writes_the_preset_credentials_and_labels"_test = [] {
        auto const copilot = ce::plan(ask(ce::group::provider, {"add", "work"}, {.preset = "copilot", .labels = "work,acme"}));
        auto const anthropic = ce::plan(ask(ce::group::provider, {"add", "employer"}, {.preset = "anthropic", .token_env = "EMPLOYER_KEY"}));

        expect(copilot.has_value() && copilot->value->contains(R"("preset":"copilot")") && copilot->value->contains(R"("labels":["work","acme"])") && copilot->value->contains("copilot-work.json")) << copilot->value.value_or("");
        expect(anthropic.has_value() && anthropic->value->contains(R"("static_token_env":"EMPLOYER_KEY")") && !anthropic->value->contains("forward_credentials"));
    };

    "capability_provider_add_writes_auth_secret_offer_and_the_ghe_hosts"_test = [] {
        auto const partner = ce::plan(ask(ce::group::provider, {"add", "partner"},
                                        {.base_url = "https://gw.example.net", .auth = "header:api-key", .secret = "env:PARTNER_KEY", .offer = "gpt-sol,gpt-luna"}));
        auto const local = ce::plan(ask(ce::group::provider, {"add", "mic"}, {.base_url = "http://127.0.0.1:9080", .auth = "none", .offer = "opus4.6-partner"}));
        auto const ghe = ce::plan(ask(ce::group::provider, {"add", "work"}, {.preset = "copilot", .ghe_host = "corp.ghe.example"}));
        auto const filed = ce::plan(ask(ce::group::provider, {"add", "x"}, {.base_url = "http://h", .auth = "header:x-key:Token", .secret = "file:~/.config/llmgate/x.key"}));

        expect(partner.has_value() && partner->value->contains(R"("auth":{"kind":"header","header":"api-key","env":"PARTNER_KEY"})") && partner->value->contains(R"("offer":["gpt-sol","gpt-luna"])")) << partner.value_or(ce::change{}).value.value_or("");
        expect(!partner->value->contains("preset"));
        expect(local.has_value() && local->value->contains(R"("auth":{"kind":"none"})") && local->value->contains(R"("base_url":"http://127.0.0.1:9080")"));
        expect(ghe.has_value() && ghe->value->contains(R"("github_url":"https://corp.ghe.example")") && ghe->value->contains(R"("api_url":"https://api.corp.ghe.example")"));
        expect(filed.has_value() && filed->value->contains(R"("scheme":"Token")") && filed->value->contains(R"("file":"~/.config/llmgate/x.key")"));
    };

    "limit_a_provider_without_a_preset_needs_a_base_url_and_an_auth_and_bad_auth_words_are_refused"_test = [] {
        expect(!ce::plan(ask(ce::group::provider, {"add", "x"}, {.auth = "none"})).has_value());
        expect(!ce::plan(ask(ce::group::provider, {"add", "x"}, {.base_url = "http://h"})).has_value());
        expect(!ce::plan(ask(ce::group::provider, {"add", "x"}, {.base_url = "http://h", .auth = "oauth"})).has_value());
        expect(!ce::plan(ask(ce::group::provider, {"add", "x"}, {.base_url = "http://h", .auth = "header"})).has_value());
        expect(!ce::plan(ask(ce::group::provider, {"add", "x"}, {.base_url = "http://h", .auth = "header:api-key", .secret = "vault:x"})).has_value());
        expect(!ce::plan(ask(ce::group::provider, {"add", "x"}, {.base_url = "http://h", .auth = "none", .secret = "env:X"})).has_value());
    };

    "capability_a_secret_file_path_is_under_the_secrets_directory_and_a_name_cannot_escape_it"_test = [] {
        expect(ce::secret_file("partner") == std::expected<std::string, std::string>{"~/.config/llmgate/secrets/partner.key"});
        expect(!ce::secret_file("").has_value() && !ce::secret_file("../x").has_value() && !ce::secret_file("a/b").has_value() && !ce::secret_file(".hidden").has_value());
        expect(!ce::secret_file(std::string(100, 'a')).has_value());
    };

    "limit_unclear_edit_words_are_refused_with_the_shape_to_use"_test = [] {
        expect(!ce::plan(ask(ce::group::routes, {"set", "sonnet"})).has_value());
        expect(!ce::plan(ask(ce::group::routes, {"bogus"})).has_value());
        expect(!ce::plan(ask(ce::group::models, {"set", "a", "x"})).has_value());
        expect(!ce::plan(ask(ce::group::config, {"set", "a..b", "1"})).has_value());
        expect(!ce::plan(ask(ce::group::config, {"set", "", "1"})).has_value());
        expect(!ce::plan(ask(ce::group::provider, {"add", "bad name"}, {.preset = "copilot"})).has_value());
        expect(!ce::plan(ask(ce::group::provider, {"add", "x"})).has_value());
        expect(!ce::plan(ask(ce::group::provider, {"add", "x"}, {.preset = "nope"})).has_value());
        expect(!ce::plan(ask(ce::group::provider, {"add", "x"}, {.preset = "chatgpt", .labels = "bad label"})).has_value());
    };

    "capability_an_applied_edit_is_validated_as_a_whole_config_and_shows_its_diff"_test = [] {
        auto const added = ce::plan(ask(ce::group::provider, {"add", "b"}, {.preset = "anthropic", .token_env = "K"}));
        auto const done = ce::apply(base, *added);

        expect(done.has_value() && done->text.contains(R"("b":)") && done->diff.contains("+")) << (done ? done->diff : done.error());
        auto const unknown = ce::apply(base, *ce::plan(ask(ce::group::routes, {"set", "opus", "zzz"})));
        expect(!unknown.has_value() && unknown.error().contains("zzz")) << unknown.error();
        auto const broken = ce::apply(R"({ "listen": )", *ce::plan(ask(ce::group::config, {"set", "listen", "x"})));
        expect(!broken.has_value());
        auto const twice = ce::apply(base, *ce::plan(ask(ce::group::provider, {"add", "a"}, {.preset = "anthropic", .token_env = "K"})));
        expect(!twice.has_value() && twice.error().contains("already exists")) << twice.error();
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}

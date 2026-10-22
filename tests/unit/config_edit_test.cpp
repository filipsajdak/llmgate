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

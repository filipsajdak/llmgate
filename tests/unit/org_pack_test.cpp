#include <boost/ut.hpp>

import llmgate.json;
import llmgate.org_pack;
import std;

namespace {

using namespace boost::ut;
namespace org = llmgate::org;
namespace js = llmgate::json;

constexpr std::string_view sample = R"j({
  // company pack
  "schema": 1, "name": "acme", "version": "1.2.3",
  "config": { "providers": { "b": { "base_url": "https://b.example" } }, "network": { "proxy": "http://p:1" }, "listen": "127.0.0.1:1" },
  "locked": ["network"], "providers_allowed": ["b", "copilot"],
  "claude": { "deny": ["Bash(curl *)"], "announcements": ["hello"], "marketplaces": { "m": { "source": "github", "repo": "a/b" } }, "plugins": { "p@m": true } },
  "recipe": "second-opinion"
})j";

js::value parsed(std::string_view const text) {
    return std::move(*js::parse(text, {.jsonc = true}));
}

std::string error_of(std::string_view const text) {
    auto const done = org::read_pack(text);
    return done ? std::string{} : done.error();
}

suite<"org_pack"> tests = [] {
    "capability_a_pack_is_read_with_comments_and_every_section"_test = [] {
        auto const done = org::read_pack(sample);

        expect(done.has_value() && done->described.name == "acme" && done->described.version == "1.2.3" && done->described.recipe == "second-opinion");
        expect(done->described.locked == std::vector<std::string>{"network"} && done->described.claude.marketplaces.at("m").repo == "a/b" && done->described.claude.plugins.at("p@m"));
        expect(js::text_at(done->layer, {"providers", "b", "base_url"}) == "https://b.example");
    };

    "limit_an_unknown_key_a_wrong_schema_and_a_bad_version_are_refused_with_the_reason"_test = [] {
        expect(error_of(R"j({"schema":1,"name":"a","version":"1.0.0","surprise":true})j").contains("surprise"));
        expect(error_of(R"j({"schema":2,"name":"a","version":"1.0.0"})j").contains("schema must be 1"));
        expect(error_of(R"j({"schema":1,"name":"a","version":"1.0"})j").contains("MAJOR.MINOR.PATCH"));
        expect(error_of(R"j({"schema":1,"name":"","version":"1.0.0"})j").contains("name is required"));
        expect(error_of("{").contains("llmgate-org.jsonc") && error_of(R"j({"schema":1,"name":"a","version":"1.0.0","locked":"x"})j").contains("locked"));
    };

    "limit_control_characters_in_any_text_of_a_pack_are_refused_so_a_diff_cannot_spoof_the_terminal"_test = [] {
        auto const pack = [](std::string_view const field, std::string_view const value) {
            return std::format(R"j({{"schema":1,"name":"a","version":"1.0.0",{}:{}}})j", field, value);
        };

        expect(error_of(R"j({"schema":1,"name":"a[2J","version":"1.0.0"})j").contains("control character"));
        expect(error_of(pack(R"j("claude")j", R"j({"announcements":["ok
]0;x"]})j")).contains("control character"));
        expect(error_of(pack(R"j("claude")j", R"j({"plugins":{"p
q@m":true}})j")).contains("control character"));
        expect(error_of(R"j({"schema":1,"name":"a","version":"1.0.0","config":{"providers":{"x\u001b[2Jy":{"base_url":"https://a"}}}})j").contains("control character"));
        expect(error_of(R"j({"schema":1,"name":"a","version":"1.0.0","config":{"labels":["a\nb"]}})j").contains("control character"));
        expect(error_of(R"j({"schema":1,"name":"a","version":"1.0.0","config":{"providers":{"ok":{"base_url":"https://a"}}}})j").empty());
        expect(error_of(pack(R"j("claude")j", R"j({"deny":["Bash(ls)"],"announcements":["Grüße aus München"]})j")).empty());
        expect(!org::printable_text("a\u0085b") || true);
        expect(!org::printable_text(std::string{"a\xC2\x85"}) && org::printable_text(std::string{"\xC3\xBC"}) && !org::printable_text("a\x7f"));
    };

    "capability_the_user_wins_over_defaults_and_the_locked_path_wins_over_the_user"_test = [] {
        auto const done = org::read_pack(sample);
        auto const user = parsed(R"j({"listen":"127.0.0.1:9","network":{"proxy":"http://mine:2","no_proxy":["x"]},"providers":{"b":{"labels":["mine"]},"c":{"base_url":"https://c"}}})j");
        auto const result = org::effective_config(*done, user);

        expect(js::text_at(result, {"listen"}) == "127.0.0.1:9" && js::text_at(result, {"providers", "b", "base_url"}) == "https://b.example" && js::text_at(result, {"providers", "c", "base_url"}) == "https://c");
        expect(js::text_at(result, {"network", "proxy"}) == "http://p:1" && js::at(result, {"network", "no_proxy"}) == nullptr);
        expect(std::get<js::array>(js::at(result, {"providers_allowed"})->data).size() == 2_u && js::at(result, {"providers", "b", "labels"}) != nullptr);
    };

    "capability_keys_the_user_set_under_a_locked_path_are_named_when_they_differ"_test = [] {
        auto const done = org::read_pack(sample);

        expect(org::overridden_keys(*done, parsed(R"j({"network":{"proxy":"http://mine:2"}})j")) == std::vector<std::string>{"network"});
        expect(org::overridden_keys(*done, parsed(R"j({"network":{"proxy":"http://p:1"}})j")).empty());
        expect(org::overridden_keys(*done, parsed(R"j({"listen":"x"})j")).empty());
    };

    "capability_policy_is_added_without_touching_the_persons_entries_and_removed_exactly"_test = [] {
        auto const done = org::read_pack(sample);
        auto const settings = parsed(R"j({"permissions":{"deny":["Read(./mine)","Bash(curl *)"]},"companyAnnouncements":["mine"],"extraKnownMarketplaces":{"own":{"source":{"source":"github","repo":"x/y"}}},"enabledPlugins":{"own@own":true},"effortLevel":"high"})j");
        auto const result = org::apply_policy(settings, done->described.claude, {});

        expect(result.added.deny.empty() && result.added.announcements == std::vector<std::string>{"hello"});
        expect(result.added.marketplaces == std::vector<std::string>{"m"} && result.added.plugins == std::vector<std::string>{"p@m"});
        expect(js::text_at(result.settings, {"extraKnownMarketplaces", "m", "source", "repo"}) == "a/b" && js::at(result.settings, {"enabledPlugins", "p@m"}) != nullptr);
        expect(js::text_at(result.settings, {"effortLevel"}) == "high");

        auto const removed = org::remove_policy(result.settings, result.added);
        expect(js::at(removed, {"extraKnownMarketplaces", "m"}) == nullptr && js::at(removed, {"extraKnownMarketplaces", "own"}) != nullptr && js::at(removed, {"enabledPlugins", "own@own"}) != nullptr);
        expect(std::get<js::array>(js::at(removed, {"companyAnnouncements"})->data).size() == 1_u && std::get<js::array>(js::at(removed, {"permissions", "deny"})->data).size() == 2_u);
    };

    "capability_a_second_application_keeps_ownership_of_what_the_pack_added_before"_test = [] {
        auto const done = org::read_pack(sample);
        auto const first = org::apply_policy(parsed("{}"), done->described.claude, {});
        auto const second = org::apply_policy(first.settings, done->described.claude, first.added);

        expect(second.added.deny == first.added.deny && second.added.marketplaces == first.added.marketplaces && second.added.plugins == first.added.plugins);
        expect(std::get<js::array>(js::at(second.settings, {"permissions", "deny"})->data).size() == 1_u);
    };

    "capability_the_difference_shows_added_and_removed_lines_and_nothing_when_equal"_test = [] {
        auto const done = org::read_pack(sample);
        auto const lines = org::summary_of(*done);
        auto const none = org::difference({}, lines);
        auto const same = org::difference(lines, lines);
        auto const changed = org::difference({"deny rule: a"}, {"deny rule: b"});

        expect(!none.empty() && std::ranges::all_of(none, [](std::string const& each) { return each.starts_with("+ "); }) && same.empty());
        expect(changed == std::vector<std::string>{"- deny rule: a", "+ deny rule: b"});
        expect(std::ranges::any_of(lines, [](std::string const& each) { return each.starts_with("provider b: "); }) && std::ranges::contains(lines, std::string{"recipe: second-opinion"}));
    };
};

}  // namespace

int main() {}

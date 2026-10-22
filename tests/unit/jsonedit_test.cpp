#include <boost/ut.hpp>

import llmgate.jsonedit;
import std;

namespace {

using namespace boost::ut;
namespace edit = llmgate::jsonedit;

std::vector<std::string> path(std::initializer_list<std::string> const keys) { return keys; }

constexpr std::string_view sample = R"({
  // where to listen
  "listen": "127.0.0.1:9480",
  "providers": {
    "a": { "base_url": "http://x" }, /* first */
    "b": { "base_url": "http://y", "offer": ["m"] }
  },
  "routes": {}
})";

const suite jsonedit_suite = [] {
    "capability_an_existing_value_is_replaced_and_comments_and_layout_survive"_test = [] {
        auto const out = edit::set(sample, path({"providers", "b", "base_url"}), R"("http://z")");

        expect(out.has_value());
        expect(out->contains("// where to listen") && out->contains("/* first */") && out->contains(R"("base_url": "http://z", "offer": ["m"])"));
        expect(!out->contains("http://y"));
    };

    "capability_a_new_member_is_added_with_the_indent_of_its_siblings"_test = [] {
        auto const out = edit::set(sample, path({"providers", "c"}), R"({ "base_url": "http://c" })");

        expect(out.has_value());
        expect(out->contains("/* first */\n    \"b\": { \"base_url\": \"http://y\", \"offer\": [\"m\"] },\n    \"c\": { \"base_url\": \"http://c\" }\n  },")) << *out;
    };

    "capability_a_member_goes_into_an_empty_object_and_missing_parents_are_created"_test = [] {
        auto const filled = edit::set(sample, path({"routes", "sonnet"}), R"(["a","b"])");
        auto const nested = edit::set(sample, path({"claude", "profile_dir"}), R"("/p")");

        expect(filled.has_value() && filled->contains("\"routes\": {\n    \"sonnet\": [\"a\",\"b\"]\n  }")) << *filled;
        expect(nested.has_value() && nested->contains("\"claude\": {\n    \"profile_dir\": \"/p\"\n  }")) << *nested;
    };

    "capability_a_member_is_removed_with_its_comma_and_line"_test = [] {
        auto const out = edit::remove(sample, path({"providers", "a"}));
        auto const last = edit::remove(sample, path({"providers", "b"}));

        expect(out.has_value() && !out->contains("\"a\"") && out->contains("\"b\"")) << *out;
        expect(last.has_value() && !last->contains("\"b\"") && last->contains("\"a\": { \"base_url\": \"http://x\" }")) << *last;
        expect(last.has_value() && !last->contains("},\n  },")) << *last;
    };

    "limit_a_bad_path_or_text_is_refused_with_a_reason"_test = [] {
        expect(!edit::set(sample, path({"listen", "x"}), "1").has_value());
        expect(!edit::remove(sample, path({"nothing"})).has_value());
        expect(!edit::set("[]", path({"a"}), "1").has_value());
        expect(!edit::set(R"({"a": "unterminated)", path({"b"}), "1").has_value());
        expect(!edit::set(sample, {}, "1").has_value());
    };

    "limit_a_key_that_looks_like_a_comment_inside_a_string_is_not_one"_test = [] {
        auto const out = edit::set(R"({ "url": "http://a//b", "x": 1 })", path({"x"}), "2");

        expect(out.has_value() && *out == R"({ "url": "http://a//b", "x": 2 })");
    };

    "capability_the_diff_shows_only_the_changed_lines_with_context"_test = [] {
        auto const out = edit::set(sample, path({"listen"}), R"("127.0.0.1:1")");

        expect(edit::diff(sample, *out).contains("-   \"listen\": \"127.0.0.1:9480\",\n+   \"listen\": \"127.0.0.1:1\",")) << edit::diff(sample, *out);
        expect(edit::diff(sample, sample) == "no change\n");
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}

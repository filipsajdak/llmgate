#include <boost/ut.hpp>

import llmgate.config;
import llmgate.config_docs;
import llmgate.doc;
import llmgate.json;
import llmgate.reflect;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate;
using namespace llmgate::config_docs;
using std::chrono::milliseconds;

enum class mode { quiet, loud };

struct inner {
    [[= reflect::doc{"Pause between two polls."}]] [[= reflect::example{R"("5s")"}]] [[= reflect::commented]] std::optional<milliseconds> pause{};
    [[= reflect::doc{"Retries."}]] unsigned retries = 3;
};

struct sample {
    [[= json::required]] [[= reflect::doc{"Where to listen.\nSecond paragraph."}]] [[= reflect::example{R"("127.0.0.1:1")"}]] std::string listen{};
    [[= reflect::doc{"A switch."}]] bool verbose = false;
    [[= reflect::doc{"A signed number."}]] std::int32_t delta = -4;
    [[= reflect::doc{"A real number."}]] double ratio = 0.5;
    [[= reflect::doc{"A duration."}]] milliseconds wait = std::chrono::seconds{2};
    [[= reflect::doc{"A choice."}]] mode how = mode::loud;
    [[= reflect::doc{"Nested with a pipe | and\nnewline."}]] inner nested{};
    [[= reflect::doc{"Maybe nested."}]] std::optional<inner> maybe{};
    [[= reflect::doc{"Names."}]] std::vector<std::string> names{};
    [[= reflect::doc{"Parts by name."}]] std::map<std::string, inner> parts{};
    [[= json::skip]] [[= reflect::doc{"Never shown."}]] int scratch = 0;
    [[= json::key{"renamed"}]] [[= reflect::doc{"Wire name differs."}]] int plain = 1;
};

struct ends {
    [[= reflect::doc{"First."}]] int first = 1;
    [[= reflect::doc{"Last."}]] [[= reflect::example{"2"}]] [[= reflect::commented]] int last = 2;
};

node const* find(std::vector<node> const& members, std::string_view const name) {
    auto const hit = std::ranges::find(members, name, &node::name);
    return hit == members.end() ? nullptr : &*hit;
}

bool documented(std::vector<node> const& members) {
    return std::ranges::all_of(members, [](node const& item) {
        return !item.doc.empty() && documented(item.members) && std::ranges::all_of(item.element, [](node const& inner_item) { return documented(inner_item.members); });
    });
}

}  // namespace

int main() {
    "capability_every_member_kind_is_described"_test = [] {
        auto const root = describe<sample>();
        expect(root.type == kind::object);
        expect(find(root.members, "scratch") == nullptr);
        expect(find(root.members, "plain") == nullptr && find(root.members, "renamed") != nullptr);
        expect(find(root.members, "verbose")->type == kind::flag);
        expect(find(root.members, "delta")->type == kind::whole && !find(root.members, "delta")->non_negative);
        expect(find(root.members, "ratio")->type == kind::real);
        expect(find(root.members, "wait")->type == kind::span);
        expect(find(root.members, "how")->type == kind::choice && find(root.members, "how")->choices == std::vector<std::string>{"quiet", "loud"});
        expect(find(root.members, "names")->type == kind::list && find(root.members, "parts")->type == kind::table);
        expect(find(find(root.members, "nested")->members, "retries")->non_negative);
        expect(find(root.members, "maybe")->optional && !find(root.members, "nested")->optional);
        expect(find(root.members, "listen")->required && !find(root.members, "verbose")->required);
        expect(find(find(root.members, "nested")->members, "pause")->commented);
    };

    "capability_defaults_come_from_a_value_initialised_object"_test = [] {
        auto const root = describe<sample>();
        expect(json::write(find(root.members, "delta")->fallback) == "-4");
        expect(json::write(find(root.members, "wait")->fallback) == R"("2s")");
        expect(json::write(find(root.members, "how")->fallback) == R"("loud")");
        expect(!has_default(*find(root.members, "nested")) && !has_default(*find(find(root.members, "nested")->members, "pause")));
    };

    "capability_the_example_is_a_valid_document_that_reads_back"_test = [] {
        auto const text = example_text(describe<sample>());
        auto const parsed = json::parse(text);
        expect(parsed.has_value()) << text;
        auto const loaded = json::read<sample>(*parsed);
        expect(loaded.has_value() && loaded->listen == "127.0.0.1:1" && loaded->delta == -4 && loaded->how == mode::loud) << text;
        expect(text.contains("  // Where to listen.\n  // Second paragraph.\n"));
        expect(text.contains("    // \"pause\": \"5s\",\n"));
        expect(!text.contains("scratch"));
    };

    "capability_the_example_has_commas_only_between_shown_members"_test = [] {
        expect(example_text(describe<ends>()).contains("  \"first\": 1\n\n  // Last.\n  // \"last\": 2,\n}"));
    };

    "capability_the_table_lists_every_path_with_type_default_and_text"_test = [] {
        auto const table = table_text(describe<sample>());
        expect(table.starts_with("| Key | Type | Default | Description |\n|---|---|---|---|\n"));
        expect(table.contains("| `listen` | string | required | Where to listen. Second paragraph. |"));
        expect(table.contains("| `how` | one of `quiet`, `loud` | `\"loud\"` |"));
        expect(table.contains("| `nested.pause` | duration | none |"));
        expect(table.contains("| `nested` | object | - | Nested with a pipe \\| and newline. |"));
        expect(table.contains("| `maybe` | object | none |"));
        expect(table.contains("| `parts.<name>.retries` | integer | `3` |"));
        expect(table.contains("| `names` | array of string | `[]` |"));
        expect(table.contains("| `parts` | map of object | `{}` |"));
        expect(!table.contains("scratch"));
    };

    "capability_the_schema_is_a_json_schema_of_the_same_members"_test = [] {
        auto const text = schema_text(describe<sample>(), "sample");
        auto const parsed = json::parse(text, {.jsonc = false});
        expect(parsed.has_value()) << text;
        expect(json::text_at(*parsed, {"$schema"}) == "https://json-schema.org/draft/2020-12/schema");
        expect(json::text_at(*parsed, {"title"}) == "sample");
        expect(json::text_at(*parsed, {"properties", "listen", "type"}) == "string");
        expect(json::at(*parsed, {"properties", "listen", "default"}) == nullptr);
        expect(json::text_at(*parsed, {"properties", "wait", "pattern"}) == duration_pattern);
        expect(json::count_at(*parsed, {"properties", "nested", "properties", "retries", "minimum"}) == 0_u);
        expect(json::at(*parsed, {"properties", "how", "enum"}) != nullptr);
        expect(json::text_at(*parsed, {"properties", "names", "items", "type"}) == "string");
        expect(json::text_at(*parsed, {"properties", "parts", "additionalProperties", "type"}) == "object");
        expect(json::text_at(*parsed, {"properties", "ratio", "type"}) == "number");
        expect(json::text_at(*parsed, {"properties", "verbose", "type"}) == "boolean");
        expect(json::text_at(*parsed, {"properties", "delta", "type"}) == "integer");
        expect(json::at(*parsed, {"properties", "delta", "minimum"}) == nullptr);
    };

    "capability_the_duration_pattern_names_the_units_the_reader_accepts"_test = [] {
        for (std::string_view const text : {"10s", "500ms", "2m", "1h"}) {
            expect(json::read_text<milliseconds>(std::format("\"{}\"", text)).has_value()) << text;
            expect(duration_pattern.contains(text.substr(text.find_first_not_of("0123456789")))) << text;
        }
        for (auto const text : {"10", "s", "1d", "-1s"}) {
            expect(!json::read_text<milliseconds>(std::format("\"{}\"", text)).has_value()) << text;
        }
    };

    "capability_comments_wrap_at_word_boundaries_and_keep_paragraphs"_test = [] {
        expect(comment_text("") == "");
        expect(comment_text("a  b") == "// a b\n");
        expect(comment_text("one\n\ntwo") == "// one\n//\n// two\n");
        auto const long_text = std::string(comment_width, 'x') + " tail";
        expect(comment_text(long_text) == "// " + std::string(comment_width, 'x') + "\n// tail\n");
        expect(table_cell("a|b\nc") == "a\\|b c");
    };

    "capability_the_gateway_configuration_documents_every_member"_test = [] {
        auto const root = describe<gateway::config>();
        auto const example = gateway_example();
        expect(every_key_is_shown(root, example));
        expect(!keys_of(root).empty());
        expect(documented(root.members));
        auto const parsed = json::parse(example);
        expect(parsed.has_value() && json::read<gateway::config>(*parsed).has_value());
        expect(gateway_table().contains("| `time.drain` | duration | `\"10s\"` |"));
        expect(json::parse(gateway_schema(), {.jsonc = false}).has_value());
    };
}

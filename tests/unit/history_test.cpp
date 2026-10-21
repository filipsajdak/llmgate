#include <boost/ut.hpp>

import llmgate.history;
import llmgate.json;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate;

std::string messages_of(std::string const& body) {
    auto const parsed = json::parse(body, {.jsonc = false});
    auto const* found = parsed ? json::at(*parsed, {"messages"}) : nullptr;
    return found == nullptr ? std::string{"<missing>"} : json::write(*found);
}

const suite history_suite = [] {
    "capability_thinking_and_redacted_thinking_blocks_are_removed_from_every_assistant_turn"_test = [] {
        auto const done = history::without_thinking(R"({"model":"m","messages":[
          {"role":"user","content":"a"},
          {"role":"assistant","content":[{"type":"thinking","thinking":"x","signature":"s1"},{"type":"text","text":"one"}]},
          {"role":"user","content":"b"},
          {"role":"assistant","content":[{"type":"redacted_thinking","data":"d"},{"type":"tool_use","id":"t","name":"Bash","input":{}}]},
          {"role":"user","content":[{"type":"tool_result","tool_use_id":"t","content":"ok"}]}]})");

        expect(done.has_value());
        expect(done->removed == 2_u);
        expect(messages_of(done->body) ==
               R"([{"role":"user","content":"a"},{"role":"assistant","content":[{"type":"text","text":"one"}]},{"role":"user","content":"b"},)"
               R"({"role":"assistant","content":[{"type":"tool_use","id":"t","name":"Bash","input":{}}]},{"role":"user","content":[{"type":"tool_result","tool_use_id":"t","content":"ok"}]}])");
    };

    "capability_the_rest_of_the_request_is_kept_in_order"_test = [] {
        auto const done = history::without_thinking(R"({"model":"m","max_tokens":64,"stream":true,"thinking":{"type":"adaptive"},"temperature":0.5,"messages":[{"role":"assistant","content":[{"type":"thinking","thinking":"x","signature":"s"},{"type":"text","text":"t"}]}]})");

        expect(done.has_value());
        expect(done->body == R"({"model":"m","max_tokens":64,"stream":true,"thinking":{"type":"adaptive"},"temperature":0.5,"messages":[{"role":"assistant","content":[{"type":"text","text":"t"}]}]})") << done->body;
    };

    "limit_an_assistant_turn_that_held_only_thinking_is_removed_not_left_empty"_test = [] {
        auto const done = history::without_thinking(R"({"messages":[{"role":"user","content":"a"},{"role":"assistant","content":[{"type":"thinking","thinking":"x","signature":"s"}]},{"role":"user","content":"b"}]})");

        expect(done.has_value() && done->removed == 1_u);
        expect(messages_of(done->body) == R"([{"role":"user","content":"a"},{"role":"user","content":"b"}])");
    };

    "limit_a_request_with_nothing_to_strip_or_that_is_not_json_is_left_alone"_test = [] {
        expect(!history::without_thinking(R"({"messages":[{"role":"user","content":"a"},{"role":"assistant","content":[{"type":"text","text":"t"}]}]})").has_value());
        expect(!history::without_thinking(R"({"model":"m"})").has_value());
        expect(!history::without_thinking("not json").has_value());
        expect(!history::without_thinking(R"({"messages":"x"})").has_value());
    };

    "limit_thinking_in_a_user_turn_or_in_a_string_is_not_touched"_test = [] {
        auto const text = R"({"messages":[{"role":"user","content":"a thinking block is {\"type\":\"thinking\"}"},{"role":"assistant","content":[{"type":"text","text":"type thinking"}]}]})";

        expect(!history::without_thinking(text).has_value());
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}

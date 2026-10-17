#include <boost/ut.hpp>

import llmgate.anthropic;
import llmgate.sse;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate;
using anthropic::repair_kind;

struct input_event {
    std::string name;
    std::string data;
};

struct result {
    std::vector<anthropic::output_event> out;
    std::vector<anthropic::repair_kind> repairs;
    bool complete = false;
};

result run_guard(std::span<input_event const> events, bool const end_of_stream = true) {
    anthropic::stream_guard guard;
    result seen;
    auto out = [&seen](anthropic::output_event const& item) { seen.out.push_back(item); };
    auto report = [&seen](anthropic::repair const& item) { seen.repairs.push_back(item.kind); };
    for (auto const& item : events) {
        guard.on_event(sse::event{.name = item.name, .data = item.data, .id = {}}, out, report);
    }
    if (end_of_stream) {
        guard.on_end(out, report);
    }
    seen.complete = guard.complete();
    return seen;
}

input_event start() { return {"message_start", R"({"type":"message_start","message":{"id":"m"}})"}; }
input_event block_start(int const i) { return {"content_block_start", std::format(R"({{"type":"content_block_start","index":{}}})", i)}; }
input_event block_delta(int const i) { return {"content_block_delta", std::format(R"({{"type":"content_block_delta","index":{},"delta":{{}}}})", i)}; }
input_event block_stop(int const i) { return {"content_block_stop", std::format(R"({{"type":"content_block_stop","index":{}}})", i)}; }
input_event delta() { return {"message_delta", R"({"type":"message_delta","delta":{},"usage":{"output_tokens":3}})"}; }
input_event stop() { return {"message_stop", R"({"type":"message_stop"})"}; }

std::vector<input_event> clean_stream() {
    return {start(), block_start(0), block_delta(0), block_stop(0), delta(), stop()};
}

std::vector<std::string> names(result const& seen) {
    return seen.out | std::views::transform(&anthropic::output_event::name) | std::ranges::to<std::vector>();
}

bool has(result const& seen, repair_kind const kind) {
    return std::ranges::contains(seen.repairs, kind);
}

const std::vector<std::string> clean_names{"message_start", "content_block_start", "content_block_delta", "content_block_stop", "message_delta", "message_stop"};

const suite guard_suite = [] {
    "capability_an_unnamed_event_is_forwarded_under_its_wire_name_and_an_unknown_type_stays_unnamed"_test = [] {
        auto events = clean_stream();
        events.pop_back();
        events.insert(events.begin() + 1, input_event{"", R"({"type":"ping"})"});
        events.emplace_back("", R"({"type":"something_new"})");
        for (auto& item : events) {
            item.name.clear();
        }

        auto const seen = run_guard(events, false);

        expect(names(seen) == std::vector<std::string>{"message_start", "ping", "content_block_start", "content_block_delta",
                                                       "content_block_stop", "message_delta", ""});
    };

    "capability_an_unnamed_error_event_is_forwarded_as_error"_test = [] {
        std::vector<input_event> const events{{"", R"({"type":"error","error":{}})"}};

        auto const seen = run_guard(events, false);

        expect(names(seen) == std::vector<std::string>{"error"});
        expect(seen.complete);
    };

    "capability_a_well_formed_stream_passes_through_unchanged"_test = [] {
        auto const events = clean_stream();

        auto const seen = run_guard(events);

        expect(names(seen) == clean_names);
        expect(seen.repairs.empty());
        expect(seen.complete);
    };

    "defect_repeated_message_start_events_are_dropped"_test = [] {
        std::vector<input_event> events{start(), start(), start(), block_start(0), block_stop(0), delta(), stop()};

        auto const seen = run_guard(events);

        expect(std::ranges::count(names(seen), std::string_view{"message_start"}) == 1);
        expect(std::ranges::count(seen.repairs, repair_kind::duplicate_message_start) == 2);
    };

    "defect_a_stream_with_no_message_start_gets_one_first"_test = [] {
        std::vector<input_event> events{block_start(0), block_delta(0), block_stop(0), delta(), stop()};

        auto const seen = run_guard(events);

        expect(names(seen).front() == "message_start");
        expect(has(seen, repair_kind::missing_message_start));
        expect(anthropic::validate_grammar(seen.out).has_value());
    };

    "defect_a_tool_use_block_without_content_block_stop_is_closed_before_message_delta"_test = [] {
        std::vector<input_event> events{start(), block_start(0), block_delta(0), delta(), stop()};

        auto const seen = run_guard(events);

        expect(names(seen) == std::vector<std::string>{"message_start", "content_block_start", "content_block_delta", "content_block_stop", "message_delta", "message_stop"});
        expect(has(seen, repair_kind::unclosed_block));
    };

    "capability_message_stop_without_message_delta_gets_one"_test = [] {
        std::vector<input_event> events{start(), block_start(0), block_stop(0), stop()};

        auto const seen = run_guard(events);

        expect(has(seen, repair_kind::missing_message_delta));
        expect(anthropic::validate_grammar(seen.out).has_value());
    };

    "capability_message_delta_without_usage_gets_a_zero_usage"_test = [] {
        std::vector<input_event> events{start(), {"message_delta", R"({"type":"message_delta","delta":{}})"}, stop()};

        auto const seen = run_guard(events);

        expect(has(seen, repair_kind::missing_usage));
        expect(anthropic::validate_grammar(seen.out).has_value());
    };

    "limit_events_after_message_stop_are_dropped"_test = [] {
        auto events = clean_stream();
        events.push_back(block_start(1));
        events.push_back(stop());

        auto const seen = run_guard(events);

        expect(names(seen) == clean_names);
        expect(std::ranges::count(seen.repairs, repair_kind::event_after_stop) == 2);
    };

    "limit_a_delta_for_a_block_that_was_never_opened_is_dropped"_test = [] {
        std::vector<input_event> events{start(), block_delta(4), delta(), stop()};

        auto const seen = run_guard(events);

        expect(has(seen, repair_kind::orphan_block_event));
        expect(anthropic::validate_grammar(seen.out).has_value());
    };

    "limit_reopening_an_open_block_closes_the_first_one"_test = [] {
        std::vector<input_event> events{start(), block_start(0), block_start(0), block_stop(0), delta(), stop()};

        auto const seen = run_guard(events);

        expect(has(seen, repair_kind::unclosed_block));
        expect(anthropic::validate_grammar(seen.out).has_value());
    };

    "defect_a_content_block_opened_after_message_delta_is_dropped"_test = [] {
        std::vector<input_event> events{start(), delta(), block_start(0), block_delta(0), block_stop(0), stop()};

        auto const seen = run_guard(events);

        expect(names(seen) == std::vector<std::string>{"message_start", "message_delta", "message_stop"});
        expect(std::ranges::count(seen.repairs, repair_kind::orphan_block_event) == 3);
    };

    "limit_a_second_message_delta_is_dropped"_test = [] {
        std::vector<input_event> events{start(), delta(), delta(), stop()};

        auto const seen = run_guard(events);

        expect(std::ranges::count(names(seen), std::string_view{"message_delta"}) == 1);
        expect(has(seen, repair_kind::duplicate_message_delta));
    };

    "limit_an_interrupted_stream_ends_with_an_error_event_not_a_fake_success"_test = [] {
        std::vector<input_event> events{start(), block_start(0), block_delta(0)};

        auto const seen = run_guard(events);

        expect(names(seen).back() == "error");
        expect(!std::ranges::contains(names(seen), std::string_view{"message_stop"}));
        expect(has(seen, repair_kind::truncated_stream));
        expect(seen.complete);
    };

    "limit_an_upstream_error_event_ends_the_stream_without_synthesized_closers"_test = [] {
        std::vector<input_event> events{start(), block_start(0), {"error", R"({"type":"error","error":{"type":"overloaded_error"}})"}, block_delta(0)};

        auto const seen = run_guard(events);

        expect(names(seen) == std::vector<std::string>{"message_start", "content_block_start", "error"});
        expect(seen.repairs == std::vector{repair_kind::event_after_stop});
    };

    "capability_pings_and_unknown_events_pass_at_any_time"_test = [] {
        std::vector<input_event> events{{"ping", R"({"type":"ping"})"}, start(), {"x_future", R"({"type":"x_future"})"}, delta(), stop()};

        auto const seen = run_guard(events);

        expect(names(seen) == std::vector<std::string>{"ping", "message_start", "x_future", "message_delta", "message_stop"});
        expect(seen.repairs.empty());
    };

    "limit_event_data_that_is_not_a_json_object_is_dropped"_test = [] {
        std::vector<input_event> events{start(), {"message_delta", "not json"}, delta(), stop()};

        auto const seen = run_guard(events);

        expect(has(seen, repair_kind::unparseable_event));
        expect(anthropic::validate_grammar(seen.out).has_value());
    };

    "limit_whatever_the_upstream_sends_the_output_always_satisfies_the_grammar"_test = [] {
        constexpr std::size_t palette_size = 11;
        constexpr int cases = 600;
        constexpr int longest = 24;
        for (int seed = 1; seed <= cases; ++seed) {
            std::mt19937 random(static_cast<unsigned>(seed));
            std::vector<input_event> events;
            auto const length = std::uniform_int_distribution<int>{0, longest}(random);
            for (int step = 0; step < length; ++step) {
                auto const index = std::uniform_int_distribution<int>{0, 2}(random);
                switch (std::uniform_int_distribution<std::size_t>{0, palette_size - 1}(random)) {
                    case 0: events.push_back(start()); break;
                    case 1: events.push_back(block_start(index)); break;
                    case 2: events.push_back(block_delta(index)); break;
                    case 3: events.push_back(block_stop(index)); break;
                    case 4: events.push_back(delta()); break;
                    case 5: events.push_back(stop()); break;
                    case 6: events.push_back({"ping", R"({"type":"ping"})"}); break;
                    case 7: events.push_back({"error", R"({"type":"error","error":{}})"}); break;
                    case 8: events.push_back({"message_delta", R"({"type":"message_delta","delta":{}})"}); break;
                    case 9: events.push_back({"junk", "{not json"}); break;
                    default: events.push_back({"x_future", R"({"type":"x_future"})"}); break;
                }
            }

            auto const seen = run_guard(events);
            auto const verdict = anthropic::validate_grammar(seen.out);

            expect(verdict.has_value()) << "seed" << seed << verdict.error_or("");
            expect(seen.complete) << "seed" << seed;
        }
    };

    "capability_the_validator_rejects_the_defects_the_guard_repairs"_test = [] {
        auto const dup = std::vector<anthropic::output_event>{{"message_start", start().data}, {"message_start", start().data}};
        auto const unclosed = std::vector<anthropic::output_event>{{"message_start", start().data}, {"content_block_start", block_start(0).data}, {"message_delta", delta().data}};

        expect(!anthropic::validate_grammar(dup).has_value());
        expect(!anthropic::validate_grammar(unclosed).has_value());
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}

#include <boost/ut.hpp>

import llmgate.sse;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate::sse;

struct owned_event {
    std::string name;
    std::string data;
    std::string id;
    bool operator==(owned_event const&) const = default;
};

std::vector<owned_event> parse_all(std::string_view const text, std::size_t const piece = std::dynamic_extent) {
    parser reader;
    std::vector<owned_event> events;
    auto collect = [&events](event const& item) {
        events.push_back({.name = std::string{item.name}, .data = std::string{item.data}, .id = std::string{item.id}});
    };
    auto const bytes = std::as_bytes(std::span{text});
    for (std::size_t at = 0; at < bytes.size(); at += piece) {
        reader.feed(bytes.subspan(at, std::min(piece, bytes.size() - at)), collect);
    }
    reader.finish(collect);
    return events;
}

constexpr std::string_view anthropic_stream =
    "event: message_start\ndata: {\"type\":\"message_start\"}\n\n"
    "event: content_block_delta\ndata: {\"delta\":\"hi\"}\n\n"
    "event: message_stop\ndata: {}\n\n";

const suite sse_suite = [] {
    "capability_events_carry_their_name_and_data"_test = [] {
        auto const events = parse_all(anthropic_stream);

        expect(events.size() == 3_u);
        expect(events[0].name == "message_start");
        expect(events[1].data == "{\"delta\":\"hi\"}");
        expect(events[2].name == "message_stop");
    };

    "capability_input_split_at_every_byte_gives_the_same_events"_test = [] {
        auto const split = parse_all(anthropic_stream, 1);
        auto const whole = parse_all(anthropic_stream);
        auto const same = std::ranges::equal(split, whole);

        expect(same);
    };

    "capability_multiple_data_lines_join_with_a_newline"_test = [] {
        auto const events = parse_all("data: a\ndata: b\n\n");

        expect(events.size() == 1_u);
        expect(events[0].data == "a\nb");
    };

    "capability_crlf_cr_and_lf_all_end_a_line"_test = [] {
        expect(parse_all("data: x\r\n\r\n").size() == 1_u);
        expect(parse_all("data: x\r\r").size() == 1_u);
        expect(parse_all("data: x\n\n").size() == 1_u);
    };

    "capability_comments_and_unknown_fields_are_ignored"_test = [] {
        auto const events = parse_all(": keep-alive\nretry: 5\ndata: y\n\n");

        expect(events.size() == 1_u);
        expect(events[0].data == "y");
    };

    "capability_only_one_leading_space_is_stripped"_test = [] {
        expect(parse_all("data:  two\n\n")[0].data == " two");
        expect(parse_all("data:none\n\n")[0].data == "none");
    };

    "limit_an_event_without_data_is_not_dispatched"_test = [] {
        expect(parse_all("event: ping\n\n").empty());
    };

    "limit_an_unterminated_event_is_dispatched_at_finish"_test = [] {
        auto const events = parse_all("data: tail");

        expect(events.size() == 1_u);
        expect(events[0].data == "tail");
    };

    "capability_written_events_parse_back_to_the_same_fields"_test = [] {
        std::string wire;
        write_event(wire, "content_block_delta", "line1\nline2");

        auto const events = parse_all(wire);

        expect(events.size() == 1_u);
        expect(events[0].name == "content_block_delta");
        expect(events[0].data == "line1\nline2");
    };
};

}  // namespace

int main() {}

#include <boost/ut.hpp>

import llmgate.http;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate::http;

struct captured {
    std::vector<message_head> heads;
    std::string body;
    int completed = 0;

    auto callback() {
        return [this](event const& item) {
            if (auto const* head = std::get_if<message_head>(&item)) {
                heads.push_back(*head);
            } else if (auto const* chunk = std::get_if<body_chunk>(&item)) {
                body.append(reinterpret_cast<char const*>(chunk->bytes.data()), chunk->bytes.size());
            } else {
                ++completed;
            }
        };
    }
};

std::span<std::byte const> bytes_of(std::string_view const text) {
    return std::as_bytes(std::span{text});
}

template <class Sink>
std::expected<void, std::error_code> feed_all(parser& reader, std::span<std::byte const> data, Sink&& sink) {
    while (!data.empty()) {
        auto const consumed = reader.feed(data, sink);
        if (!consumed) {
            return std::unexpected{consumed.error()};
        }
        data = data.subspan(*consumed);
    }
    return {};
}

constexpr std::string_view simple_request =
    "POST /anthropic/v1/messages HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\nX-Api-Key: secret\r\n\r\nhello";

constexpr std::string_view chunked_response =
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Type: text/event-stream\r\n\r\n"
    "6\r\nevent:\r\n3\r\n ok\r\n0\r\n\r\n";

const suite http_suite = [] {
    "capability_request_head_and_body_are_reported"_test = [] {
        parser reader{kind::request};
        captured seen;
        auto callback = seen.callback();

        auto const result = reader.feed(bytes_of(simple_request), callback);

        expect(result.has_value());
        expect(seen.heads.size() == 1_u);
        expect(seen.heads[0].method == "POST");
        expect(seen.heads[0].target == "/anthropic/v1/messages");
        expect(seen.heads[0].content_length == 5_u);
        expect(seen.heads[0].fields.find("x-api-key") == "secret");
        expect(seen.body == "hello");
        expect(seen.completed == 1);
    };

    "capability_chunked_response_is_decoded"_test = [] {
        parser reader{kind::response};
        captured seen;
        auto callback = seen.callback();

        auto const result = reader.feed(bytes_of(chunked_response), callback);

        expect(result.has_value());
        expect(seen.heads[0].status == 200);
        expect(seen.heads[0].chunked);
        expect(seen.body == "event: ok");
        expect(seen.completed == 1);
    };

    "capability_input_split_at_every_byte_gives_the_same_result"_test = [] {
        parser reader{kind::request};
        captured seen;
        auto callback = seen.callback();

        for (auto const byte : bytes_of(simple_request)) {
            expect(reader.feed(std::span{&byte, 1}, callback).has_value());
        }

        expect(seen.heads.size() == 1_u);
        expect(seen.heads[0].fields.find("Host") == "localhost");
        expect(seen.body == "hello");
        expect(seen.completed == 1);
    };

    "capability_two_pipelined_requests_complete_separately"_test = [] {
        parser reader{kind::request};
        captured seen;
        auto callback = seen.callback();
        std::string const both = std::string{simple_request} + std::string{simple_request};

        expect(feed_all(reader, bytes_of(both), callback).has_value());

        expect(seen.completed == 2);
        expect(seen.body == "hellohello");
    };

    "capability_feed_stops_at_the_end_of_each_message"_test = [] {
        parser reader{kind::request};
        captured seen;
        auto callback = seen.callback();
        std::string const both = std::string{simple_request} + std::string{simple_request};

        auto const consumed = reader.feed(bytes_of(both), callback);

        expect(consumed.has_value());
        expect(*consumed == simple_request.size());
        expect(seen.completed == 1);
    };

    "capability_response_without_length_ends_at_finish"_test = [] {
        parser reader{kind::response};
        captured seen;
        auto callback = seen.callback();

        expect(reader.feed(bytes_of("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\npartial"), callback).has_value());
        expect(seen.completed == 0);
        expect(reader.finish(callback).has_value());

        expect(seen.body == "partial");
        expect(seen.completed == 1);
    };

    "capability_a_temporary_callback_is_safe_to_pass"_test = [] {
        parser reader{kind::request};
        int completed = 0;

        expect(reader.feed(bytes_of(simple_request), [&completed](event const& item) {
                   completed += std::holds_alternative<message_complete>(item) ? 1 : 0;
               }).has_value());

        expect(completed == 1);
    };

    "limit_garbage_is_a_malformed_message_with_a_diagnostic"_test = [] {
        parser reader{kind::request};
        captured seen;
        auto callback = seen.callback();

        auto const result = reader.feed(bytes_of("\x01\x02 not http\r\n\r\n"), callback);

        expect(!result.has_value());
        expect(result.error() == errc::malformed_message);
        expect(!reader.diagnostic().empty());
    };

    "limit_a_head_over_the_limit_is_rejected_as_too_large"_test = [] {
        parser reader{kind::request, {.max_head_bytes = 32}};
        captured seen;
        auto callback = seen.callback();

        auto const result = reader.feed(bytes_of(simple_request), callback);

        expect(!result.has_value());
        expect(result.error() == errc::head_too_large);
    };

    "limit_a_body_cut_short_is_a_truncated_message_at_finish"_test = [] {
        parser reader{kind::response};
        captured seen;
        auto callback = seen.callback();

        expect(reader.feed(bytes_of("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc"), callback).has_value());
        auto const result = reader.finish(callback);

        expect(!result.has_value());
        expect(result.error() == errc::truncated_message);
    };
};

}  // namespace

int main() {}

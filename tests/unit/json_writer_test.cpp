#include <boost/ut.hpp>

import llmgate.json;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate::json;
namespace json = llmgate::json;
using std::chrono::milliseconds;

enum class level { quiet, loud };

struct limits {
    milliseconds idle = std::chrono::seconds{60};
    std::int32_t max_messages = 100;

    constexpr bool operator==(limits const&) const = default;
};

struct headers {
    [[= key{"x-api-key"}]] std::string api_key{};
    std::optional<std::string> trace{};

    constexpr bool operator==(headers const&) const = default;
};

struct plain_sample {
    bool verbose = false;
    std::uint64_t big = 0;
    std::int8_t small = 0;
    double ratio = 0.0;
    std::string name{};
    level mode = level::quiet;
    std::optional<level> maybe{};
    limits timeouts{};
    std::vector<limits> history{};
    std::vector<std::optional<std::int32_t>> slots{};

    constexpr bool operator==(plain_sample const&) const = default;
};

struct annotated_sample {
    [[= key{"id"}]] std::string identifier{};
    [[= flatten]] headers extra{};
    [[= json::skip]] int scratch = 0;
    [[= redact]] std::string secret{};
    std::map<level, std::string> by_level{};
    std::map<std::string, std::vector<std::string>> by_name{};
};

struct with_pointer {
    [[= json::skip]] char const* text = "";
    int count = 1;
};

static_assert(writable<plain_sample>);
static_assert(writable<annotated_sample>);
static_assert(writable<with_pointer>);
static_assert(!writable<std::array<int, 2>>);
static_assert(!writable<char const*>);
static_assert(!writable<std::vector<char const*>>);
static_assert(!writable<std::set<int>>);

constexpr plain_sample filled_plain() {
    plain_sample sample{.verbose = true, .big = 12, .small = -3, .ratio = 2.5, .name = "n\"q", .mode = level::loud, .maybe = level::loud};
    sample.timeouts.idle = std::chrono::seconds{90};
    sample.history.push_back({.idle = milliseconds{1500}, .max_messages = 7});
    sample.slots.push_back(std::nullopt);
    sample.slots.push_back(5);
    return sample;
}

constexpr bool round_trips(plain_sample const& sample) {
    auto const back = read<plain_sample>(to_value(sample));
    return back && *back == sample;
}


const suite writer_suite = [] {
    "capability_a_value_is_written_member_by_member_with_absent_optionals_omitted"_test = [] {
        expect(write(to_value(headers{.api_key = "k"})) == R"({"x-api-key":"k"})");
        expect(write(to_value(headers{.api_key = "k", .trace = "t"})) == R"({"x-api-key":"k","trace":"t"})");
    };

    "capability_enums_are_written_by_name_and_durations_in_the_largest_exact_unit"_test = [] {
        auto const object_text = write(to_value(filled_plain()));

        expect(object_text.contains(R"("mode":"loud")"));
        expect(object_text.contains(R"("idle":"90s")"));
        expect(object_text.contains(R"("idle":"1500ms")"));
        expect(write(to_value(limits{.idle = std::chrono::minutes{10}})).contains(R"("idle":"10m")"));
        expect(write(to_value(limits{.idle = std::chrono::hours{2}})).contains(R"("idle":"2h")"));
        expect(write(to_value(limits{.idle = milliseconds{0}})).contains(R"("idle":"0ms")"));
    };

    "capability_a_value_reads_back_to_the_same_value_without_going_through_text"_test = [] {
        expect(round_trips(filled_plain()));
        expect(round_trips(plain_sample{}));
    };

    "capability_a_written_document_reads_back_to_the_same_value"_test = [] {
        auto const sample = filled_plain();
        auto const back = read_text<plain_sample>(write(to_value(sample)));

        expect(back.has_value());
        expect(*back == sample);
    };

    "capability_annotations_rename_flatten_skip_and_redact_a_member"_test = [] {
        annotated_sample sample{.identifier = "i", .extra = {.api_key = "k"}, .scratch = 9, .secret = "hunter2"};
        sample.by_level.emplace(level::loud, "l");
        sample.by_name.emplace("a", std::vector<std::string>{"x", "y"});

        expect(write(to_value(sample)) ==
               R"({"id":"i","x-api-key":"k","secret":"<redacted>","by_level":{"loud":"l"},"by_name":{"a":["x","y"]}})");
    };

    "capability_the_reader_honours_key_flatten_and_skip"_test = [] {
        auto const back = read_text<annotated_sample>(
            R"({"id":"i","x-api-key":"k","trace":"t","secret":"s","by_level":{"quiet":"q"},"by_name":{}})");

        expect(back.has_value());
        expect(back->identifier == "i");
        expect(back->extra.api_key == "k");
        expect(back->extra.trace == std::optional<std::string>{"t"});
        expect(back->secret == "s");
        expect(back->by_level.at(level::quiet) == "q");
        expect(back->scratch == 0);
    };

    "capability_the_wire_name_replaces_the_member_name_for_unknown_key_checks"_test = [] {
        expect(read_text<headers>(R"({"api_key":"k"})").error().message == "unknown key");
        expect(read_text<annotated_sample>(R"({"scratch":1})").error().message == "unknown key");
        expect(read_text<annotated_sample>(R"({"x-api-key":"k","zzz":1})").error().path == "zzz");
        expect(read_text<annotated_sample>(R"({"zzz":1})", {}, {.allow_unknown_keys = true}).has_value());
    };

    "capability_an_enum_keyed_map_rejects_an_unknown_key_with_its_path"_test = [] {
        auto const failed = read_text<annotated_sample>(R"({"by_level":{"nope":"x"}})");

        expect(!failed.has_value());
        expect(failed.error().path == "by_level.nope");
        expect(failed.error().message == "not one of the allowed names");
    };

    "limit_an_unsigned_count_above_the_signed_range_saturates"_test = [] {
        auto const written = to_value(plain_sample{.big = std::numeric_limits<std::uint64_t>::max()});

        expect(count_at(written, {"big"}) == std::optional<std::uint64_t>{std::numeric_limits<std::int64_t>::max()});
    };

    "capability_a_pointer_member_is_written_only_when_it_is_skipped"_test = [] {
        expect(write(to_value(with_pointer{})) == R"({"count":1})");
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}

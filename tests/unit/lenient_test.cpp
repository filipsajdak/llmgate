#include <boost/ut.hpp>

import llmgate.json;
import llmgate.lenient;
import std;

namespace {

using namespace boost::ut;
namespace json = llmgate::json;
namespace lenient = llmgate::lenient;

struct inner {
    std::uint32_t narrow = 0;
    std::optional<double> ratio{};
};

struct sample {
    [[= json::required]] std::int64_t stamp = 0;
    [[= json::key{"wire_name"}]] std::string name{};
    std::uint64_t count = 0;
    std::int8_t small = 0;
    bool flag = false;
    [[= json::flatten]] inner shared{};
    inner nested{};
    [[= json::skip]] int hidden = 7;
};

static_assert(lenient::readable<sample> && !lenient::readable<std::string> && !lenient::readable<std::vector<int>>);

std::expected<sample, json::read_error> read_text(std::string_view const text) {
    return lenient::read<sample>(*json::parse(text, {.jsonc = false}));
}

const suite lenient_suite = [] {
    "capability_a_well_typed_object_reads_like_json_read"_test = [] {
        auto const got = read_text(R"({"stamp":9,"wire_name":"n","count":4,"small":-3,"flag":true,"narrow":5,"ratio":0.5,"nested":{"narrow":6},"unknown":[1]})");

        expect(got.has_value());
        expect(got->stamp == 9 && got->name == "n" && got->count == 4_u && got->small == -3 && got->flag);
        expect(got->shared.narrow == 5_u && got->shared.ratio == std::optional<double>{0.5} && got->nested.narrow == 6_u && got->hidden == 7);
    };

    "limit_a_wrong_typed_member_reads_as_its_default_and_the_others_survive"_test = [] {
        auto const got = read_text(R"({"stamp":1,"wire_name":4,"count":"x","small":1.5,"flag":"yes","narrow":null,"ratio":"high","nested":3})");

        expect(got.has_value());
        expect(got->name.empty() && got->count == 0_u && got->small == 0 && !got->flag && got->shared.narrow == 0_u && !got->shared.ratio);
        expect(got->nested.narrow == 0_u);
    };

    "capability_a_count_accepts_a_fraction_and_refuses_a_negative_number"_test = [] {
        expect(read_text(R"({"stamp":1,"count":7.9})")->count == 7_u);
        expect(read_text(R"({"stamp":1,"count":-1})")->count == 0_u);
    };

    "limit_an_out_of_range_number_is_clamped_to_the_member_type"_test = [] {
        auto const got = read_text(R"({"stamp":1,"small":1000,"narrow":99999999999})");

        expect(got->small == std::numeric_limits<std::int8_t>::max());
        expect(got->shared.narrow == std::numeric_limits<std::uint32_t>::max());
        expect(read_text(R"({"stamp":1,"small":-1000})")->small == std::numeric_limits<std::int8_t>::min());
    };

    "capability_an_integer_or_a_fraction_reads_into_an_optional_real"_test = [] {
        expect(read_text(R"({"stamp":1,"ratio":3})")->shared.ratio == std::optional<double>{3.0});
        expect(read_text(R"({"stamp":1,"ratio":null})")->shared.ratio == std::nullopt);
    };

    "limit_a_missing_required_member_or_a_non_object_is_an_error"_test = [] {
        expect(!read_text(R"({"count":1})") && !read_text(R"({"stamp":"1"})") && !read_text("[1]") && !read_text("3"));
        expect(read_text("[1]").error().message == "expected an object");
    };

    "limit_the_first_of_two_equal_keys_decides_even_when_it_is_wrong_typed"_test = [] {
        expect(read_text(R"({"stamp":1,"count":"x","count":5})")->count == 0_u);
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}

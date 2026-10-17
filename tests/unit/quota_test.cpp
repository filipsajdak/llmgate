#include <boost/ut.hpp>

import llmgate.json;
import llmgate.quota;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate;

constexpr std::string_view free_plan = R"({"login":"someone","copilot_plan":"individual","access_type_sku":"free_limited_copilot",
"quota_reset_date":"2026-11-01","unknown_future_key":{"a":[1,2]},
"quota_snapshots":{"chat":{"entitlement":200,"remaining":196,"percent_remaining":98.2,"unlimited":false,"has_quota":true,"extra":1},
"completions":{"entitlement":2000,"remaining":1983,"percent_remaining":99.1,"unlimited":false,"has_quota":true},
"premium_interactions":{"entitlement":0,"remaining":0,"percent_remaining":0.0,"unlimited":false,"has_quota":false}}})";

constexpr std::string_view paid_plan = R"({"copilot_plan":"business","access_type_sku":"copilot_for_business_seat_quota","quota_reset_date":"2026-12-01",
"quota_snapshots":{"chat":{"unlimited":true,"has_quota":true,"entitlement":0,"remaining":0},
"premium_interactions":{"entitlement":300,"quota_remaining":42.5,"has_quota":true,"unlimited":false}}})";

constexpr std::size_t flood = 40;

const suite quota_suite = [] {
    "capability_the_free_plan_reply_is_read_with_unknown_keys_ignored"_test = [] {
        auto const found = quota::parse(free_plan);

        expect(found.has_value());
        expect(found->plan == "individual" && found->sku == "free_limited_copilot" && found->reset_date == "2026-11-01");
        expect(found->quotas.size() == 3_u);
        expect(found->quotas[0].remaining == 196_u && found->quotas[0].entitlement == 200_u && found->quotas[0].percent_remaining == 98);
    };

    "capability_the_remaining_count_falls_back_to_the_fractional_field_and_the_percent_is_computed"_test = [] {
        auto const found = quota::parse(paid_plan);

        expect(found->quotas[1].remaining == 42_u);
        expect(found->quotas[1].percent_remaining == 14);
        expect(found->quotas[0].unlimited);
    };

    "capability_a_low_allowance_is_flagged_but_an_unlimited_or_absent_one_is_not"_test = [] {
        expect(quota::any_low(*quota::parse(paid_plan)));
        expect(!quota::any_low(*quota::parse(free_plan)));
    };

    "capability_the_report_names_each_allowance_with_remaining_total_and_reset_date"_test = [] {
        auto const drawn = quota::render(*quota::parse(free_plan));

        expect(drawn.contains("plan: individual (free_limited_copilot)"));
        expect(drawn.contains("resets: 2026-11-01"));
        expect(drawn.contains("chat: 196 of 200 left (98%)"));
        expect(drawn.contains("premium requests: not included in this plan"));
    };

    "limit_a_reply_without_quota_snapshots_is_unusable"_test = [] {
        expect(!quota::parse(R"({"copilot_plan":"individual"})"));
        expect(!quota::parse("<html>"));
        expect(!quota::parse(""));
    };

    "limit_control_bytes_in_the_reply_never_reach_the_report"_test = [] {
        auto const drawn = quota::render(*quota::parse(R"({"copilot_plan":"a\u001b[31mred\u0007","quota_snapshots":{"x\u001b":{"has_quota":true}}})"));

        expect(drawn.find('\x1b') == std::string::npos && drawn.find('\x07') == std::string::npos);
    };

    "limit_at_most_sixteen_allowances_are_kept"_test = [] {
        std::string many = R"({"quota_snapshots":{)";
        for (std::size_t index = 0; index < flood; ++index) {
            many += (index == 0 ? "" : ",") + std::format(R"("q{}":{{"has_quota":true}})", index);
        }

        expect(quota::parse(many + "}}")->quotas.size() == quota::most_allowances);
    };

    "capability_the_json_form_parses_back_and_carries_the_numbers"_test = [] {
        auto const text = quota::to_json(*quota::parse(free_plan));
        auto const again = json::parse(text, {.jsonc = false});

        expect(again.has_value());
        expect(json::text_at(*again, {"plan"}) == "individual");
        expect(text.contains(R"("remaining": 196)"));
    };

    "capability_the_json_form_keeps_its_bytes_and_key_order"_test = [] {
        constexpr std::string_view golden = R"({
  "plan": "individual",
  "sku": "free_limited_copilot",
  "reset_date": "2026-11-01",
  "quotas": [
    {
      "name": "chat",
      "unlimited": false,
      "has_quota": true,
      "entitlement": 200,
      "remaining": 196,
      "percent_remaining": 98
    },
    {
      "name": "completions",
      "unlimited": false,
      "has_quota": true,
      "entitlement": 2000,
      "remaining": 1983,
      "percent_remaining": 99
    },
    {
      "name": "premium_interactions",
      "unlimited": false,
      "has_quota": false,
      "entitlement": 0,
      "remaining": 0,
      "percent_remaining": 0
    }
  ]
}
)";

        expect(quota::to_json(*quota::parse(free_plan)) == golden);
        expect(quota::to_json(quota::summary{}) == "{\n  \"plan\": \"\",\n  \"sku\": \"\",\n  \"reset_date\": \"\",\n  \"quotas\": []\n}\n");
    };

    "capability_allowances_keep_the_order_the_reply_listed_them_in"_test = [] {
        auto const found = quota::parse(free_plan);

        expect(found->quotas.size() == 3_u);
        expect(found->quotas[0].name == "chat" && found->quotas[1].name == "completions" && found->quotas[2].name == "premium_interactions");
    };

    "limit_wrong_typed_reply_fields_read_as_zero_false_or_empty_one_by_one"_test = [] {
        auto const found = quota::parse(
            R"({"copilot_plan":5,"access_type_sku":"s","quota_snapshots":{"x":{"entitlement":"many","remaining":"q","quota_remaining":7,"unlimited":"yes","has_quota":1,"percent_remaining":"high"},"skipped":3,"y":{"entitlement":-1,"remaining":null,"quota_remaining":4,"has_quota":true,"percent_remaining":null}}})");

        expect(found.has_value() && found->plan.empty() && found->sku == "s" && found->quotas.size() == 2_u);
        auto const& first = found->quotas[0];
        auto const& second = found->quotas[1];
        expect(first.name == "x" && first.remaining == 7_u && first.entitlement == 0_u && !first.unlimited && !first.has_quota && first.percent_remaining == 0);
        expect(second.name == "y" && second.remaining == 4_u && second.entitlement == 0_u && second.has_quota && second.percent_remaining == 0);
    };

    "limit_a_given_percent_wins_over_the_computed_one_and_is_clamped"_test = [] {
        auto const found = quota::parse(R"({"quota_snapshots":{"a":{"entitlement":10,"remaining":5,"percent_remaining":33},"b":{"entitlement":10,"remaining":5,"percent_remaining":250.5}}})");

        expect(found->quotas[0].percent_remaining == 33 && found->quotas[1].percent_remaining == quota::full_percent);
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}

#include <boost/ut.hpp>

import llmgate.usage;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate::usage;

constexpr std::int64_t noon = 1760702400;

const suite usage_suite = [] {
    "capability_a_stream_is_counted_from_its_start_and_its_final_delta"_test = [] {
        observer seen;

        seen.on_event("message_start", R"({"message":{"model":"claude-x","usage":{"input_tokens":20,"output_tokens":1,"cache_read_input_tokens":6}}})");
        seen.on_event("content_block_delta", R"({"usage":{"output_tokens":500}})");
        seen.on_event("message_delta", R"({"usage":{"output_tokens":9}})");

        expect(seen.model() == "claude-x");
        expect(seen.used() == tokens{.input = 20, .output = 9, .cache_read = 6});
    };

    "limit_a_later_delta_can_only_raise_a_count"_test = [] {
        observer seen;
        seen.on_event("message_delta", R"({"usage":{"output_tokens":9}})");

        seen.on_event("message_delta", R"({"usage":{"output_tokens":4}})");

        expect(seen.used().output == 9_u);
    };

    "capability_a_plain_reply_is_counted_from_its_usage_member"_test = [] {
        observer seen;

        seen.on_body(R"({"model":"m","usage":{"input_tokens":3,"output_tokens":4,"cache_creation_input_tokens":5}})");

        expect(seen.used() == tokens{.input = 3, .output = 4, .cache_creation = 5});
        expect(seen.used().counted() == 12_u);
    };

    "limit_damaged_or_unexpected_usage_data_counts_as_nothing"_test = [] {
        observer seen;
        seen.on_body("not json");
        seen.on_body(R"({"usage":{"input_tokens":-5,"output_tokens":"many"}})");
        seen.on_event("message_start", "[");

        expect(seen.used() == tokens{});
    };

    "capability_the_session_comes_from_either_user_id_format"_test = [] {
        expect(session_of(R"({"metadata":{"user_id":"user_9f_account_a1_session_77ab-cd"}})") == "77ab-cd");
        expect(session_of(R"({"metadata":{"user_id":"{\"device_id\":\"d\",\"account_uuid\":\"\",\"session_id\":\"s-9\"}"}})") == "s-9");
    };

    "limit_a_missing_odd_or_oversized_session_is_dropped_rather_than_stored"_test = [] {
        expect(session_of(R"({"messages":[]})").empty());
        expect(session_of(R"({"metadata":{"user_id":"user_x_session_has space"}})").empty());
        expect(session_of(R"({"metadata":{"user_id":"user_x_session_)" + std::string(65, 'a') + R"("}})").empty());
        expect(session_of(R"({"metadata":{"user_id":"a"},"metadata":{"user_id":"b_session_c"}})").empty());
    };

    "capability_models_fall_into_roles_regardless_of_case"_test = [] {
        expect(role_of("claude-OPUS-5-5") == "opus");
        expect(role_of("claude-sonnet-5.5") == "sonnet");
        expect(role_of("claude-haiku-5-5") == "haiku");
        expect(role_of("gpt-4o") == "other");
    };

    "defect_control_characters_in_model_names_never_reach_the_report"_test = [] {
        ledger book;
        book.add({.time = noon, .requested = "a\x1b]0;pwned\x07", .model = "m\x1b[2J\n"});
        auto const loaded = load(R"({"time":1760702400,"model":"x\u001b[31m","requested":"r"})" "\n");

        auto const text = report(book, 0, day_of(noon)) + report(loaded.loaded, 0, day_of(noon));

        expect(std::ranges::none_of(text, [](char const c) { return c != '\n' && (static_cast<unsigned char>(c) < 0x20 || c == 0x7f); }));
        expect(text.contains("m?[2J?") && text.contains("x?[31m"));
    };

    "capability_a_record_survives_the_ledger_line_round_trip"_test = [] {
        record const entry{.time = noon, .requested = "sonnet", .model = "claude-sonnet-5-5", .role = "sonnet", .provider = "copilot", .session = "s1",
                           .used = {.input = 1, .output = 2, .cache_read = 3, .cache_creation = 4}, .duration_ms = 350, .status = 200, .repairs = 1};

        auto const line = to_line(entry);

        expect(line.ends_with("}\n") && std::ranges::count(line, '\n') == 1);
        expect(from_line(line) == entry);
    };

    "capability_a_ledger_line_written_before_providers_existed_reads_back_with_no_provider"_test = [] {
        constexpr std::string_view old_line = R"({"time":1760659200,"requested":"opus","model":"m","role":"opus","session":"","input":1,"output":2,"cache_read":0,"cache_creation":0,"duration_ms":5,"status":200,"repairs":0})";

        auto const entry = from_line(old_line);

        expect(entry.has_value() && entry->provider.empty() && entry->model == "m" && entry->used.input == 1_u);
    };

    "defect_a_hostile_provider_name_in_a_ledger_line_is_neutralised"_test = [] {
        auto const entry = from_line(to_line({.time = noon, .provider = "evil\u001bname"}));

        expect(entry.has_value() && entry->provider == "evil?name");
    };

    "limit_unreadable_ledger_lines_are_skipped_and_counted_never_fatal"_test = [] {
        auto const text = to_line({.time = noon, .model = "m"}) + "{broken\n" + R"({"no_time":1})" + "\n\n" + to_line({.time = noon, .model = "m"});

        auto const result = load(text);

        expect(result.skipped == 2_u);
        expect(result.loaded.on_day(day_of(noon)).requests == 2_u);
    };

    "capability_days_and_months_follow_the_utc_calendar"_test = [] {
        expect(iso_date(day_of(noon)) == "2025-10-17");
        expect(iso_date(day_of(1709164800)) == "2024-02-29");
        expect(iso_date(day_of(1709251200)) == "2024-03-01");
        expect(iso_date(day_of(-1)) == "1969-12-31");
        expect(month_of(day_of(1759276800)) != month_of(day_of(1759276799)));
    };

    "capability_the_ledger_adds_per_day_and_per_month_and_groups_by_model"_test = [] {
        ledger book;
        book.add({.time = noon, .model = "a", .used = {.input = 10, .output = 5}});
        book.add({.time = noon + 60, .model = "a", .used = {.input = 1}});
        book.add({.time = noon + 60, .model = "b", .used = {.output = 2}});
        book.add({.time = noon + seconds_per_day, .model = "a", .used = {.input = 100}});

        expect(book.on_day(day_of(noon)).requests == 3_u);
        expect(book.on_day(day_of(noon)).used.counted() == 18_u);
        expect(book.in_month(month_of(day_of(noon))).used.counted() == 118_u);
        expect(book.groups().size() == 3_u);
        expect(book.on_day(day_of(noon) + 5).requests == 0_u);
    };

    "limit_counts_saturate_instead_of_wrapping"_test = [] {
        tokens big{.input = std::numeric_limits<std::uint64_t>::max()};

        big += tokens{.input = 5};

        expect(big.input == std::numeric_limits<std::uint64_t>::max());
        expect(big.counted() == std::numeric_limits<std::uint64_t>::max());
    };

    "capability_a_budget_moves_from_ok_to_warning_to_exhausted"_test = [] {
        budget const limits{.daily_tokens = 1000, .warn_percent = 80};

        expect(evaluate(limits, 799, 0).state == level::ok);
        expect(evaluate(limits, 800, 0).state == level::warning);
        expect(evaluate(limits, 999, 0).state == level::warning);
        expect(evaluate(limits, 1000, 0).state == level::exhausted);
        expect(evaluate(limits, 5000, 0).percent == 500_u);
    };

    "capability_the_tighter_of_the_two_windows_decides"_test = [] {
        budget const limits{.daily_tokens = 1000, .monthly_tokens = 10000};

        auto const monthly = evaluate(limits, 100, 9500);
        auto const daily = evaluate(limits, 900, 100);

        expect(monthly.window == "monthly" && monthly.state == level::warning);
        expect(daily.window == "daily" && daily.state == level::warning);
        expect(evaluate({}, 1000000, 1000000).limit == 0_u);
    };

    "capability_the_refusal_says_which_window_ran_out_and_when_it_resets"_test = [] {
        auto const daily = refusal_message(evaluate({.daily_tokens = 1000000}, 1200000, 0));
        auto const monthly = refusal_message(evaluate({.monthly_tokens = 5000}, 0, 6000));

        expect(daily == "llmgate budget used up: daily 1.2M of 1.0M tokens; resets at 00:00 UTC");
        expect(monthly == "llmgate budget used up: monthly 6k of 5k tokens; resets on the first day of next month (UTC)");
    };

    "capability_the_summary_json_feeds_the_status_line_headline"_test = [] {
        ledger book;
        book.add({.time = noon, .model = "a", .used = {.input = 600, .output = 200, .cache_read = 9999}});

        auto const body = R"({"requests":3,"usage":)" + summary_json(book, {.daily_tokens = 1000}, noon) + "}";
        auto const head = headline_from(body);

        expect(head.today == 800_u);
        expect(head.state == "warning" && head.window == "daily" && head.limit == 1000_u && head.percent == 80_u);
    };

    "limit_a_stats_body_without_usage_gives_an_empty_headline"_test = [] {
        expect(headline_from(R"({"requests":1})").limit == 0_u);
        expect(headline_from("nope").today == 0_u);
    };

    "limit_a_ledger_line_without_counts_reads_as_zero_not_one"_test = [] {
        auto const entry = from_line(R"({"time":5})");

        expect(entry.has_value() && entry->used == tokens{} && entry->repairs == 0_u);
    };

    "limit_dates_stay_right_in_centuries_that_are_not_leap_years_and_before_year_one"_test = [] {
        constexpr std::int64_t last_day_of_february_2100 = 47540;
        constexpr std::int64_t first_day_of_march_0 = -719468;
        constexpr std::int64_t days_in_a_gregorian_era = 146097;

        expect(iso_date(last_day_of_february_2100) == "2100-02-28");
        expect(iso_date(last_day_of_february_2100 + 1) == "2100-03-01");
        expect(iso_date(first_day_of_march_0) == "0-03-01");
        expect(iso_date(first_day_of_march_0 - 1) == "0-02-29");
        expect(iso_date(first_day_of_march_0 - days_in_a_gregorian_era) == "-400-03-01");
    };

    "limit_a_budget_with_nothing_used_still_reports_its_window_and_limit"_test = [] {
        auto const idle = evaluate({.daily_tokens = 1000}, 0, 0);

        expect(idle.limit == 1000_u && idle.window == "daily" && idle.percent == 0_u && idle.state == level::ok);
        expect(evaluate({.daily_tokens = 1, .monthly_tokens = 1000}, 0, 0).limit == 1_u);
    };

    "limit_report_columns_keep_their_widths_in_the_header_the_rows_and_the_total"_test = [] {
        constexpr std::size_t date_width = 12;
        constexpr std::size_t model_width = 22;
        constexpr std::size_t request_width = 9;
        constexpr std::size_t number_width = 12;
        constexpr std::size_t counters = 4;
        constexpr std::size_t line_width = date_width + model_width + request_width + counters * number_width;
        ledger book;
        book.add({.time = noon, .model = "m", .used = {.input = 1}});

        auto const text = report(book, day_of(noon), day_of(noon));
        auto const lines = text | std::views::split('\n') | std::ranges::to<std::vector<std::string>>();

        expect(lines.size() == 4_u);
        expect(lines[0].size() == line_width && lines[1].size() == line_width && lines[2].size() == line_width);
        expect(lines[0].starts_with("date" + std::string(date_width - 4, ' ') + "model" + std::string(model_width - 5, ' ') + std::string(request_width - 8, ' ') + "requests"));
        expect(lines[1].starts_with("2025-10-17  m" + std::string(model_width - 1, ' ') + std::string(request_width - 1, ' ') + "1"));
        expect(lines[2] == "total" + std::string(date_width + model_width - 5, ' ') + std::string(request_width - 1, ' ') + "1" + std::string(number_width - 1, ' ') + "1" +
                               std::string(number_width - 1, ' ') + "0" + std::string(number_width - 1, ' ') + "0" + std::string(number_width - 1, ' ') + "0");
    };

    "limit_the_json_report_is_indented_by_two_spaces"_test = [] {
        ledger book;
        book.add({.time = noon, .model = "m"});

        auto const json = report_json(book, day_of(noon), day_of(noon));

        expect(json.contains("\n  \"days\": 1"));
        expect(!json.contains("\n   \"days\""));
    };

    "capability_the_report_lists_days_models_and_a_total_inside_the_range"_test = [] {
        ledger book;
        book.add({.time = noon, .model = "claude-a", .used = {.input = 1500, .output = 20}});
        book.add({.time = noon + seconds_per_day, .model = "claude-a", .used = {.input = 5}});
        book.add({.time = noon - 30 * seconds_per_day, .model = "old", .used = {.input = 1}});
        auto const last = day_of(noon) + 1;

        auto const text = report(book, last - 1, last);
        auto const json = report_json(book, last - 1, last);

        expect(text.contains("2025-10-17") && text.contains("2025-10-18"));
        expect(!text.contains("old"));
        expect(text.contains("1k"));
        expect(text.contains("total"));
        expect(json.contains(R"("date": "2025-10-17")") && json.contains(R"("days": 2)") && !json.contains("old"));
    };
    "capability_the_ledger_line_keeps_its_bytes_and_key_order"_test = [] {
        constexpr std::string_view golden =
            R"({"time":1760702400,"requested":"sonnet","model":"claude-sonnet-5-5","role":"sonnet","provider":"copilot","session":"s1","input":1,"output":2,"cache_read":3,"cache_creation":4,"duration_ms":350,"status":200,"repairs":1})"
            "\n";
        constexpr std::string_view bare = R"({"time":0,"requested":"","model":"","role":"","provider":"","session":"","input":0,"output":0,"cache_read":0,"cache_creation":0,"duration_ms":0,"status":0,"repairs":0})" "\n";

        auto const line = to_line({.time = noon, .requested = "sonnet", .model = "claude-sonnet-5-5", .role = "sonnet", .provider = "copilot", .session = "s1",
                                   .used = {.input = 1, .output = 2, .cache_read = 3, .cache_creation = 4}, .duration_ms = 350, .status = 200, .repairs = 1});

        expect(line == golden);
        expect(to_line({}) == bare);
    };

    "limit_a_damaged_ledger_line_is_read_field_by_field_as_zero_or_empty"_test = [] {
        auto const entry = from_line(R"({"time":7,"model":3,"input":"x","output":-3,"cache_read":4.7,"cache_creation":null,"duration_ms":"q","status":99999,"repairs":[1]})");

        expect(entry.has_value());
        expect(entry->time == 7 && entry->model.empty() && entry->used == tokens{.cache_read = 4});
        expect(entry->duration_ms == 0 && entry->status == 999 && entry->repairs == 0_u);
        expect(from_line(R"({"time":7,"status":-4})")->status == 0);
        expect(!from_line(R"({"time":"7"})") && !from_line(R"({"time":7.5})") && !from_line("[1]") && !from_line("3"));
    };

    "capability_the_summary_json_keeps_its_bytes_and_key_order"_test = [] {
        constexpr std::string_view warning =
            R"({"today":{"input":600,"output":200,"cache_read":9999,"cache_creation":0,"requests":1},"month":{"input":600,"output":200,"cache_read":9999,"cache_creation":0,"requests":1},"budget":{"state":"warning","window":"daily","used":800,"limit":1000,"percent":80}})";
        constexpr std::string_view idle =
            R"({"today":{"input":0,"output":0,"cache_read":0,"cache_creation":0,"requests":0},"month":{"input":0,"output":0,"cache_read":0,"cache_creation":0,"requests":0},"budget":{"state":"ok","window":"","used":0,"limit":0,"percent":0}})";
        ledger book;
        book.add({.time = noon, .model = "a", .used = {.input = 600, .output = 200, .cache_read = 9999}});

        expect(summary_json(book, {.daily_tokens = 1000}, noon) == warning);
        expect(summary_json({}, {}, noon) == idle);
        expect(summary_json(book, {.monthly_tokens = 700}, noon).ends_with(R"("budget":{"state":"exhausted","window":"monthly","used":800,"limit":700,"percent":114}})"));
    };

    "capability_the_json_report_keeps_its_bytes_and_key_order"_test = [] {
        constexpr std::string_view golden = R"({
  "days": 1,
  "rows": [
    {
      "date": "2025-10-17",
      "model": "m",
      "input": 7,
      "output": 999,
      "cache_read": 1000,
      "cache_creation": 0,
      "requests": 1
    }
  ]
}
)";
        ledger book;
        book.add({.time = noon, .model = "m", .used = {.input = 7, .output = 999, .cache_read = 1000}});

        expect(report_json(book, day_of(noon), day_of(noon)) == golden);
    };

    "capability_the_text_report_keeps_its_bytes"_test = [] {
        ledger book;
        book.add({.time = noon, .model = "m", .used = {.input = 7, .output = 999, .cache_read = 1000}});

        auto const text = report(book, day_of(noon), day_of(noon));

        expect(text == std::format("{:<12}{:<22}{:>9}{:>12}{:>12}{:>12}{:>12}\n", "date", "model", "requests", "input", "output", "cache read", "cache write") +
                           std::format("{:<12}{:<22}{:>9}{:>12}{:>12}{:>12}{:>12}\n", "2025-10-17", "m", "1", "7", "999", "1k", "0") +
                           std::format("{:<34}{:>9}{:>12}{:>12}{:>12}{:>12}\n", "total", "1", "7", "999", "1k", "0"));
    };

    "limit_wrong_typed_stream_counts_read_as_zero_one_by_one"_test = [] {
        observer seen;

        seen.on_body(R"({"usage":{"input_tokens":5,"output_tokens":"many","cache_read_input_tokens":2.5,"cache_creation_input_tokens":-1}})");

        expect(seen.used() == tokens{.input = 5, .cache_read = 2});
    };

    "limit_a_damaged_stats_body_gives_a_headline_of_zero_and_empty_one_by_one"_test = [] {
        auto const head = headline_from(R"({"usage":{"today":{"input":"x","output":2.9,"cache_creation":-1},"budget":{"state":3,"window":"daily","used":"u","limit":10,"percent":5000}}})");

        expect(head.today == 2_u && head.state.empty() && head.window == "daily" && head.used == 0_u && head.limit == 10_u && head.percent == percent_cap);
        expect(headline_from("[]").limit == 0_u && headline_from(R"({"usage":3})").today == 0_u && headline_from(R"({"usage":{"budget":[]}})").window.empty());
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}

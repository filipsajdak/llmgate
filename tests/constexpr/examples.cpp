import llmgate.claude;
import llmgate.json;
import llmgate.quota;
import llmgate.usage;
import std;

namespace {

using namespace llmgate;

constexpr std::string_view sample = R"({"copilot_plan":"individual","access_type_sku":"free_limited_copilot","quota_reset_date":"2026-11-01",
"quota_snapshots":{"chat":{"entitlement":200,"remaining":196,"percent_remaining":98,"unlimited":false,"has_quota":true},
"premium_interactions":{"entitlement":0,"remaining":0,"percent_remaining":0,"unlimited":false,"has_quota":false},
"completions":{"entitlement":2000,"remaining":100,"percent_remaining":5,"unlimited":false,"has_quota":true}}})";

constexpr bool both_observed(std::string_view const start, std::string_view const delta, usage::tokens const expected) {
    usage::observer seen;
    seen.on_event("message_start", start);
    seen.on_event("message_delta", delta);
    return seen.used() == expected;
}

static_assert(both_observed(R"({"type":"message_start","message":{"model":"m","usage":{"input_tokens":10,"cache_read_input_tokens":4,"output_tokens":1}}})",
                            R"({"type":"message_delta","delta":{},"usage":{"output_tokens":7}})", {.input = 10, .output = 7, .cache_read = 4}));
static_assert(both_observed(R"({"message":{"usage":{"output_tokens":9}}})", R"({"usage":{"output_tokens":3}})", {.output = 9}));
static_assert(both_observed("not json", "[]", {}));
static_assert([] {
    usage::observer seen;
    seen.on_body(R"({"model":"claude-x","usage":{"input_tokens":5,"output_tokens":2,"cache_creation_input_tokens":1}})");
    return seen.model() == "claude-x" && seen.used() == usage::tokens{.input = 5, .output = 2, .cache_creation = 1};
}());
static_assert(usage::from_line(R"({"time":1,"model":"m\u001b[2Jx","requested":"\u0007r"})")->model == "m?[2Jx" &&
              usage::from_line(R"({"time":1,"session":"bad\u001bid"})")->session.empty());
static_assert(usage::to_line(*usage::from_line(usage::to_line({.time = 1760000000, .requested = "a", .model = "b", .role = "opus", .session = "s", .used = {1, 2, 3, 4}, .duration_ms = 9, .status = 200, .repairs = 2}))) ==
              usage::to_line({.time = 1760000000, .requested = "a", .model = "b", .role = "opus", .session = "s", .used = {1, 2, 3, 4}, .duration_ms = 9, .status = 200, .repairs = 2}));
static_assert(!usage::from_line("{}") && !usage::from_line("garbage") && usage::from_line(R"({"time":5})")->time == 5);
static_assert(usage::to_line({.time = 7, .model = "m", .used = {.input = 1}, .status = 200}) ==
              R"({"time":7,"requested":"","model":"m","role":"","provider":"","session":"","input":1,"output":0,"cache_read":0,"cache_creation":0,"duration_ms":0,"status":200,"repairs":0,"stop_reason":"","error":"","kind":"","verdict":""})"
              "\n");
static_assert([] {
    usage::ledger book;
    book.add({.time = 100, .model = "b", .used = {.input = 2}});
    book.add({.time = 90000, .model = "a", .used = {.output = 1500}});
    book.add({.time = 90100, .model = "a", .used = {.output = 10}});
    auto const json_text = usage::report_json(book, 1, 1);
    return json_text.contains("\"date\": \"1970-01-02\"") && json_text.contains("\"requests\": 2") && !json_text.contains("1970-01-01");
}());
static_assert(usage::load(usage::to_line({.time = 5, .model = "m"}) + "junk\n\n" + usage::to_line({.time = 6, .model = "m"})).skipped == 1);
static_assert(usage::headline_from(R"({"usage":{"today":{"input":5,"output":2,"cache_read":99,"cache_creation":1},"budget":{"state":"warning","window":"daily","used":8,"limit":10,"percent":80}}})").today == 8);
static_assert(usage::headline_from("x").state.empty() && usage::headline_from("{}").limit == 0);
static_assert(usage::headline_from(R"({"usage":{"today":{"input":"x","output":2},"budget":{"state":3,"window":"daily","percent":5000}}})").today == 2 &&
              usage::headline_from(R"({"usage":{"today":{"input":"x","output":2},"budget":{"state":3,"window":"daily","percent":5000}}})").percent == usage::percent_cap);
static_assert(usage::summary_json({}, {}, 0) ==
              R"({"today":{"input":0,"output":0,"cache_read":0,"cache_creation":0,"requests":0},"month":{"input":0,"output":0,"cache_read":0,"cache_creation":0,"requests":0},"budget":{"state":"ok","window":"","used":0,"limit":0,"percent":0}})");

static_assert(quota::parse(sample).has_value() && quota::parse(sample)->quotas.size() == 3 && quota::parse(sample)->quotas[0].percent_remaining == 98);
static_assert(quota::parse(sample)->plan == "individual" && quota::parse(sample)->reset_date == "2026-11-01" && quota::any_low(*quota::parse(sample)));
static_assert(!quota::parse("{}") && !quota::parse("[") && !quota::parse(R"({"quota_snapshots":3})"));
static_assert(quota::describe(quota::parse(sample)->quotas[1]) == "premium requests: not included in this plan");
static_assert(quota::describe(quota::parse(sample)->quotas[2]) == "completions: 100 of 2000 left (5%) - low");
static_assert(quota::parse(R"({"quota_snapshots":{"x":{"unlimited":true,"has_quota":true}}})")->quotas[0].unlimited);
static_assert(!quota::any_low(*quota::parse(R"({"quota_snapshots":{"x":{"unlimited":true,"has_quota":true,"percent_remaining":0}}})")));
static_assert(quota::parse(R"({"quota_snapshots":{"x":{"entitlement":10,"remaining":5,"has_quota":true}}})")->quotas[0].percent_remaining == 50);
static_assert(quota::parse(R"({"quota_snapshots":{"x":{"entitlement":10,"quota_remaining":4,"remaining":"q","has_quota":true}}})")->quotas[0].remaining == 4);
static_assert(quota::parse(R"({"quota_snapshots":{"x":{"percent_remaining":-5,"has_quota":true}}})")->quotas[0].percent_remaining == 0);
static_assert(quota::parse(R"({"quota_snapshots":{"x":{"percent_remaining":900}}})")->quotas[0].percent_remaining == quota::full_percent);
static_assert(quota::render(*quota::parse(R"({"copilot_plan":"a\u001b[31m","quota_snapshots":{}})")).find('\x1b') == std::string::npos);

constexpr claude::environment_list sample_environment() {
    return claude::environment({}, "u", "k", "/p");
}

static_assert(json::write(claude::merge_vscode(json::value{}, sample_environment())).contains("claudeCode.disableLoginPrompt"));
static_assert(json::write(claude::vscode_snippet(sample_environment())).contains(R"({"name":"CLAUDE_CONFIG_DIR","value":"/p"})"));
static_assert([] {
    auto const once = claude::merge_vscode(json::value{}, sample_environment());
    return json::write(once) == json::write(claude::merge_vscode(once, sample_environment()));
}());
static_assert(json::write(claude::vscode_snippet(sample_environment())) == json::write(claude::merge_vscode(json::value{}, sample_environment())));

}  // namespace

int main() {}

export module llmgate.quota;

import std;
import llmgate.json;
import llmgate.text;
import llmgate.usage;

export namespace llmgate::quota {

constexpr int low_percent = 20;
constexpr int full_percent = 100;
constexpr std::size_t most_allowances = 16;

struct allowance {
    std::string name;
    bool unlimited = false;
    bool has_quota = false;
    std::uint64_t entitlement = 0;
    std::uint64_t remaining = 0;
    int percent_remaining = 0;

    friend constexpr bool operator==(allowance const&, allowance const&) = default;
};

struct summary {
    std::string plan;
    std::string sku;
    std::string reset_date;
    std::vector<allowance> allowances;
};

}  // namespace llmgate::quota

namespace llmgate::quota::detail {

constexpr std::optional<double> real_at(json::value const& root, std::initializer_list<std::string_view> const path) {
    auto const* found = json::at(root, path);
    if (found == nullptr) {
        return std::nullopt;
    }
    if (auto const* whole = std::get_if<std::int64_t>(&found->data)) {
        return static_cast<double>(*whole);
    }
    if (auto const* real = std::get_if<double>(&found->data)) {
        return *real;
    }
    return std::nullopt;
}

constexpr bool flag_at(json::value const& root, std::string_view const key) {
    auto const* found = json::at(root, {key});
    auto const* flag = found == nullptr ? nullptr : std::get_if<bool>(&found->data);
    return flag != nullptr && *flag;
}

constexpr int clamp_percent(double const value) {
    if (!(value > 0.0)) {
        return 0;
    }
    return value >= static_cast<double>(full_percent) ? full_percent : static_cast<int>(value);
}

constexpr int percent_of(json::value const& source, std::uint64_t const entitlement, std::uint64_t const remaining) {
    if (auto const given = real_at(source, {"percent_remaining"})) {
        return clamp_percent(*given);
    }
    if (entitlement == 0) {
        return 0;
    }
    return clamp_percent(static_cast<double>(remaining) * full_percent / static_cast<double>(entitlement));
}

constexpr allowance allowance_of(std::string_view const name, json::value const& source) {
    auto const entitlement = json::count_at(source, {"entitlement"}).value_or(0);
    auto const remaining = json::count_at(source, {"remaining"}).or_else([&source] { return json::count_at(source, {"quota_remaining"}); }).value_or(0);
    return {.name = usage::label(name),
            .unlimited = flag_at(source, "unlimited"),
            .has_quota = flag_at(source, "has_quota"),
            .entitlement = entitlement,
            .remaining = remaining,
            .percent_remaining = percent_of(source, entitlement, remaining)};
}

constexpr std::string_view title(std::string_view const name) {
    return name == "premium_interactions" ? "premium requests" : name;
}

}  // namespace llmgate::quota::detail

export namespace llmgate::quota {

constexpr std::optional<summary> parse(std::string_view const text) {
    auto const document = json::parse(text, {.jsonc = false});
    if (!document) {
        return std::nullopt;
    }
    auto const* snapshots = json::at(*document, {"quota_snapshots"});
    auto const* members = snapshots == nullptr ? nullptr : std::get_if<json::object>(&snapshots->data);
    if (members == nullptr) {
        return std::nullopt;
    }
    summary found{.plan = usage::label(json::text_at(*document, {"copilot_plan"})),
                  .sku = usage::label(json::text_at(*document, {"access_type_sku"})),
                  .reset_date = usage::label(json::text_at(*document, {"quota_reset_date"})),
                  .allowances = {}};
    for (auto const& [name, source] : *members) {
        if (found.allowances.size() < most_allowances && std::holds_alternative<json::object>(source.data)) {
            found.allowances.push_back(detail::allowance_of(name, source));
        }
    }
    return found;
}

constexpr bool is_low(allowance const& item) {
    return item.has_quota && !item.unlimited && item.percent_remaining <= low_percent;
}

constexpr bool any_low(summary const& found) {
    return std::ranges::any_of(found.allowances, is_low);
}

constexpr std::string describe(allowance const& item) {
    auto const title = std::string{detail::title(item.name)};
    if (item.unlimited) {
        return title + ": unlimited";
    }
    if (!item.has_quota) {
        return title + ": not included in this plan";
    }
    return title + ": " + text::decimal(item.remaining) + " of " + text::decimal(item.entitlement) + " left (" + text::decimal(item.percent_remaining) + "%)" +
           (is_low(item) ? " - low" : "");
}

constexpr std::string render(summary const& found) {
    std::string out = "plan: " + (found.plan.empty() ? std::string{"unknown"} : found.plan) + (found.sku.empty() ? "" : " (" + found.sku + ")") + "\n";
    if (!found.reset_date.empty()) {
        out += "resets: " + found.reset_date + "\n";
    }
    for (auto const& item : found.allowances) {
        out += describe(item) + "\n";
    }
    return out;
}

constexpr json::value to_json_value(summary const& found) {
    json::array items;
    for (auto const& item : found.allowances) {
        items.push_back(json::make_object({{"name", json::make_string(item.name)},
                                           {"unlimited", json::value{item.unlimited}},
                                           {"has_quota", json::value{item.has_quota}},
                                           {"entitlement", json::value{static_cast<std::int64_t>(item.entitlement)}},
                                           {"remaining", json::value{static_cast<std::int64_t>(item.remaining)}},
                                           {"percent_remaining", json::value{static_cast<std::int64_t>(item.percent_remaining)}}}));
    }
    return json::make_object({{"plan", json::make_string(found.plan)},
                              {"sku", json::make_string(found.sku)},
                              {"reset_date", json::make_string(found.reset_date)},
                              {"quotas", json::value{std::move(items)}}});
}

constexpr std::string to_json(summary const& found) {
    return json::write(to_json_value(found), 2);
}

constexpr std::string_view unavailable_message = "the Copilot quota endpoint did not answer or returned an unexpected reply";

}  // namespace llmgate::quota

namespace llmgate::quota {

constexpr std::string_view sample = R"({"copilot_plan":"individual","access_type_sku":"free_limited_copilot","quota_reset_date":"2026-11-01",
"quota_snapshots":{"chat":{"entitlement":200,"remaining":196,"percent_remaining":98,"unlimited":false,"has_quota":true},
"premium_interactions":{"entitlement":0,"remaining":0,"percent_remaining":0,"unlimited":false,"has_quota":false},
"completions":{"entitlement":2000,"remaining":100,"percent_remaining":5,"unlimited":false,"has_quota":true}}})";

static_assert(parse(sample).has_value() && parse(sample)->allowances.size() == 3 && parse(sample)->allowances[0].percent_remaining == 98);
static_assert(parse(sample)->plan == "individual" && parse(sample)->reset_date == "2026-11-01" && any_low(*parse(sample)));
static_assert(!parse("{}") && !parse("[") && !parse(R"({"quota_snapshots":3})"));
static_assert(describe(parse(sample)->allowances[1]) == "premium requests: not included in this plan");
static_assert(describe(parse(sample)->allowances[2]) == "completions: 100 of 2000 left (5%) - low");
static_assert(parse(R"({"quota_snapshots":{"x":{"unlimited":true,"has_quota":true}}})")->allowances[0].unlimited);
static_assert(!any_low(*parse(R"({"quota_snapshots":{"x":{"unlimited":true,"has_quota":true,"percent_remaining":0}}})")));
static_assert(parse(R"({"quota_snapshots":{"x":{"entitlement":10,"remaining":5,"has_quota":true}}})")->allowances[0].percent_remaining == 50);
static_assert(parse(R"({"quota_snapshots":{"x":{"percent_remaining":-5,"has_quota":true}}})")->allowances[0].percent_remaining == 0);
static_assert(parse(R"({"quota_snapshots":{"x":{"percent_remaining":900}}})")->allowances[0].percent_remaining == full_percent);
static_assert(render(*parse(R"({"copilot_plan":"a\u001b[31m","quota_snapshots":{}})")).find('\x1b') == std::string::npos);

}  // namespace llmgate::quota

template class std::optional<llmgate::quota::summary>;

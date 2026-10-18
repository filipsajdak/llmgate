export module llmgate.quota;

import std;
import llmgate.json;
import llmgate.lenient;
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
    std::vector<allowance> quotas;
};

}  // namespace llmgate::quota

namespace llmgate::quota::detail {

struct github_snapshot {
    std::uint64_t entitlement = 0;
    std::optional<std::uint64_t> remaining{};
    std::optional<std::uint64_t> quota_remaining{};
    bool unlimited = false;
    bool has_quota = false;
    std::optional<double> percent_remaining{};
};

struct github_reply {
    [[= json::key{"copilot_plan"}]] std::string plan{};
    [[= json::key{"access_type_sku"}]] std::string sku{};
    [[= json::key{"quota_reset_date"}]] std::string reset_date{};
};

constexpr int clamp_percent(double const value) {
    if (!(value > 0.0)) {
        return 0;
    }
    return value >= static_cast<double>(full_percent) ? full_percent : static_cast<int>(value);
}

constexpr std::uint64_t remaining_of(github_snapshot const& reading) {
    return reading.remaining.value_or(reading.quota_remaining.value_or(0));
}

constexpr int percent_of(github_snapshot const& reading) {
    if (reading.percent_remaining) {
        return clamp_percent(*reading.percent_remaining);
    }
    if (reading.entitlement == 0) {
        return 0;
    }
    return clamp_percent(static_cast<double>(remaining_of(reading)) * full_percent / static_cast<double>(reading.entitlement));
}

constexpr allowance allowance_of(std::string_view const name, json::value const& source) {
    auto const reading = lenient::read<github_snapshot>(source).value_or(github_snapshot{});
    return {.name = usage::label(name),
            .unlimited = reading.unlimited,
            .has_quota = reading.has_quota,
            .entitlement = reading.entitlement,
            .remaining = remaining_of(reading),
            .percent_remaining = percent_of(reading)};
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
    auto const head = lenient::read<detail::github_reply>(*document).value_or(detail::github_reply{});
    summary found{.plan = usage::label(head.plan), .sku = usage::label(head.sku), .reset_date = usage::label(head.reset_date), .quotas = {}};
    for (auto const& [name, source] : *members) {
        if (found.quotas.size() < most_allowances && std::holds_alternative<json::object>(source.data)) {
            found.quotas.push_back(detail::allowance_of(name, source));
        }
    }
    return found;
}

constexpr bool is_low(allowance const& item) {
    return item.has_quota && !item.unlimited && item.percent_remaining <= low_percent;
}

constexpr bool any_low(summary const& found) {
    return std::ranges::any_of(found.quotas, is_low);
}

constexpr std::string describe(allowance const& item) {
    auto const title = std::string{detail::title(item.name)};
    if (item.unlimited) {
        return title + ": unlimited";
    }
    if (!item.has_quota) {
        return title + ": not included in this plan";
    }
    std::string line;
    text::append_all(line, title, ": ", text::decimal(item.remaining), " of ", text::decimal(item.entitlement), " left (", text::decimal(item.percent_remaining), "%)",
                     is_low(item) ? " - low" : "");
    return line;
}

constexpr std::string render(summary const& found) {
    std::string out;
    text::append_all(out, "plan: ", found.plan.empty() ? std::string_view{"unknown"} : std::string_view{found.plan});
    if (!found.sku.empty()) {
        text::append_all(out, " (", found.sku, ")");
    }
    out += '\n';
    if (!found.reset_date.empty()) {
        text::append_all(out, "resets: ", found.reset_date, "\n");
    }
    for (auto const& item : found.quotas) {
        text::append_all(out, describe(item), "\n");
    }
    return out;
}

constexpr json::value to_json_value(summary const& found) {
    return json::to_value(found);
}

constexpr std::string to_json(summary const& found) {
    return json::write(to_json_value(found), 2);
}

constexpr std::string_view unavailable_message = "the Copilot quota endpoint did not answer or returned an unexpected reply";

}  // namespace llmgate::quota

template class std::optional<llmgate::quota::summary>;

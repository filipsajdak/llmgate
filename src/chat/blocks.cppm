export module llmgate.blocks;

import std;
import llmgate.json;

export namespace llmgate::blocks {

constexpr std::int64_t low_budget_limit = 4000;
constexpr std::int64_t medium_budget_limit = 16000;

constexpr json::value const* member(json::value const& source, std::string_view const name) {
    return json::at(source, {name});
}

constexpr std::string text_member(json::value const& source, std::string_view const name) {
    return json::text_at(source, {name});
}

constexpr json::value string_value(std::string text) {
    return json::make_string(std::move(text));
}

constexpr json::array blocks_of(json::value const& content) {
    if (auto const* text = std::get_if<std::string>(&content.data)) {
        return {json::make_object({{"type", string_value("text")}, {"text", string_value(*text)}})};
    }
    if (auto const* list = std::get_if<json::array>(&content.data)) {
        return *list;
    }
    return {};
}

constexpr bool flag_set(json::value const& source, std::string_view const name) {
    auto const* flag = member(source, name);
    return flag != nullptr && std::get_if<bool>(&flag->data) != nullptr && std::get<bool>(flag->data);
}

constexpr std::string joined_text(json::array const& blocks, std::string_view const separator) {
    std::string out;
    for (auto const& block : blocks) {
        if (text_member(block, "type") == "text") {
            out += out.empty() ? std::string_view{} : separator;
            out += text_member(block, "text");
        }
    }
    return out;
}

constexpr std::optional<std::string> image_url(json::value const& block) {
    auto const* source = member(block, "source");
    if (source == nullptr) {
        return std::nullopt;
    }
    auto const kind = text_member(*source, "type");
    if (kind == "url") {
        return text_member(*source, "url");
    }
    if (kind != "base64") {
        return std::nullopt;
    }
    std::string url{"data:"};
    url += text_member(*source, "media_type");
    url += ";base64,";
    url += text_member(*source, "data");
    return url;
}

constexpr std::string_view effort_named(std::string_view const wanted) {
    constexpr std::string_view low = "low";
    constexpr std::string_view medium = "medium";
    constexpr std::string_view high = "high";
    if (wanted == low) {
        return low;
    }
    return wanted == high || wanted == "xhigh" || wanted == "max" ? high : medium;
}

constexpr std::string_view effort_for_budget(std::int64_t const budget) {
    return budget < low_budget_limit ? "low" : budget < medium_budget_limit ? "medium" : "high";
}

constexpr std::optional<std::string_view> effort_from_budget(json::value const* const thinking) {
    auto const* budget = thinking == nullptr ? nullptr : member(*thinking, "budget_tokens");
    if (budget == nullptr || std::get_if<std::int64_t>(&budget->data) == nullptr) {
        return std::nullopt;
    }
    return effort_for_budget(std::get<std::int64_t>(budget->data));
}

constexpr std::optional<std::string_view> named_effort(json::value const& root) {
    auto const* config = member(root, "output_config");
    auto const named = config == nullptr ? std::string{} : text_member(*config, "effort");
    return named.empty() ? std::nullopt : std::optional{effort_named(named)};
}

constexpr std::optional<std::string_view> reasoning_effort(json::value const& root) {
    auto const* thinking = member(root, "thinking");
    auto const kind = thinking == nullptr ? std::string{} : text_member(*thinking, "type");
    if (kind == "disabled") {
        return std::nullopt;
    }
    if (auto const named = named_effort(root)) {
        return named;
    }
    if (auto const budgeted = effort_from_budget(thinking)) {
        return budgeted;
    }
    return kind.empty() ? std::nullopt : std::optional<std::string_view>{"medium"};
}

constexpr auto status_error_types = std::to_array<std::pair<int, std::string_view>>(
    {{400, "invalid_request_error"}, {401, "authentication_error"}, {403, "permission_error"}, {404, "not_found_error"}, {429, "rate_limit_error"}});

constexpr std::string_view error_type_for_status(int const status) {
    auto const found = std::ranges::find(status_error_types, status, &std::pair<int, std::string_view>::first);
    return found == status_error_types.end() ? "api_error" : found->second;
}

static_assert(effort_named("max") == "high" && effort_named("low") == "low" && effort_named("x") == "medium");
static_assert(effort_for_budget(100) == "low" && effort_for_budget(5000) == "medium" && effort_for_budget(20000) == "high");
static_assert(error_type_for_status(429) == "rate_limit_error" && error_type_for_status(500) == "api_error");

}  // namespace llmgate::blocks

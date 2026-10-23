export module llmgate.check;

import std;
import llmgate.control;
import llmgate.gateway;
import llmgate.json;
import llmgate.reflect;

export namespace llmgate::check {

enum class step { ping, tool, context, agent };

struct job {
    std::string provider{};
    std::string target{};
    std::string skipped{};
};

struct outcome {
    std::string provider{};
    std::string step{};
    bool skipped = false;
    std::string detail{};
    std::int64_t milliseconds = 0;
};

constexpr std::size_t long_repeats = 3000;
constexpr std::size_t shown_detail = 90;

}  // namespace llmgate::check

namespace llmgate::check::detail {

std::optional<std::string> text_member(json::value const& parent, std::string_view const key) {
    if (auto const* members = std::get_if<json::object>(&parent.data)) {
        for (auto const& [name, value] : *members) {
            if (name == key) {
                return std::get_if<std::string>(&value.data) == nullptr ? std::nullopt : std::optional<std::string>{*std::get_if<std::string>(&value.data)};
            }
        }
    }
    return std::nullopt;
}

std::optional<json::value> member_of(json::value const& parent, std::string_view const key) {
    if (auto const* members = std::get_if<json::object>(&parent.data)) {
        for (auto const& [name, value] : *members) {
            if (name == key) {
                return value;
            }
        }
    }
    return std::nullopt;
}

std::string message_of(std::string_view const body) {
    auto const parsed = json::parse(body, {.jsonc = false});
    auto const error = parsed ? member_of(*parsed, "error") : std::nullopt;
    auto const words = error ? text_member(*error, "message") : std::nullopt;
    return words ? *words : std::string{body};
}

std::string plain(std::string_view const text) {
    return text.substr(0, shown_detail) | std::views::transform([](char const c) { return c >= ' ' && c < '\x7f' ? c : ' '; }) | std::ranges::to<std::string>();
}

json::value user_message(std::string text) {
    json::array messages;
    messages.push_back(json::make_object({{"role", json::make_string("user")}, {"content", json::make_string(std::move(text))}}));
    return json::value{std::move(messages)};
}

std::string prompt_of(step const which) {
    if (which != step::context) {
        return which == step::tool ? "Call the echo tool with the text ok." : "Reply with the single word ok.";
    }
    std::string text = "Here is a long text to read.\n";
    for (std::size_t at = 0; at < long_repeats; ++at) {
        text += "lorem ipsum dolor sit amet ";
    }
    return text + "\nIgnore the text above and reply with only the number 3.";
}

json::value echo_tool() {
    json::array tools;
    tools.push_back(json::make_object(
        {{"name", json::make_string("echo")},
         {"description", json::make_string("Echo a text")},
         {"input_schema", json::make_object({{"type", json::make_string("object")},
                                             {"properties", json::make_object({{"text", json::make_object({{"type", json::make_string("string")}})}})},
                                             {"required", json::value{json::array{json::make_string("text")}}}})}}));
    return json::value{std::move(tools)};
}

std::string seconds_text(std::int64_t const milliseconds) {
    return milliseconds < 1000 ? std::format("{} ms", milliseconds) : std::format("{:.1f} s", static_cast<double>(milliseconds) / 1000.0);
}

}  // namespace llmgate::check::detail

export namespace llmgate::check {

std::optional<std::string> target_of(gateway::config const& settings, std::string_view const provider) {
    auto const all = gateway::effective_providers(settings);
    auto const found = all.find(std::string{provider});
    if (found == all.end()) {
        return std::nullopt;
    }
    for (auto const& [claude_name, upstream] : found->second.models) {
        if (claude_name.contains("haiku")) {
            return control::id_of(upstream, provider);
        }
    }
    if (!found->second.models.empty()) {
        return control::id_of(found->second.models.begin()->second, provider);
    }
    if (!found->second.offer.empty()) {
        return control::id_of(found->second.offer.front(), provider);
    }
    return std::nullopt;
}

std::vector<job> plan(gateway::config const& settings, std::string_view const only) {
    std::vector<job> jobs;
    for (auto const& [name, item] : gateway::effective_providers(settings)) {
        if (!only.empty() && name != only) {
            continue;
        }
        auto target = target_of(settings, name);
        job one{.provider = name};
        if (item.forward_credentials) {
            one.skipped = "forwards your Claude login, which a check run from the terminal does not have";
        } else if (!target) {
            one.skipped = "no model is listed for it (add models or offer)";
        } else {
            one.target = std::move(*target);
        }
        jobs.push_back(std::move(one));
    }
    return jobs;
}

std::vector<job> agent_jobs(gateway::config const& settings) {
    std::vector<job> jobs;
    for (auto const& [name, item] : settings.agents) {
        jobs.push_back({.provider = "agent " + name, .target = item.target});
    }
    for (auto const& [agent, target] : settings.spawn_rules) {
        jobs.push_back({.provider = "rule " + agent, .target = target});
    }
    return jobs;
}

std::string request_body(step const which, std::string_view const target) {
    json::object body;
    body.emplace_back("model", json::make_string(std::string{target}));
    body.emplace_back("max_tokens", json::value{static_cast<std::int64_t>(which == step::tool ? 64 : 16)});
    body.emplace_back("stream", json::value{true});
    body.emplace_back("messages", detail::user_message(detail::prompt_of(which)));
    if (which == step::tool) {
        body.emplace_back("tools", detail::echo_tool());
        body.emplace_back("tool_choice", json::make_object({{"type", json::make_string("tool")}, {"name", json::make_string("echo")}}));
    }
    return json::write(json::value{std::move(body)});
}

std::map<std::string, std::string> headers_of(step const which) {
    if (which != step::agent) {
        return {};
    }
    return {{"x-claude-code-request-class", "subagent"}, {"x-claude-code-agent-type", "custom"}};
}

std::string judge(step const which, int const status, std::string_view const body) {
    if (status != 200) {
        return detail::plain(std::format("HTTP {}: {}", status, detail::message_of(body)));
    }
    if (which == step::tool) {
        return body.contains(R"("tool_use")") ? std::string{} : std::string{"no tool call came back"};
    }
    return body.contains("text_delta") && body.contains("message_stop") ? std::string{} : std::string{"the reply was not a message"};
}

std::string step_name(step const which) { return std::string{reflect::enum_name(which)}; }

std::string render(std::vector<outcome> const& rows) {
    std::string out = std::format("{:<18} {:<8} {:<6} {:>8}  {}\n", "provider", "step", "result", "time", "detail");
    for (auto const& row : rows) {
        auto const result = row.skipped ? "skip" : row.detail.empty() ? "pass" : "FAIL";
        std::format_to(std::back_inserter(out), "{:<18} {:<8} {:<6} {:>8}  {}\n", row.provider, row.step, result, row.skipped ? std::string{"-"} : detail::seconds_text(row.milliseconds), row.detail);
    }
    return out;
}

struct json_row {
    std::string provider{};
    std::string step{};
    std::string result{};
    std::string detail{};
    std::int64_t milliseconds = 0;
};

std::string to_json(std::vector<outcome> const& rows) {
    std::vector<json_row> out;
    for (auto const& row : rows) {
        out.push_back({.provider = row.provider, .step = row.step, .result = row.skipped ? "skipped" : row.detail.empty() ? "pass" : "fail", .detail = row.detail, .milliseconds = row.milliseconds});
    }
    return json::write(json::to_value(out));
}

bool all_passed(std::vector<outcome> const& rows) {
    return std::ranges::all_of(rows, [](outcome const& row) { return row.skipped || row.detail.empty(); });
}

}  // namespace llmgate::check

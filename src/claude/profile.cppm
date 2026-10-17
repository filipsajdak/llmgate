export module llmgate.claude;

import std;
import llmgate.json;
import llmgate.text;

export namespace llmgate::claude {

using text::decimal;

struct launch_config {
    std::string opus = "claude-opus-5-5";
    std::string sonnet = "claude-sonnet-5-5";
    std::string haiku = "claude-haiku-5-5";
    std::string model = "opusplan";
    std::int64_t context_tokens = 180000;
    std::string profile_dir = "~/.claude-llmgate";
    std::string effort = "medium";
    bool telemetry = false;
};

using environment_list = std::vector<std::pair<std::string, std::string>>;

constexpr std::string_view dummy_api_key = "none";

constexpr environment_list environment(launch_config const& settings, std::string_view const gateway_url, std::string_view const client_key,
                                       std::string_view const expanded_profile_dir) {
    environment_list list{
        {"CLAUDE_CONFIG_DIR", std::string{expanded_profile_dir}},
        {"ANTHROPIC_BASE_URL", std::string{gateway_url}},
        {"ANTHROPIC_API_KEY", std::string{client_key.empty() ? dummy_api_key : client_key}},
        {"ANTHROPIC_MODEL", settings.model},
        {"ANTHROPIC_DEFAULT_OPUS_MODEL", settings.opus},
        {"ANTHROPIC_DEFAULT_SONNET_MODEL", settings.sonnet},
        {"ANTHROPIC_DEFAULT_HAIKU_MODEL", settings.haiku},
        {"CLAUDE_CODE_DISABLE_UNKNOWN_MODEL_WINDOW_ENFORCEMENT", "1"},
        {"CLAUDE_CODE_MAX_CONTEXT_TOKENS", decimal(settings.context_tokens)},
        {"CLAUDE_CODE_AUTO_COMPACT_WINDOW", decimal(settings.context_tokens)},
        {"CLAUDE_CODE_AUTO_MODE_SERVER", "0"},
        {"CLAUDE_CODE_ATTRIBUTION_HEADER", "0"},
    };
    if (!settings.telemetry) {
        list.emplace_back("CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC", "1");
        list.emplace_back("DISABLE_TELEMETRY", "1");
        list.emplace_back("DISABLE_ERROR_REPORTING", "1");
    }
    return list;
}

constexpr std::string shell_quote(std::string_view const text) {
    std::string out = "'";
    for (auto const c : text) {
        out += c == '\'' ? std::string{"'\\''"} : std::string(1, c);
    }
    out.push_back('\'');
    return out;
}

constexpr std::string shell_exports(environment_list const& list) {
    std::string out;
    for (auto const& [name, value] : list) {
        out += "export " + name + "=" + shell_quote(value) + "\n";
    }
    return out;
}

constexpr json::value environment_object(environment_list const& list) {
    json::object members;
    for (auto const& [name, value] : list) {
        members.emplace_back(name, json::make_string(value));
    }
    return json::value{std::move(members)};
}

constexpr json::value vscode_snippet(environment_list const& list) {
    json::array entries;
    for (auto const& [name, value] : list) {
        entries.push_back(json::make_object({{"name", json::make_string(name)}, {"value", json::make_string(value)}}));
    }
    return json::make_object({{"claudeCode.environmentVariables", json::value{std::move(entries)}},
                              {"claudeCode.disableLoginPrompt", json::value{true}}});
}

constexpr auto default_denials = std::to_array<std::string_view>({
    "Read(**/.env)",
    "Read(**/.env.*)",
    "Read(./secrets/**)",
    "Read(./config/credentials.json)",
    "Read(~/.config/llmgate/**)",
    "Read(~/.claude/**)",
});

}  // namespace llmgate::claude

namespace llmgate::claude::detail {

constexpr json::value* find_member(json::object& members, std::string_view const key) {
    auto const found = std::ranges::find(members, key, &json::object::value_type::first);
    return found == members.end() ? nullptr : &found->second;
}

constexpr json::object& object_member(json::object& parent, std::string_view const key) {
    if (auto* existing = find_member(parent, key)) {
        if (auto* members = std::get_if<json::object>(&existing->data)) {
            return *members;
        }
        *existing = json::value{json::object{}};
        return std::get<json::object>(existing->data);
    }
    parent.emplace_back(std::string{key}, json::value{json::object{}});
    return std::get<json::object>(parent.back().second.data);
}

constexpr json::array& array_member(json::object& parent, std::string_view const key) {
    if (auto* existing = find_member(parent, key)) {
        if (auto* items = std::get_if<json::array>(&existing->data)) {
            return *items;
        }
        *existing = json::value{json::array{}};
        return std::get<json::array>(existing->data);
    }
    parent.emplace_back(std::string{key}, json::value{json::array{}});
    return std::get<json::array>(parent.back().second.data);
}

constexpr bool contains_text(json::array const& items, std::string_view const text) {
    return std::ranges::any_of(items, [text](json::value const& item) {
        auto const* held = std::get_if<std::string>(&item.data);
        return held != nullptr && *held == text;
    });
}

}  // namespace llmgate::claude::detail

export namespace llmgate::claude {

constexpr json::value merge_settings(json::value existing, std::string_view const effort, std::string_view const status_line_command) {
    if (!std::holds_alternative<json::object>(existing.data)) {
        existing = json::value{json::object{}};
    }
    auto& root = std::get<json::object>(existing.data);
    auto& deny = detail::array_member(detail::object_member(root, "permissions"), "deny");
    for (auto const rule : default_denials) {
        if (!detail::contains_text(deny, rule)) {
            deny.push_back(json::make_string(std::string{rule}));
        }
    }
    if (detail::find_member(root, "effortLevel") == nullptr) {
        root.emplace_back("effortLevel", json::make_string(std::string{effort}));
    }
    if (detail::find_member(root, "statusLine") == nullptr) {
        root.emplace_back("statusLine", json::make_object({{"type", json::make_string("command")},
                                                           {"command", json::make_string(std::string{status_line_command})}}));
    }
    return existing;
}

constexpr json::value merge_vscode(json::value existing, environment_list const& list) {
    if (!std::holds_alternative<json::object>(existing.data)) {
        existing = json::value{json::object{}};
    }
    auto& root = std::get<json::object>(existing.data);
    auto& entries = detail::array_member(root, "claudeCode.environmentVariables");
    for (auto const& [name, value] : list) {
        auto const same_name = [&name](json::value const& entry) {
            auto const* members = std::get_if<json::object>(&entry.data);
            if (members == nullptr) {
                return false;
            }
            auto const found = std::ranges::find(*members, std::string_view{"name"}, &json::object::value_type::first);
            auto const* held = found == members->end() ? nullptr : std::get_if<std::string>(&found->second.data);
            return held != nullptr && *held == name;
        };
        auto const replacement = json::make_object({{"name", json::make_string(name)}, {"value", json::make_string(value)}});
        if (auto const present = std::ranges::find_if(entries, same_name); present != entries.end()) {
            *present = replacement;
        } else {
            entries.push_back(replacement);
        }
    }
    if (auto* flag = detail::find_member(root, "claudeCode.disableLoginPrompt")) {
        *flag = json::value{true};
    } else {
        root.emplace_back("claudeCode.disableLoginPrompt", json::value{true});
    }
    return existing;
}

constexpr std::string launchd_plist(std::string_view const label, std::string_view const binary, std::string_view const config_path,
                                    std::string_view const log_path) {
    std::string out = R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>)";
    out += label;
    out += "</string>\n  <key>ProgramArguments</key>\n  <array>\n    <string>";
    out += binary;
    out += "</string>\n    <string>--config</string>\n    <string>";
    out += config_path;
    out += "</string>\n    <string>serve</string>\n  </array>\n  <key>RunAtLoad</key><true/>\n  <key>KeepAlive</key><true/>\n  <key>StandardOutPath</key><string>";
    out += log_path;
    out += "</string>\n  <key>StandardErrorPath</key><string>";
    out += log_path;
    out += "</string>\n</dict>\n</plist>\n";
    return out;
}

constexpr std::string systemd_unit(std::string_view const binary, std::string_view const config_path) {
    std::string out = "[Unit]\nDescription=llmgate, a gateway for Claude Code\nAfter=network-online.target\n\n[Service]\nExecStart=";
    out += binary;
    out += " --config ";
    out += config_path;
    out += " serve\nRestart=on-failure\nRestartSec=2\n\n[Install]\nWantedBy=default.target\n";
    return out;
}

static_assert(json::write(merge_vscode(json::value{}, environment({}, "u", "", "/p"))).contains("claudeCode.disableLoginPrompt"));
static_assert(shell_quote("it's") == R"('it'\''s')");
static_assert(decimal(180000) == "180000" && decimal(-5) == "-5");
static_assert(environment(launch_config{}, "http://127.0.0.1:9480/anthropic", "", "/p").size() == 15);
static_assert(environment(launch_config{.telemetry = true}, "u", "k", "/p").size() == 12);
static_assert(json::write(merge_settings(json::value{}, "medium", "llmgate statusline")) == json::write(merge_settings(merge_settings(json::value{}, "medium", "llmgate statusline"), "medium", "llmgate statusline")));

}  // namespace llmgate::claude

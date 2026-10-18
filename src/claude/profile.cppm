module;

#include <meta>

export module llmgate.claude;

import std;
import llmgate.doc;
import llmgate.json;
import llmgate.reflect;
import llmgate.role;
import llmgate.text;

export namespace llmgate::claude {

using text::decimal;

struct launch_config {
    [[= reflect::doc{"Model name Claude Code uses for the opus role."}]] std::string opus = "claude-opus-5-5";
    [[= reflect::doc{"Model name Claude Code uses for the sonnet role."}]] std::string sonnet = "claude-sonnet-5-5";
    [[= reflect::doc{"Model name Claude Code uses for the haiku role."}]] std::string haiku = "claude-haiku-5-5";
    [[= reflect::doc{"Model Claude Code starts with: a model name or an alias such as opusplan."}]] std::string model = "opusplan";
    [[= reflect::doc{"Context window Claude Code assumes, in tokens; it also sets the auto-compact window."}]] std::int64_t context_tokens = 180000;
    [[= reflect::doc{"Directory of the separate Claude Code profile that llmgate claude uses."}]] std::string profile_dir = "~/.claude-llmgate";
    [[= reflect::doc{"Effort level written to the profile when it has none."}]] std::string effort = "medium";
    [[= reflect::doc{"Whether Claude Code keeps its telemetry on; off by default."}]] bool telemetry = false;
};

consteval bool one_model_slot_per_role() {
    auto const members = std::define_static_array(std::meta::nonstatic_data_members_of(^^launch_config, std::meta::access_context::unchecked()));
    return std::ranges::all_of(roles::named_families(), [members](roles::role const family) {
        return std::ranges::count_if(members, [family](std::meta::info const member) { return std::meta::identifier_of(member) == roles::name_of(family); }) == 1;
    });
}
static_assert(one_model_slot_per_role());

using environment_variable = text::name_value;

using environment_list = std::vector<environment_variable>;

constexpr std::string_view vscode_variables_key = "claudeCode.environmentVariables";
constexpr std::string_view vscode_login_prompt_key = "claudeCode.disableLoginPrompt";
constexpr std::string_view escaped_quote = R"('\'')";

constexpr std::string_view placeholder_token = "none";

constexpr environment_list environment(launch_config const& settings, std::string_view const gateway_url, std::string_view const client_key,
                                       std::string_view const expanded_profile_dir) {
    environment_list list{
        {"CLAUDE_CONFIG_DIR", std::string{expanded_profile_dir}},
        {"ANTHROPIC_BASE_URL", std::string{gateway_url}},
        {"ANTHROPIC_AUTH_TOKEN", std::string{client_key.empty() ? placeholder_token : client_key}},
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

constexpr void append_shell_quoted(std::string& out, std::string_view const text) {
    out += '\'';
    bool first = true;
    for (auto const part : text | std::views::split('\'')) {
        if (!std::exchange(first, false)) {
            out += escaped_quote;
        }
        out.append(std::string_view{part});
    }
    out += '\'';
}

constexpr std::string shell_quote(std::string_view const text) {
    std::string out;
    out.reserve(text.size() + 2);
    append_shell_quoted(out, text);
    return out;
}

constexpr std::string shell_exports(environment_list const& list) {
    std::string out;
    for (auto const& [name, value] : list) {
        text::append_all(out, "export ", name, "=");
        append_shell_quoted(out, value);
        out += '\n';
    }
    return out;
}

constexpr json::value environment_object(environment_list const& list) {
    return json::value{std::ranges::to<json::object>(list | std::views::transform([](environment_variable const& variable) {
                                                         return std::pair<std::string, json::value>{variable.name, json::make_string(variable.value)};
                                                     }))};
}

constexpr json::value vscode_snippet(environment_list const& list) {
    json::object root;
    root.emplace_back(vscode_variables_key, json::to_value(list));
    root.emplace_back(vscode_login_prompt_key, json::value{true});
    return json::value{std::move(root)};
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

constexpr std::string_view variable_name_key =
    std::meta::identifier_of(std::meta::nonstatic_data_members_of(std::meta::dealias(^^environment_variable), std::meta::access_context::current()).front());

constexpr bool has_name(json::value const& entry, std::string_view const name) {
    auto const* members = std::get_if<json::object>(&entry.data);
    if (members == nullptr) {
        return false;
    }
    auto const found = std::ranges::find(*members, variable_name_key, &json::object::value_type::first);
    auto const* held = found == members->end() ? nullptr : std::get_if<std::string>(&found->second.data);
    return held != nullptr && *held == name;
}

constexpr void upsert_variable(json::array& entries, environment_variable const& variable) {
    auto replacement = json::to_value(variable);
    if (auto const present = std::ranges::find_if(entries, [&variable](json::value const& entry) { return has_name(entry, variable.name); }); present != entries.end()) {
        *present = std::move(replacement);
    } else {
        entries.push_back(std::move(replacement));
    }
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
    auto& entries = detail::array_member(root, vscode_variables_key);
    for (auto const& variable : list) {
        detail::upsert_variable(entries, variable);
    }
    if (auto* flag = detail::find_member(root, vscode_login_prompt_key)) {
        *flag = json::value{true};
    } else {
        root.emplace_back(vscode_login_prompt_key, json::value{true});
    }
    return existing;
}

constexpr std::string replaced(std::string_view const text, std::span<std::pair<char, std::string_view> const> const table) {
    std::string out;
    out.reserve(text.size());
    for (auto const c : text) {
        auto const entry = std::ranges::find(table, c, &std::pair<char, std::string_view>::first);
        if (entry == table.end()) {
            out += c;
        } else {
            out.append(entry->second);
        }
    }
    return out;
}

constexpr auto xml_entities = std::to_array<std::pair<char, std::string_view>>({{'&', "&amp;"}, {'<', "&lt;"}, {'>', "&gt;"}});
constexpr auto unit_escapes = std::to_array<std::pair<char, std::string_view>>({{'"', "\\\""}, {'\\', "\\\\"}, {'%', "%%"}, {'$', "$$"}});

constexpr std::string xml_escaped(std::string_view const text) {
    return replaced(text, xml_entities);
}

inline std::string unit_quoted(std::string_view const text) {
    return std::format("\"{}\"", replaced(text, unit_escapes));
}

inline std::string launchd_plist(std::string_view const label, std::string_view const binary, std::string_view const config_path,
                                 std::string_view const log_path) {
    return std::format(R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>{0}</string>
  <key>ProgramArguments</key>
  <array>
    <string>{1}</string>
    <string>--config</string>
    <string>{2}</string>
    <string>serve</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>{3}</string>
  <key>StandardErrorPath</key><string>{3}</string>
</dict>
</plist>
)",
                       xml_escaped(label), xml_escaped(binary), xml_escaped(config_path), xml_escaped(log_path));
}

inline std::string systemd_unit(std::string_view const binary, std::string_view const config_path) {
    return std::format(R"([Unit]
Description=llmgate, a gateway for Claude Code
After=network-online.target

[Service]
ExecStart={} --config {} serve
Restart=on-failure
RestartSec=2

[Install]
WantedBy=default.target
)",
                       unit_quoted(binary), unit_quoted(config_path));
}

static_assert(shell_quote("it's") == R"('it'\''s')");
static_assert(decimal(180000) == "180000" && decimal(-5) == "-5");
static_assert(environment(launch_config{}, "http://127.0.0.1:9480/anthropic", "", "/p").size() == 15);
static_assert(environment(launch_config{.telemetry = true}, "u", "k", "/p").size() == 12);
static_assert(json::write(merge_settings(json::value{}, "medium", "llmgate statusline")) == json::write(merge_settings(merge_settings(json::value{}, "medium", "llmgate statusline"), "medium", "llmgate statusline")));

}  // namespace llmgate::claude

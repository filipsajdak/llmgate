export module llmgate.chat_request;

import std;
import llmgate.blocks;
export import llmgate.chat_quirks;
import llmgate.json;
import llmgate.reflect;
import llmgate.text;

export namespace llmgate::chat {

struct renamed_name {
    std::string used{};
    std::string original{};
};

struct translated {
    std::string body{};
    bool client_streams = false;
    std::vector<std::string> dropped{};
    std::vector<renamed_name> renamed{};
};

struct refusal {
    std::string message{};
};

constexpr std::size_t longest_name = 64;

}  // namespace llmgate::chat

namespace llmgate::chat::detail {

using blocks::blocks_of;
using blocks::flag_set;
using blocks::joined_text;
using blocks::member;
using blocks::string_value;
using blocks::text_member;

constexpr std::size_t hash_chars = 8;
constexpr std::uint32_t fnv_offset = 2166136261U;
constexpr std::uint32_t fnv_prime = 16777619U;
constexpr unsigned bits_per_digit = 4;

constexpr bool name_character(char const c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

constexpr std::string hash_of(std::string_view const text) {
    constexpr std::string_view digits = "0123456789abcdef";
    auto hash = fnv_offset;
    for (char const c : text) {
        hash = (hash ^ static_cast<unsigned char>(c)) * fnv_prime;
    }
    std::string out(hash_chars, '0');
    for (std::size_t index = 0; index < hash_chars; ++index) {
        out[hash_chars - 1 - index] = digits[(hash >> (index * bits_per_digit)) & 0xF];
    }
    return out;
}

constexpr std::string used_name(std::string_view const original) {
    if (!original.empty() && original.size() <= longest_name && std::ranges::all_of(original, name_character)) {
        return std::string{original};
    }
    std::string out;
    for (char const c : original.substr(0, longest_name - hash_chars - 1)) {
        out.push_back(name_character(c) ? c : '_');
    }
    text::append_all(out, "_", hash_of(original));
    return out;
}

struct name_book {
    std::vector<renamed_name> renamed{};

    constexpr std::string name(std::string_view const original) {
        auto used = used_name(original);
        if (used != original && std::ranges::none_of(renamed, [&](renamed_name const& each) { return each.used == used; })) {
            renamed.push_back({.used = used, .original = std::string{original}});
        }
        return used;
    }
};

constexpr refusal bad_block(std::size_t const message_at, std::size_t const block_at, std::string_view const what) {
    refusal out;
    text::append_all(out.message, "messages.", text::decimal(message_at), ".content.", text::decimal(block_at), ": ", what);
    return out;
}

constexpr json::value text_part(std::string text) {
    return json::make_object({{"type", string_value("text")}, {"text", string_value(std::move(text))}});
}

constexpr json::value image_part(std::string url) {
    return json::make_object({{"type", string_value("image_url")}, {"image_url", json::make_object({{"url", string_value(std::move(url))}})}});
}

constexpr json::value message_of(std::string_view const role, json::value content) {
    return json::make_object({{"role", string_value(std::string{role})}, {"content", std::move(content)}});
}

constexpr json::value user_message(json::array parts) {
    if (parts.size() == 1 && text_member(parts.front(), "type") == "text") {
        return message_of("user", string_value(text_member(parts.front(), "text")));
    }
    return message_of("user", json::value{std::move(parts)});
}

struct conversation {
    json::array messages{};
    json::array parts{};

    constexpr void flush() {
        if (!parts.empty()) {
            messages.push_back(user_message(std::move(parts)));
            parts = {};
        }
    }
};

constexpr json::array pictures_in(json::array const& blocks) {
    json::array pictures;
    for (auto const& inner : blocks) {
        auto url = text_member(inner, "type") == "image" ? blocks::image_url(inner) : std::nullopt;
        if (url) {
            pictures.push_back(image_part(std::move(*url)));
        }
    }
    return pictures;
}

constexpr void add_tool_result(conversation& turn, json::value const& block) {
    turn.flush();
    auto const* inner = member(block, "content");
    auto const content = blocks_of(inner == nullptr ? json::value{} : *inner);
    auto output = joined_text(content, "\n");
    if (flag_set(block, "is_error")) {
        output.insert(0, "Error: ");
    }
    turn.messages.push_back(json::make_object({{"role", string_value("tool")}, {"tool_call_id", string_value(text_member(block, "tool_use_id"))}, {"content", string_value(std::move(output))}}));
    if (auto pictures = pictures_in(content); !pictures.empty()) {
        turn.messages.push_back(user_message(std::move(pictures)));
    }
}

enum class block_kind { text, image, tool_use, tool_result, thinking, redacted_thinking };
static_assert(reflect::unique_values<block_kind>());

constexpr std::optional<refusal> add_image(conversation& turn, json::value const& block, std::size_t const message_at, std::size_t const block_at) {
    auto url = blocks::image_url(block);
    if (!url) {
        return bad_block(message_at, block_at, "image block needs a base64 or url source");
    }
    turn.parts.push_back(image_part(std::move(*url)));
    return std::nullopt;
}

constexpr refusal foreign_block(std::string_view const holder, json::value const& block, std::size_t const message_at, std::size_t const block_at) {
    std::string what;
    text::append_all(what, holder, " message cannot hold a '", text_member(block, "type"), "' block for this provider");
    return bad_block(message_at, block_at, what);
}

constexpr std::optional<refusal> add_user_block(conversation& turn, json::value const& block, std::size_t const message_at, std::size_t const block_at) {
    auto const named = reflect::enum_from_name<block_kind>(text_member(block, "type"));
    switch (named.value_or(block_kind::tool_use)) {
        case block_kind::text: turn.parts.push_back(text_part(text_member(block, "text"))); return std::nullopt;
        case block_kind::image: return add_image(turn, block, message_at, block_at);
        case block_kind::tool_result: add_tool_result(turn, block); return std::nullopt;
        case block_kind::thinking:
        case block_kind::redacted_thinking: return std::nullopt;
        case block_kind::tool_use: break;
    }
    return foreign_block("a user", block, message_at, block_at);
}

struct assistant_turn {
    std::string text{};
    json::array calls{};
};

constexpr std::optional<refusal> add_tool_call(assistant_turn& turn, name_book& names, json::value const& block, std::size_t const message_at, std::size_t const block_at) {
    auto const call_id = text_member(block, "id");
    if (call_id.empty()) {
        return bad_block(message_at, block_at, "tool_use needs an id");
    }
    auto const* input = member(block, "input");
    turn.calls.push_back(json::make_object({{"id", string_value(call_id)},
                                            {"type", string_value("function")},
                                            {"function", json::make_object({{"name", string_value(names.name(text_member(block, "name")))},
                                                                            {"arguments", string_value(json::write(input == nullptr ? json::make_object({}) : *input))}})}}));
    return std::nullopt;
}

constexpr std::optional<refusal> add_assistant_block(assistant_turn& turn, name_book& names, json::value const& block, std::size_t const message_at, std::size_t const block_at) {
    auto const named = reflect::enum_from_name<block_kind>(text_member(block, "type"));
    switch (named.value_or(block_kind::image)) {
        case block_kind::text:
            turn.text += turn.text.empty() ? std::string_view{} : std::string_view{"\n"};
            turn.text += text_member(block, "text");
            return std::nullopt;
        case block_kind::tool_use: return add_tool_call(turn, names, block, message_at, block_at);
        case block_kind::thinking:
        case block_kind::redacted_thinking: return std::nullopt;
        case block_kind::image:
        case block_kind::tool_result: break;
    }
    return foreign_block("an assistant", block, message_at, block_at);
}

constexpr std::optional<refusal> add_user_message(conversation& turn, json::array const& content, std::size_t const message_at) {
    for (std::size_t block_at = 0; block_at < content.size(); ++block_at) {
        if (auto problem = add_user_block(turn, content[block_at], message_at, block_at)) {
            return problem;
        }
    }
    turn.flush();
    return std::nullopt;
}

constexpr std::optional<refusal> add_assistant_message(conversation& turn, name_book& names, json::array const& content, std::size_t const message_at) {
    assistant_turn assistant;
    for (std::size_t block_at = 0; block_at < content.size(); ++block_at) {
        if (auto problem = add_assistant_block(assistant, names, content[block_at], message_at, block_at)) {
            return problem;
        }
    }
    if (assistant.text.empty() && assistant.calls.empty()) {
        return std::nullopt;
    }
    json::object out{{"role", string_value("assistant")}};
    if (!assistant.text.empty()) {
        out.emplace_back("content", string_value(std::move(assistant.text)));
    }
    if (!assistant.calls.empty()) {
        out.emplace_back("tool_calls", json::value{std::move(assistant.calls)});
    }
    turn.messages.push_back(json::value{std::move(out)});
    return std::nullopt;
}

constexpr std::optional<refusal> add_message(conversation& turn, name_book& names, json::value const& source, std::size_t const message_at) {
    auto const role = text_member(source, "role");
    auto const* content = member(source, "content");
    auto const blocks = blocks_of(content == nullptr ? json::value{} : *content);
    if (role == "assistant") {
        return add_assistant_message(turn, names, blocks, message_at);
    }
    if (role == "system") {
        turn.messages.push_back(message_of("system", string_value(joined_text(blocks, "\n\n"))));
        return std::nullopt;
    }
    return add_user_message(turn, blocks, message_at);
}

constexpr std::expected<json::array, refusal> chat_messages(json::value const& parsed, name_book& names) {
    auto const* list = std::get_if<json::array>(&member(parsed, "messages")->data);
    if (list == nullptr) {
        return std::unexpected{refusal{.message = "messages must be an array"}};
    }
    conversation turn;
    if (auto const* system = member(parsed, "system")) {
        if (auto text = joined_text(blocks_of(*system), "\n\n"); !text.empty()) {
            turn.messages.push_back(message_of("system", string_value(std::move(text))));
        }
    }
    for (std::size_t at = 0; at < list->size(); ++at) {
        if (auto problem = add_message(turn, names, (*list)[at], at)) {
            return std::unexpected{std::move(*problem)};
        }
    }
    return std::move(turn.messages);
}

constexpr json::value default_schema() {
    return json::make_object({{"type", string_value("object")}, {"properties", json::make_object({})}});
}

constexpr json::value parameters_of(json::value const& tool) {
    auto const* schema = member(tool, "input_schema");
    auto const* fields = schema == nullptr ? nullptr : std::get_if<json::object>(&schema->data);
    return fields != nullptr && !fields->empty() ? *schema : default_schema();
}

constexpr json::value function_entry(json::value const& tool, name_book& names) {
    json::object function{{"name", string_value(names.name(text_member(tool, "name")))}};
    if (auto const* description = member(tool, "description")) {
        function.emplace_back("description", *description);
    }
    function.emplace_back("parameters", parameters_of(tool));
    return json::make_object({{"type", string_value("function")}, {"function", json::value{std::move(function)}}});
}

constexpr refusal foreign_tool(std::size_t const at, std::string_view const kind) {
    refusal problem;
    text::append_all(problem.message, "tools.", text::decimal(at), ": Input tag '", kind, "' found using 'type' does not match any of the expected tags");
    return problem;
}

constexpr std::expected<json::array, refusal> tool_list(json::value const& tools, name_book& names) {
    auto const* list = std::get_if<json::array>(&tools.data);
    json::array out;
    for (std::size_t at = 0; list != nullptr && at < list->size(); ++at) {
        auto const kind = text_member((*list)[at], "type");
        if (!kind.empty() && kind != "custom") {
            return std::unexpected{foreign_tool(at, kind)};
        }
        out.push_back(function_entry((*list)[at], names));
    }
    return out;
}

constexpr std::optional<json::value> tool_choice_of(json::value const& choice, name_book& names) {
    auto const kind = text_member(choice, "type");
    if (kind == "auto" || kind == "none") {
        return string_value(kind);
    }
    if (kind == "any") {
        return string_value("required");
    }
    if (kind == "tool") {
        return json::make_object({{"type", string_value("function")}, {"function", json::make_object({{"name", string_value(names.name(text_member(choice, "name")))}})}});
    }
    return std::nullopt;
}

constexpr bool known_field(std::string_view const name) {
    constexpr auto handled = std::to_array<std::string_view>({"model", "system", "messages", "tools", "tool_choice", "thinking", "output_config", "stream", "max_tokens", "stop_sequences", "temperature", "top_p"});
    return std::ranges::contains(handled, name);
}

constexpr std::vector<std::string> dropped_names(json::value const& parsed) {
    std::vector<std::string> dropped;
    for (auto const& [name, ignored] : std::get<json::object>(parsed.data)) {
        if (!known_field(name)) {
            dropped.push_back(name);
        }
    }
    return dropped;
}

constexpr std::optional<refusal> missing_part(json::value const& parsed) {
    if (std::get_if<json::object>(&parsed.data) == nullptr) {
        return refusal{.message = "the request must be a JSON object"};
    }
    if (text_member(parsed, "model").empty()) {
        return refusal{.message = "model is required and must be a string"};
    }
    return member(parsed, "messages") == nullptr ? std::optional<refusal>{refusal{.message = "messages is required"}} : std::nullopt;
}

constexpr void add_limits(json::object& out, json::value const& parsed, quirks const& chosen) {
    if (auto const* limit = member(parsed, "max_tokens")) {
        out.emplace_back(chosen.use_max_completion_tokens ? "max_completion_tokens" : "max_tokens", *limit);
    }
    auto const* stops = member(parsed, "stop_sequences");
    auto const* list = stops == nullptr ? nullptr : std::get_if<json::array>(&stops->data);
    if (!chosen.drop_stop_sequences && list != nullptr && !list->empty()) {
        out.emplace_back("stop", *stops);
    }
}

constexpr bool carries_tools(json::value const& parsed) {
    auto const* tools = member(parsed, "tools");
    auto const* list = tools == nullptr ? nullptr : std::get_if<json::array>(&tools->data);
    return list != nullptr && !list->empty();
}

constexpr bool sends_effort(json::value const& parsed, quirks const& chosen) {
    return !chosen.drop_reasoning_effort && !(chosen.drop_reasoning_effort_with_tools && carries_tools(parsed));
}

constexpr void add_sampling(json::object& out, json::value const& parsed, quirks const& chosen) {
    for (auto const name : {"temperature", "top_p"}) {
        if (auto const* value = member(parsed, name); value != nullptr && !chosen.drop_sampling) {
            out.emplace_back(name, *value);
        }
    }
    if (auto const effort = blocks::reasoning_effort(parsed); effort && sends_effort(parsed, chosen)) {
        out.emplace_back("reasoning_effort", string_value(std::string{*effort}));
    }
}

constexpr std::optional<refusal> add_tools(json::object& out, json::value const& parsed, name_book& names) {
    auto const* tools = member(parsed, "tools");
    if (tools == nullptr) {
        return std::nullopt;
    }
    auto listed = tool_list(*tools, names);
    if (!listed) {
        return std::move(listed.error());
    }
    if (listed->empty()) {
        return std::nullopt;
    }
    out.emplace_back("tools", json::value{std::move(*listed)});
    if (auto const* choice = member(parsed, "tool_choice")) {
        if (auto mapped = tool_choice_of(*choice, names)) {
            out.emplace_back("tool_choice", std::move(*mapped));
        }
        if (flag_set(*choice, "disable_parallel_tool_use")) {
            out.emplace_back("parallel_tool_calls", json::value{false});
        }
    }
    return std::nullopt;
}

}  // namespace llmgate::chat::detail

export namespace llmgate::chat {

constexpr std::expected<translated, refusal> translate_request(std::string_view const anthropic_body, quirks const& chosen) {
    auto const parsed = json::parse(anthropic_body, {.jsonc = false});
    if (!parsed) {
        refusal problem;
        text::append_all(problem.message, "the request is not valid JSON: ", parsed.error().message);
        return std::unexpected{std::move(problem)};
    }
    if (auto problem = detail::missing_part(*parsed)) {
        return std::unexpected{std::move(*problem)};
    }
    detail::name_book names;
    auto messages = detail::chat_messages(*parsed, names);
    if (!messages) {
        return std::unexpected{std::move(messages.error())};
    }
    json::object out{{"model", detail::string_value(detail::text_member(*parsed, "model"))}, {"messages", json::value{std::move(*messages)}}};
    detail::add_limits(out, *parsed, chosen);
    detail::add_sampling(out, *parsed, chosen);
    if (auto problem = detail::add_tools(out, *parsed, names)) {
        return std::unexpected{std::move(*problem)};
    }
    out.emplace_back("stream", json::value{true});
    out.emplace_back("stream_options", json::make_object({{"include_usage", json::value{true}}}));
    return translated{.body = json::write(json::value{std::move(out)}),
                      .client_streams = detail::flag_set(*parsed, "stream"),
                      .dropped = detail::dropped_names(*parsed),
                      .renamed = std::move(names.renamed)};
}

constexpr std::string used_name_of(std::string_view const original) {
    return detail::used_name(original);
}

static_assert(translate_request(R"({"model":"m","stream":true,"messages":[{"role":"user","content":"hi"}]})", {}).has_value());
static_assert(!translate_request(R"({"model":"m","messages":[],"tools":[{"type":"advisor_20260301","name":"a"}]})", {}).has_value());
static_assert(used_name_of("Bash") == "Bash" && used_name_of("mcp__srv__tool-1") == "mcp__srv__tool-1");
static_assert(used_name_of("has space").starts_with("has_space_") && used_name_of("has space").size() == 18);
static_assert(used_name_of(std::string(100, 'a')).size() == longest_name && used_name_of(std::string(100, 'a')) != used_name_of(std::string(101, 'a')));

}  // namespace llmgate::chat

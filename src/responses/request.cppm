module;

export module llmgate.responses_request;

import std;
import llmgate.json;
import llmgate.reflect;
import llmgate.text;

export namespace llmgate::responses {

constexpr std::string_view signature_prefix = "llmgate-r1:";
constexpr std::int64_t low_budget_limit = 4000;
constexpr std::int64_t medium_budget_limit = 16000;

constexpr auto rejected_fields = std::to_array<std::string_view>(
    {"background", "conversation", "max_output_tokens", "max_tool_calls", "metadata", "moderation", "multi_agent", "prompt", "prompt_cache_retention",
     "safety_identifier", "temperature", "top_logprobs", "top_p", "truncation", "user", "previous_response_id"});

struct request_options {
    std::string prompt_cache_key{};
};

struct translated {
    std::string body{};
    bool client_streams = false;
    std::vector<std::string> dropped{};
};

struct refusal {
    std::string message{};
};

}  // namespace llmgate::responses

namespace llmgate::responses::detail {

using result = std::expected<json::value, refusal>;

constexpr json::value const* member(json::value const& source, std::string_view const name) {
    return json::at(source, {name});
}

constexpr std::string text_member(json::value const& source, std::string_view const name) {
    return json::text_at(source, {name});
}

constexpr json::value string_value(std::string text) {
    return json::make_string(std::move(text));
}

constexpr json::value part(std::string_view const kind, std::string_view const key, std::string text) {
    return json::make_object({{"type", string_value(std::string{kind})}, {std::string{key}, string_value(std::move(text))}});
}

constexpr json::value message(std::string_view const role, json::array parts) {
    return json::make_object({{"type", string_value("message")}, {"role", string_value(std::string{role})}, {"content", json::value{std::move(parts)}}});
}

constexpr json::array blocks_of(json::value const& content) {
    if (auto const* text = std::get_if<std::string>(&content.data)) {
        return {part("text", "text", *text)};
    }
    if (auto const* list = std::get_if<json::array>(&content.data)) {
        return *list;
    }
    return {};
}

constexpr refusal bad_block(std::size_t const message_at, std::size_t const block_at, std::string_view const what) {
    refusal out;
    text::append_all(out.message, "messages.", text::decimal(message_at), ".content.", text::decimal(block_at), ": ", what);
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
    if (kind == "base64") {
        std::string url;
        text::append_all(url, "data:", text_member(*source, "media_type"), ";base64,", text_member(*source, "data"));
        return url;
    }
    return std::nullopt;
}

constexpr json::value image_part(std::string url) {
    return part("input_image", "image_url", std::move(url));
}

struct conversation {
    json::array items{};
    json::array parts{};
    std::string role{};

    constexpr void flush() {
        if (!parts.empty()) {
            items.push_back(message(role, std::move(parts)));
            parts = {};
        }
    }
};

constexpr std::string joined_text(json::array const& blocks, std::string_view const separator) {
    std::string out;
    for (auto const& block : blocks) {
        if (text_member(block, "type") == "text") {
            text::append_all(out, out.empty() ? "" : separator, text_member(block, "text"));
        }
    }
    return out;
}

constexpr bool flag_set(json::value const& source, std::string_view const name) {
    auto const* flag = member(source, name);
    return flag != nullptr && std::get_if<bool>(&flag->data) != nullptr && std::get<bool>(flag->data);
}

constexpr json::array pictures_in(json::array const& blocks) {
    json::array pictures;
    for (auto const& inner : blocks) {
        auto url = text_member(inner, "type") == "image" ? image_url(inner) : std::nullopt;
        if (url) {
            pictures.push_back(image_part(std::move(*url)));
        }
    }
    return pictures;
}

constexpr json::value call_output(json::value const& block, std::string output) {
    return json::make_object({{"type", string_value("function_call_output")}, {"call_id", string_value(text_member(block, "tool_use_id"))}, {"output", string_value(std::move(output))}});
}

constexpr void add_tool_result(conversation& turn, json::value const& block) {
    turn.flush();
    auto const* inner = member(block, "content");
    auto const content = blocks_of(inner == nullptr ? json::value{} : *inner);
    auto output = joined_text(content, "\n");
    turn.items.push_back(call_output(block, flag_set(block, "is_error") ? "Error: " + output : std::move(output)));
    if (auto pictures = pictures_in(content); !pictures.empty()) {
        turn.items.push_back(message("user", std::move(pictures)));
    }
}

constexpr std::optional<refusal> add_tool_use(conversation& turn, json::value const& block, std::size_t const message_at, std::size_t const block_at) {
    auto const call_id = text_member(block, "id");
    if (call_id.empty()) {
        return bad_block(message_at, block_at, "tool_use needs an id");
    }
    turn.flush();
    auto const* input = member(block, "input");
    turn.items.push_back(json::make_object({{"type", string_value("function_call")}, {"call_id", string_value(call_id)}, {"name", string_value(text_member(block, "name"))},
                                            {"arguments", string_value(json::write(input == nullptr ? json::make_object({}) : *input))}}));
    return std::nullopt;
}

constexpr void add_thinking(conversation& turn, json::value const& block) {
    std::string_view const signature_view = [&] {
        auto const* found = member(block, "signature");
        auto const* text = found == nullptr ? nullptr : std::get_if<std::string>(&found->data);
        return text == nullptr ? std::string_view{} : std::string_view{*text};
    }();
    if (!signature_view.starts_with(signature_prefix)) {
        return;
    }
    auto const restored = json::parse(signature_view.substr(signature_prefix.size()), {.jsonc = false});
    if (restored && text_member(*restored, "type") == "reasoning") {
        turn.flush();
        turn.items.push_back(*restored);
    }
}

enum class block_kind { text, image, tool_use, tool_result, thinking, redacted_thinking };
static_assert(reflect::unique_values<block_kind>());

constexpr std::optional<refusal> add_text(conversation& turn, json::value const& block, std::string_view const role, std::size_t const message_at, std::size_t const block_at) {
    auto const* text = member(block, "text");
    if (text == nullptr || std::get_if<std::string>(&text->data) == nullptr) {
        return bad_block(message_at, block_at, "text block needs a text");
    }
    turn.parts.push_back(part(role == "assistant" ? "output_text" : "input_text", "text", std::get<std::string>(text->data)));
    return std::nullopt;
}

constexpr std::optional<refusal> add_image(conversation& turn, json::value const& block, std::size_t const message_at, std::size_t const block_at) {
    auto url = image_url(block);
    if (!url) {
        return bad_block(message_at, block_at, "image block needs a base64 or url source");
    }
    turn.parts.push_back(image_part(std::move(*url)));
    return std::nullopt;
}

constexpr std::optional<refusal> add_block(conversation& turn, json::value const& block, std::string_view const role, std::size_t const message_at, std::size_t const block_at) {
    auto const kind = text_member(block, "type");
    auto const named = reflect::enum_from_name<block_kind>(kind);
    if (!named) {
        std::string what;
        text::append_all(what, "unsupported content block type '", kind, "' for this provider");
        return bad_block(message_at, block_at, what);
    }
    switch (*named) {
        case block_kind::text: return add_text(turn, block, role, message_at, block_at);
        case block_kind::image: return add_image(turn, block, message_at, block_at);
        case block_kind::tool_use: return add_tool_use(turn, block, message_at, block_at);
        case block_kind::tool_result: add_tool_result(turn, block); return std::nullopt;
        case block_kind::thinking: add_thinking(turn, block); return std::nullopt;
        case block_kind::redacted_thinking: return std::nullopt;
    }
    std::unreachable();
}

constexpr std::optional<refusal> add_message(conversation& turn, json::value const& source, std::size_t const message_at) {
    auto const role = text_member(source, "role");
    turn.role = role == "system" ? "developer" : role;
    auto const* content = member(source, "content");
    auto const blocks = blocks_of(content == nullptr ? json::value{} : *content);
    for (std::size_t block_at = 0; block_at < blocks.size(); ++block_at) {
        if (auto problem = add_block(turn, blocks[block_at], role == "system" ? "user" : role, message_at, block_at)) {
            return problem;
        }
    }
    turn.flush();
    return std::nullopt;
}

constexpr std::expected<json::array, refusal> input_items(json::value const& messages) {
    auto const* list = std::get_if<json::array>(&messages.data);
    if (list == nullptr) {
        return std::unexpected{refusal{.message = "messages must be an array"}};
    }
    conversation turn;
    for (std::size_t at = 0; at < list->size(); ++at) {
        if (auto problem = add_message(turn, (*list)[at], at)) {
            return std::unexpected{std::move(*problem)};
        }
    }
    return std::move(turn.items);
}

constexpr std::string instructions_of(json::value const& system) {
    return joined_text(blocks_of(system), "\n\n");
}

constexpr json::value default_schema() {
    return json::make_object({{"type", string_value("object")}, {"properties", json::make_object({})}});
}

constexpr json::value parameters_of(json::value const& tool) {
    auto const* schema = member(tool, "input_schema");
    auto const* fields = schema == nullptr ? nullptr : std::get_if<json::object>(&schema->data);
    return fields != nullptr && !fields->empty() ? *schema : default_schema();
}

constexpr json::value function_entry(json::value const& tool) {
    json::object entry{{"type", string_value("function")}, {"name", string_value(text_member(tool, "name"))}};
    if (auto const* description = member(tool, "description")) {
        entry.emplace_back("description", *description);
    }
    entry.emplace_back("parameters", parameters_of(tool));
    return json::value{std::move(entry)};
}

constexpr refusal foreign_tool(std::size_t const at, std::string_view const kind) {
    refusal problem;
    text::append_all(problem.message, "tools.", text::decimal(at), ": Input tag '", kind, "' found using 'type' does not match any of the expected tags");
    return problem;
}

constexpr result tool_list(json::value const& tools) {
    auto const* list = std::get_if<json::array>(&tools.data);
    json::array out;
    for (std::size_t at = 0; list != nullptr && at < list->size(); ++at) {
        auto const kind = text_member((*list)[at], "type");
        if (!kind.empty() && kind != "custom") {
            return std::unexpected{foreign_tool(at, kind)};
        }
        out.push_back(function_entry((*list)[at]));
    }
    return json::value{std::move(out)};
}

constexpr std::optional<json::value> tool_choice_of(json::value const& choice) {
    auto const kind = text_member(choice, "type");
    if (kind == "auto") {
        return string_value("auto");
    }
    if (kind == "any") {
        return string_value("required");
    }
    if (kind == "none") {
        return string_value("none");
    }
    if (kind == "tool") {
        return json::make_object({{"type", string_value("function")}, {"name", string_value(text_member(choice, "name"))}});
    }
    return std::nullopt;
}

constexpr bool disables_parallel_calls(json::value const& choice);

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

constexpr std::optional<std::string_view> effort_from_budget(json::value const& thinking) {
    auto const* budget = member(thinking, "budget_tokens");
    if (budget == nullptr || std::get_if<std::int64_t>(&budget->data) == nullptr) {
        return std::nullopt;
    }
    return effort_for_budget(std::get<std::int64_t>(budget->data));
}

constexpr std::optional<std::string_view> reasoning_effort(json::value const& root) {
    auto const* thinking = member(root, "thinking");
    auto const kind = thinking == nullptr ? std::string{} : text_member(*thinking, "type");
    if (kind == "disabled") {
        return std::nullopt;
    }
    auto const* config = member(root, "output_config");
    if (auto const named = config == nullptr ? std::string{} : text_member(*config, "effort"); !named.empty()) {
        return effort_named(named);
    }
    if (auto const budgeted = thinking == nullptr ? std::nullopt : effort_from_budget(*thinking)) {
        return budgeted;
    }
    return kind.empty() ? std::nullopt : std::optional<std::string_view>{"medium"};
}

constexpr bool known_field(std::string_view const name) {
    constexpr auto handled = std::to_array<std::string_view>({"model", "system", "messages", "tools", "tool_choice", "thinking", "output_config", "stream"});
    return std::ranges::contains(handled, name);
}

constexpr bool streams(json::value const& root) {
    return flag_set(root, "stream");
}

constexpr bool disables_parallel_calls(json::value const& choice) {
    return flag_set(choice, "disable_parallel_tool_use");
}

constexpr json::value reasoning_object(std::string_view const effort) {
    return json::make_object({{"effort", string_value(std::string{effort})}, {"summary", string_value("auto")}});
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

constexpr void add_instructions(json::object& out, json::value const& parsed) {
    auto const* system = member(parsed, "system");
    if (auto text = system == nullptr ? std::string{} : instructions_of(*system); !text.empty()) {
        out.emplace_back("instructions", string_value(std::move(text)));
    }
}

constexpr std::optional<refusal> add_tools(json::object& out, json::value const& parsed, bool& parallel_allowed) {
    auto const* tools = member(parsed, "tools");
    if (tools == nullptr) {
        return std::nullopt;
    }
    auto listed = tool_list(*tools);
    if (!listed) {
        return std::move(listed.error());
    }
    auto const* entries = std::get_if<json::array>(&listed->data);
    if (entries == nullptr || entries->empty()) {
        return std::nullopt;
    }
    out.emplace_back("tools", std::move(*listed));
    if (auto const* choice = member(parsed, "tool_choice")) {
        if (auto mapped = tool_choice_of(*choice)) {
            out.emplace_back("tool_choice", std::move(*mapped));
        }
        parallel_allowed = !disables_parallel_calls(*choice);
    }
    return std::nullopt;
}

constexpr void add_fixed_settings(json::object& out, json::value const& parsed, bool const parallel_allowed, std::string_view const cache_key) {
    if (!parallel_allowed) {
        out.emplace_back("parallel_tool_calls", json::value{false});
    }
    if (auto const effort = reasoning_effort(parsed)) {
        out.emplace_back("reasoning", reasoning_object(*effort));
    }
    out.emplace_back("include", json::value{json::array{string_value("reasoning.encrypted_content")}});
    out.emplace_back("store", json::value{false});
    out.emplace_back("stream", json::value{true});
    if (!cache_key.empty()) {
        out.emplace_back("prompt_cache_key", string_value(std::string{cache_key}));
    }
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

}  // namespace llmgate::responses::detail

export namespace llmgate::responses {

constexpr std::expected<translated, refusal> translate_request(std::string_view const anthropic_body, request_options const& options) {
    auto const parsed = json::parse(anthropic_body, {.jsonc = false});
    if (!parsed) {
        refusal problem;
        text::append_all(problem.message, "the request is not valid JSON: ", parsed.error().message);
        return std::unexpected{std::move(problem)};
    }
    if (auto problem = detail::missing_part(*parsed)) {
        return std::unexpected{std::move(*problem)};
    }
    auto items = detail::input_items(*detail::member(*parsed, "messages"));
    if (!items) {
        return std::unexpected{std::move(items.error())};
    }
    json::object out{{"model", detail::string_value(detail::text_member(*parsed, "model"))}};
    detail::add_instructions(out, *parsed);
    out.emplace_back("input", json::value{std::move(*items)});
    bool parallel_allowed = true;
    if (auto problem = detail::add_tools(out, *parsed, parallel_allowed)) {
        return std::unexpected{std::move(*problem)};
    }
    detail::add_fixed_settings(out, *parsed, parallel_allowed, options.prompt_cache_key);
    return translated{.body = json::write(json::value{std::move(out)}), .client_streams = detail::streams(*parsed), .dropped = detail::dropped_names(*parsed)};
}

static_assert(translate_request(R"({"model":"m","stream":true,"messages":[{"role":"user","content":"hi"}]})", {}).has_value());
static_assert(!translate_request(R"({"model":"m","messages":[],"tools":[{"type":"advisor_20260301","name":"a"}]})", {}).has_value());

}  // namespace llmgate::responses

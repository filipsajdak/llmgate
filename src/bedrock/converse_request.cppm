export module llmgate.converse_request;

import std;
import llmgate.bedrock_options;
import llmgate.blocks;
import llmgate.chat_request;
import llmgate.json;
import llmgate.text;

export namespace llmgate::converse {

using chat::refusal;
using chat::renamed_name;

struct translated {
    std::string body{};
    bool client_streams = false;
    std::vector<std::string> dropped{};
    std::vector<renamed_name> renamed{};
};

}  // namespace llmgate::converse

namespace llmgate::converse::detail {

using blocks::blocks_of;
using blocks::flag_set;
using blocks::member;
using blocks::string_value;
using blocks::text_member;

constexpr std::string_view empty_result_text = "(empty)";

struct context {
    bedrock::converse_options const* chosen = nullptr;
    bool thinking = false;
    std::vector<renamed_name> renamed{};
    std::size_t documents = 0;
};

using converted = std::expected<json::array, refusal>;
using converter = converted (*)(context&, json::value const&);

constexpr refusal refused(std::string_view const what) {
    return refusal{.message = std::string{what}};
}

constexpr json::value cache_point() {
    return json::make_object({{"cachePoint", json::make_object({{"type", string_value("default")}})}});
}

constexpr void add_cache_point(context const& ctx, json::value const& source, json::array& out) {
    if (member(source, "cache_control") != nullptr && !ctx.chosen->drop_cache_points) {
        out.push_back(cache_point());
    }
}

constexpr std::string format_of(std::string_view const media_type) {
    constexpr std::array<std::pair<std::string_view, std::string_view>, 6> aliases{
        {{"jpg", "jpeg"}, {"mpeg", "mp3"}, {"x-wav", "wav"}, {"x-m4a", "m4a"}, {"quicktime", "mov"}, {"x-matroska", "mkv"}}};
    auto const slash = media_type.find('/');
    auto const subtype = slash == std::string_view::npos ? media_type : media_type.substr(slash + 1);
    auto const found = std::ranges::find(aliases, subtype, &std::pair<std::string_view, std::string_view>::first);
    return std::string{found == aliases.end() ? subtype : found->second};
}

constexpr std::string document_format_of(std::string_view const media_type) {
    constexpr std::array<std::pair<std::string_view, std::string_view>, 5> known{
        {{"application/pdf", "pdf"}, {"text/plain", "txt"}, {"text/markdown", "md"}, {"text/csv", "csv"}, {"text/html", "html"}}};
    auto const found = std::ranges::find(known, media_type, &std::pair<std::string_view, std::string_view>::first);
    return found == known.end() ? std::string{} : std::string{found->second};
}

constexpr json::value bytes_source(std::string_view const data) {
    return json::make_object({{"bytes", string_value(std::string{data})}});
}

constexpr std::string base64_of(json::value const& block, std::string& media_type) {
    auto const* source = member(block, "source");
    if (source == nullptr || text_member(*source, "type") != "base64") {
        return {};
    }
    media_type = text_member(*source, "media_type");
    return text_member(*source, "data");
}

constexpr converted blank() {
    return json::array{};
}

constexpr converted text_block(context& ctx, json::value const& block) {
    auto const text = text_member(block, "text");
    json::array out;
    if (text.find_first_not_of(" \t\r\n") != std::string::npos) {
        out.push_back(json::make_object({{"text", string_value(text)}}));
        add_cache_point(ctx, block, out);
    }
    return out;
}

template <std::size_t Key>
constexpr converted media_block(context& ctx, json::value const& block) {
    constexpr std::array<std::string_view, 3> keys{"image", "audio", "video"};
    std::string media_type;
    auto const data = base64_of(block, media_type);
    if (data.empty()) {
        return std::unexpected{refused(std::string{keys[Key]} + " blocks must carry base64 data")};
    }
    json::array out{json::make_object(
        {{std::string{keys[Key]}, json::make_object({{"format", string_value(format_of(media_type))}, {"source", bytes_source(data)}})}})};
    add_cache_point(ctx, block, out);
    return out;
}

constexpr converted document_block(context& ctx, json::value const& block) {
    std::string media_type;
    auto const data = base64_of(block, media_type);
    auto const format = document_format_of(media_type);
    if (data.empty() || format.empty()) {
        return std::unexpected{refused("document blocks must carry base64 pdf, txt, md, csv or html data")};
    }
    std::string name{"document-"};
    text::append_all(name, text::decimal(++ctx.documents));
    json::array out{json::make_object(
        {{"document", json::make_object({{"format", string_value(format)}, {"name", string_value(std::move(name))}, {"source", bytes_source(data)}})}})};
    add_cache_point(ctx, block, out);
    return out;
}

constexpr std::string used_name(context& ctx, std::string_view const original) {
    auto used = chat::used_name_of(original);
    if (used != original && std::ranges::none_of(ctx.renamed, [&](renamed_name const& each) { return each.used == used; })) {
        ctx.renamed.push_back({.used = used, .original = std::string{original}});
    }
    return used;
}

constexpr converted tool_use_block(context& ctx, json::value const& block) {
    auto const* input = member(block, "input");
    json::array out{json::make_object({{"toolUse", json::make_object({{"toolUseId", string_value(text_member(block, "id"))},
                                                                      {"name", string_value(used_name(ctx, text_member(block, "name")))},
                                                                      {"input", input == nullptr ? json::make_object({}) : *input}})}})};
    return out;
}

constexpr converted result_content(context& ctx, json::value const& content) {
    json::array out;
    for (auto const& inner : blocks_of(content)) {
        auto const kind = text_member(inner, "type");
        auto piece = kind == "image" ? media_block<0>(ctx, inner) : text_block(ctx, inner);
        if (!piece) {
            return piece;
        }
        std::ranges::move(*piece, std::back_inserter(out));
    }
    if (out.empty()) {
        out.push_back(json::make_object({{"text", string_value(std::string{empty_result_text})}}));
    }
    return out;
}

constexpr converted tool_result_block(context& ctx, json::value const& block) {
    auto const* content = member(block, "content");
    auto inner = result_content(ctx, content == nullptr ? json::value{} : *content);
    if (!inner) {
        return inner;
    }
    json::array out{json::make_object({{"toolResult", json::make_object({{"toolUseId", string_value(text_member(block, "tool_use_id"))},
                                                                         {"content", json::value{std::move(*inner)}},
                                                                         {"status", string_value(flag_set(block, "is_error") ? "error" : "success")}})}})};
    add_cache_point(ctx, block, out);
    return out;
}

constexpr converted thinking_block(context& ctx, json::value const& block) {
    if (!ctx.thinking) {
        return blank();
    }
    return json::array{json::make_object({{"reasoningContent", json::make_object({{"reasoningText", json::make_object({{"text", string_value(text_member(block, "thinking"))},
                                                                                                                           {"signature", string_value(text_member(block, "signature"))}})}})}})};
}

constexpr converted redacted_block(context& ctx, json::value const& block) {
    if (!ctx.thinking) {
        return blank();
    }
    return json::array{json::make_object({{"reasoningContent", json::make_object({{"redactedContent", string_value(text_member(block, "data"))}})}})};
}

constexpr std::array<std::pair<std::string_view, converter>, 9> converters{{{"text", text_block},
                                                                            {"image", media_block<0>},
                                                                            {"audio", media_block<1>},
                                                                            {"video", media_block<2>},
                                                                            {"document", document_block},
                                                                            {"tool_use", tool_use_block},
                                                                            {"tool_result", tool_result_block},
                                                                            {"thinking", thinking_block},
                                                                            {"redacted_thinking", redacted_block}}};

constexpr refusal foreign(std::size_t const message_at, std::size_t const block_at, std::string_view const kind) {
    refusal out;
    text::append_all(out.message, "messages.", text::decimal(message_at), ".content.", text::decimal(block_at), ": a block of type '", kind, "' cannot be sent to the Converse operation");
    return out;
}

constexpr converted convert_blocks(context& ctx, json::array const& source, std::size_t const message_at) {
    json::array out;
    for (std::size_t at = 0; at < source.size(); ++at) {
        auto const kind = text_member(source[at], "type");
        auto const found = std::ranges::find(converters, kind, &std::pair<std::string_view, converter>::first);
        if (found == converters.end()) {
            return std::unexpected{foreign(message_at, at, kind)};
        }
        auto piece = found->second(ctx, source[at]);
        if (!piece) {
            return piece;
        }
        std::ranges::move(*piece, std::back_inserter(out));
    }
    return out;
}

struct turn {
    std::string role{};
    json::array content{};
};

constexpr std::string_view converse_role(std::string_view const role) {
    return role == "assistant" ? "assistant" : "user";
}

constexpr json::value turn_value(turn& each) {
    return json::make_object({{"role", string_value(each.role)}, {"content", json::value{std::move(each.content)}}});
}

struct conversation {
    std::vector<turn> turns{};
    json::array leading_system{};

    constexpr void add(std::string_view const role, bool const leading, json::array content) {
        if (content.empty()) {
            return;
        }
        if (leading) {
            std::ranges::move(content, std::back_inserter(leading_system));
        } else if (!turns.empty() && turns.back().role == converse_role(role)) {
            std::ranges::move(content, std::back_inserter(turns.back().content));
        } else {
            turns.push_back({.role = std::string{converse_role(role)}, .content = std::move(content)});
        }
    }
};

constexpr std::optional<refusal> add_message(context& ctx, conversation& book, json::value const& source, std::size_t const at) {
    auto const* content = member(source, "content");
    auto converted_content = convert_blocks(ctx, blocks_of(content == nullptr ? json::value{} : *content), at);
    if (!converted_content) {
        return std::move(converted_content.error());
    }
    auto const role = text_member(source, "role");
    book.add(role, role == "system" && book.turns.empty(), std::move(*converted_content));
    return std::nullopt;
}

constexpr std::expected<conversation, refusal> conversation_of(context& ctx, json::value const& parsed) {
    auto const* list = std::get_if<json::array>(&member(parsed, "messages")->data);
    if (list == nullptr) {
        return std::unexpected{refused("messages must be an array")};
    }
    conversation book;
    for (std::size_t at = 0; at < list->size(); ++at) {
        if (auto problem = add_message(ctx, book, (*list)[at], at)) {
            return std::unexpected{std::move(*problem)};
        }
    }
    return book;
}

constexpr json::array system_blocks(context& ctx, json::value const& parsed, json::array leading) {
    json::array out;
    if (auto const* system = member(parsed, "system")) {
        for (auto const& block : blocks_of(*system)) {
            auto piece = text_block(ctx, block);
            std::ranges::move(*piece, std::back_inserter(out));
        }
    }
    std::ranges::move(leading, std::back_inserter(out));
    return out;
}

constexpr json::value schema_of(json::value const& tool) {
    auto const* schema = member(tool, "input_schema");
    auto const* fields = schema == nullptr ? nullptr : std::get_if<json::object>(&schema->data);
    return fields != nullptr && !fields->empty() ? *schema : json::make_object({{"type", string_value("object")}, {"properties", json::make_object({})}});
}

constexpr json::value tool_spec(context& ctx, json::value const& tool) {
    json::object spec{{"name", string_value(used_name(ctx, text_member(tool, "name")))}};
    if (auto const* description = member(tool, "description")) {
        spec.emplace_back("description", *description);
    }
    spec.emplace_back("inputSchema", json::make_object({{"json", schema_of(tool)}}));
    return json::make_object({{"toolSpec", json::value{std::move(spec)}}});
}

constexpr refusal foreign_tool(std::size_t const at, std::string_view const kind) {
    refusal out;
    text::append_all(out.message, "tools.", text::decimal(at), ": Input tag '", kind, "' found using 'type' does not match any of the expected tags");
    return out;
}

constexpr std::expected<json::array, refusal> tool_list(context& ctx, json::array const& tools) {
    json::array out;
    for (std::size_t at = 0; at < tools.size(); ++at) {
        auto const kind = text_member(tools[at], "type");
        if (!kind.empty() && kind != "custom") {
            return std::unexpected{foreign_tool(at, kind)};
        }
        out.push_back(tool_spec(ctx, tools[at]));
        add_cache_point(ctx, tools[at], out);
    }
    return out;
}

constexpr std::optional<json::value> tool_choice_of(context& ctx, json::value const& choice) {
    auto const kind = text_member(choice, "type");
    if (kind == "auto") {
        return json::make_object({{"auto", json::make_object({})}});
    }
    if (kind == "any") {
        return json::make_object({{"any", json::make_object({})}});
    }
    if (kind == "tool") {
        return json::make_object({{"tool", json::make_object({{"name", string_value(used_name(ctx, text_member(choice, "name")))}})}});
    }
    return std::nullopt;
}

constexpr bool tool_choice_is_none(json::value const& parsed) {
    auto const* choice = member(parsed, "tool_choice");
    return choice != nullptr && text_member(*choice, "type") == "none";
}

constexpr std::optional<refusal> add_tool_config(context& ctx, json::object& out, json::value const& parsed) {
    auto const* tools = member(parsed, "tools");
    auto const* list = tools == nullptr ? nullptr : std::get_if<json::array>(&tools->data);
    if (list == nullptr || list->empty() || tool_choice_is_none(parsed)) {
        return std::nullopt;
    }
    auto listed = tool_list(ctx, *list);
    if (!listed) {
        return std::move(listed.error());
    }
    json::object config{{"tools", json::value{std::move(*listed)}}};
    if (auto const* choice = member(parsed, "tool_choice")) {
        if (auto mapped = tool_choice_of(ctx, *choice)) {
            config.emplace_back("toolChoice", std::move(*mapped));
        }
    }
    out.emplace_back("toolConfig", json::value{std::move(config)});
    return std::nullopt;
}

constexpr void add_inference(json::object& out, json::value const& parsed) {
    json::object config;
    constexpr std::array<std::pair<std::string_view, std::string_view>, 4> names{
        {{"max_tokens", "maxTokens"}, {"temperature", "temperature"}, {"top_p", "topP"}, {"stop_sequences", "stopSequences"}}};
    for (auto const& [from, to] : names) {
        if (auto const* value = member(parsed, from)) {
            config.emplace_back(std::string{to}, *value);
        }
    }
    if (!config.empty()) {
        out.emplace_back("inferenceConfig", json::value{std::move(config)});
    }
}

constexpr bool thinking_requested(json::value const& parsed) {
    auto const* thinking = member(parsed, "thinking");
    auto const kind = thinking == nullptr ? std::string{} : text_member(*thinking, "type");
    return !kind.empty() && kind != "disabled";
}

constexpr void add_thinking(context const& ctx, json::object& out, json::value const& parsed) {
    if (ctx.thinking && thinking_requested(parsed)) {
        out.emplace_back("additionalModelRequestFields", json::make_object({{"thinking", *member(parsed, "thinking")}}));
    }
}

constexpr bool carried(std::string_view const name, bool const thinking) {
    constexpr auto always = std::to_array<std::string_view>({"model", "system", "messages", "tools", "tool_choice", "stream", "max_tokens", "stop_sequences", "temperature", "top_p"});
    return std::ranges::contains(always, name) || (thinking && name == "thinking");
}

constexpr std::vector<std::string> dropped_names(json::value const& parsed, bool const thinking) {
    std::vector<std::string> dropped;
    for (auto const& [name, ignored] : std::get<json::object>(parsed.data)) {
        if (!carried(name, thinking)) {
            dropped.push_back(name);
        }
    }
    return dropped;
}

constexpr std::optional<refusal> missing_part(json::value const& parsed) {
    if (std::get_if<json::object>(&parsed.data) == nullptr) {
        return refused("the request must be a JSON object");
    }
    return member(parsed, "messages") == nullptr ? std::optional<refusal>{refused("messages is required")} : std::nullopt;
}

}  // namespace llmgate::converse::detail

export namespace llmgate::converse {

constexpr std::expected<translated, refusal> translate_request(std::string_view const anthropic_body, bedrock::converse_options const& chosen) {
    auto const parsed = json::parse(anthropic_body, {.jsonc = false});
    if (!parsed) {
        refusal problem;
        text::append_all(problem.message, "the request is not valid JSON: ", parsed.error().message);
        return std::unexpected{std::move(problem)};
    }
    if (auto problem = detail::missing_part(*parsed)) {
        return std::unexpected{std::move(*problem)};
    }
    detail::context ctx{.chosen = &chosen, .thinking = std::ranges::contains(chosen.thinking_models, detail::text_member(*parsed, "model"))};
    auto book = detail::conversation_of(ctx, *parsed);
    if (!book) {
        return std::unexpected{std::move(book.error())};
    }
    json::object out;
    json::array turns;
    for (auto& each : book->turns) {
        turns.push_back(detail::turn_value(each));
    }
    out.emplace_back("messages", json::value{std::move(turns)});
    if (auto system = detail::system_blocks(ctx, *parsed, std::move(book->leading_system)); !system.empty()) {
        out.emplace_back("system", json::value{std::move(system)});
    }
    detail::add_inference(out, *parsed);
    if (auto problem = detail::add_tool_config(ctx, out, *parsed)) {
        return std::unexpected{std::move(*problem)};
    }
    detail::add_thinking(ctx, out, *parsed);
    return translated{.body = json::write(json::value{std::move(out)}),
                      .client_streams = detail::flag_set(*parsed, "stream"),
                      .dropped = detail::dropped_names(*parsed, ctx.thinking),
                      .renamed = std::move(ctx.renamed)};
}

static_assert(translate_request(R"({"model":"m","stream":true,"messages":[{"role":"user","content":"hi"}]})", {}).has_value());
static_assert(!translate_request(R"({"model":"m","messages":[],"tools":[{"type":"advisor_20260301","name":"a"}]})", {}).has_value());
static_assert(!translate_request("[]", {}).has_value());

}  // namespace llmgate::converse

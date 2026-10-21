module;

export module llmgate.history;

import std;
import llmgate.json;

export namespace llmgate::history {

struct stripped {
    std::string body{};
    std::size_t removed = 0;
};

}  // namespace llmgate::history

namespace llmgate::history::detail {

constexpr json::value* slot(json::value& source, std::string_view const name) {
    auto* members = std::get_if<json::object>(&source.data);
    if (members == nullptr) {
        return nullptr;
    }
    for (auto& [key, value] : *members) {
        if (key == name) {
            return &value;
        }
    }
    return nullptr;
}

constexpr json::array* blocks_of(json::value& message) {
    auto* content = slot(message, "content");
    return content == nullptr ? nullptr : std::get_if<json::array>(&content->data);
}

constexpr bool is_thinking(json::value const& block) {
    auto const kind = json::text_at(block, {"type"});
    return kind == "thinking" || kind == "redacted_thinking";
}

constexpr bool is_assistant(json::value const& message) {
    return json::text_at(message, {"role"}) == "assistant";
}

constexpr std::size_t drop_thinking(json::value& message) {
    auto* blocks = is_assistant(message) ? blocks_of(message) : nullptr;
    return blocks == nullptr ? 0 : std::erase_if(*blocks, is_thinking);
}

constexpr bool emptied(json::value& message) {
    auto* blocks = is_assistant(message) ? blocks_of(message) : nullptr;
    return blocks != nullptr && blocks->empty();
}

}  // namespace llmgate::history::detail

export namespace llmgate::history {

constexpr std::optional<stripped> without_thinking(std::string_view const body) {
    auto parsed = json::parse(body, {.jsonc = false});
    auto* slot = parsed ? detail::slot(*parsed, "messages") : nullptr;
    auto* messages = slot == nullptr ? nullptr : std::get_if<json::array>(&slot->data);
    if (messages == nullptr) {
        return std::nullopt;
    }
    std::size_t removed = 0;
    for (auto& message : *messages) {
        removed += detail::drop_thinking(message);
    }
    if (removed == 0) {
        return std::nullopt;
    }
    std::erase_if(*messages, detail::emptied);
    return stripped{.body = json::write(*parsed), .removed = removed};
}

static_assert(without_thinking(R"({"messages":[{"role":"assistant","content":[{"type":"thinking"},{"type":"text","text":"t"}]}]})")->removed == 1);
static_assert(!without_thinking(R"({"messages":[]})").has_value());

}  // namespace llmgate::history

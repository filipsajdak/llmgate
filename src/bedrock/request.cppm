export module llmgate.bedrock_request;

import std;
export import llmgate.bedrock_options;
import llmgate.blocks;
import llmgate.json;
import llmgate.text;

export namespace llmgate::bedrock {

constexpr std::string_view anthropic_version = "bedrock-2023-05-31";

constexpr auto documented_betas = std::to_array<std::string_view>(
    {"computer-use-2024-10-22", "computer-use-2025-01-24", "computer-use-2025-11-24", "token-efficient-tools-2025-02-19", "interleaved-thinking-2025-05-14", "output-128k-2025-02-19",
     "dev-full-thinking-2025-05-14", "context-1m-2025-08-07", "context-management-2025-06-27", "effort-2025-11-24", "tool-search-tool-2025-10-19", "tool-examples-2025-10-29",
     "fine-grained-tool-streaming-2025-05-14"});

struct translated {
    std::string body{};
    bool client_streams = false;
    std::vector<std::string> dropped_betas{};
};

struct refusal {
    std::string message{};
};

}  // namespace llmgate::bedrock

namespace llmgate::bedrock::detail {

constexpr std::vector<std::string> tokens_of(std::string_view const header) {
    std::vector<std::string> out;
    for (auto const part : std::views::split(header, ',')) {
        std::string_view token{part.begin(), part.end()};
        while (!token.empty() && token.front() == ' ') {
            token.remove_prefix(1);
        }
        while (!token.empty() && token.back() == ' ') {
            token.remove_suffix(1);
        }
        if (!token.empty()) {
            out.emplace_back(token);
        }
    }
    return out;
}

constexpr bool allowed(std::string_view const token, options const& chosen) {
    if (chosen.betas.empty()) {
        return std::ranges::contains(documented_betas, token);
    }
    return std::ranges::contains(chosen.betas, token);
}

constexpr bool carried_in_the_url(std::string_view const name) {
    return name == "model" || name == "stream" || name == "anthropic_version" || name == "anthropic_beta";
}

constexpr json::array text_blocks_of(json::value const& content) {
    json::array out;
    for (auto const& block : blocks::blocks_of(content)) {
        if (blocks::text_member(block, "type") == "text") {
            out.push_back(block);
        }
    }
    return out;
}

constexpr void append_blocks(json::value& message, json::array extra) {
    auto* members = std::get_if<json::object>(&message.data);
    for (std::size_t at = 0; members != nullptr && at < members->size(); ++at) {
        if ((*members)[at].first == "content") {
            auto merged = blocks::blocks_of((*members)[at].second);
            std::ranges::move(extra, std::back_inserter(merged));
            (*members)[at].second = json::value{std::move(merged)};
            return;
        }
    }
}

constexpr std::string_view role_of(json::value const& message) {
    auto const* role = blocks::member(message, "role");
    auto const* text = role == nullptr ? nullptr : std::get_if<std::string>(&role->data);
    return text == nullptr ? std::string_view{} : std::string_view{*text};
}

struct folded {
    json::array messages{};
    json::array system_extra{};
};

constexpr void fold_one(folded& out, json::value const& message) {
    if (role_of(message) != "system") {
        out.messages.push_back(message);
        return;
    }
    auto const* content = blocks::member(message, "content");
    auto extra = text_blocks_of(content == nullptr ? json::value{} : *content);
    if (out.messages.empty()) {
        std::ranges::move(extra, std::back_inserter(out.system_extra));
    } else if (role_of(out.messages.back()) == "user") {
        append_blocks(out.messages.back(), std::move(extra));
    } else {
        out.messages.push_back(json::make_object({{"role", blocks::string_value("user")}, {"content", json::value{std::move(extra)}}}));
    }
}

constexpr folded fold_system_messages(json::value const& messages) {
    folded out;
    auto const* list = std::get_if<json::array>(&messages.data);
    for (std::size_t at = 0; list != nullptr && at < list->size(); ++at) {
        fold_one(out, (*list)[at]);
    }
    return out;
}

constexpr json::value system_with(json::value const* original, json::array extra) {
    auto merged = original == nullptr ? json::array{} : blocks::blocks_of(*original);
    std::ranges::move(extra, std::back_inserter(merged));
    return json::value{std::move(merged)};
}

constexpr json::array split_betas(std::string_view const header, options const& chosen, std::vector<std::string>& dropped) {
    json::array kept;
    for (auto& token : tokens_of(header)) {
        if (allowed(token, chosen)) {
            kept.push_back(blocks::string_value(std::move(token)));
        } else {
            dropped.push_back(std::move(token));
        }
    }
    return kept;
}

constexpr void copy_members(json::object const& members, folded messages, json::object& out) {
    bool system_written = messages.system_extra.empty();
    for (auto const& [name, value] : members) {
        if (name == "messages") {
            out.emplace_back(name, json::value{std::move(messages.messages)});
        } else if (name == "system" && !system_written) {
            out.emplace_back(name, system_with(&value, std::move(messages.system_extra)));
            system_written = true;
        } else if (!carried_in_the_url(name)) {
            out.emplace_back(name, value);
        }
    }
    if (!system_written) {
        out.emplace_back("system", system_with(nullptr, std::move(messages.system_extra)));
    }
}

}  // namespace llmgate::bedrock::detail

export namespace llmgate::bedrock {

constexpr std::expected<translated, refusal> translate_request(std::string_view const anthropic_body, std::string_view const beta_header, options const& chosen) {
    auto const parsed = json::parse(anthropic_body, {.jsonc = false});
    if (!parsed) {
        refusal problem;
        text::append_all(problem.message, "the request is not valid JSON: ", parsed.error().message);
        return std::unexpected{std::move(problem)};
    }
    auto const* members = std::get_if<json::object>(&parsed->data);
    if (members == nullptr) {
        return std::unexpected{refusal{.message = "the request must be a JSON object"}};
    }
    json::object out{{"anthropic_version", blocks::string_value(std::string{anthropic_version})}};
    auto messages = detail::fold_system_messages(json::value{});
    if (auto const* original = blocks::member(*parsed, "messages")) {
        messages = detail::fold_system_messages(*original);
    }
    detail::copy_members(*members, std::move(messages), out);
    translated done{.client_streams = blocks::flag_set(*parsed, "stream")};
    if (auto kept = detail::split_betas(beta_header, chosen, done.dropped_betas); !kept.empty()) {
        out.emplace_back("anthropic_beta", json::value{std::move(kept)});
    }
    done.body = json::write(json::value{std::move(out)});
    return done;
}

static_assert(translate_request(R"({"model":"m","stream":true,"max_tokens":5,"messages":[]})", "", {}).has_value());
static_assert(!translate_request("[]", "", {}).has_value() && !translate_request("{", "", {}).has_value());

}  // namespace llmgate::bedrock

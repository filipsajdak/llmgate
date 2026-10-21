module llmgate.model_list;

import std;
import llmgate.json;
import llmgate.lenient;
import llmgate.reflect;

namespace llmgate::model_list::detail {

struct limits_wire {
    std::uint64_t max_context_window_tokens = 0;
    std::uint64_t max_prompt_tokens = 0;
    std::uint64_t max_output_tokens = 0;
};

struct capabilities_wire {
    std::string family{};
    std::string tokenizer{};
    limits_wire limits{};
};

struct policy_wire {
    std::string state{};
};

struct listed_wire {
    std::string id{};
    capabilities_wire capabilities{};
    policy_wire policy{};
    std::uint64_t max_input_tokens = 0;
    std::uint64_t max_tokens = 0;
};

struct plan_wire {
    std::string slug{};
    std::uint64_t context_window = 0;
    std::string visibility{};
};

constexpr std::string_view hidden_visibility = "hide";

std::vector<std::string> endpoints_of(json::value const& item) {
    std::vector<std::string> names;
    if (auto const* listed = json::at(item, {"supported_endpoints"})) {
        if (auto const* items = std::get_if<json::array>(&listed->data)) {
            for (auto const& name : *items) {
                if (auto const* text = std::get_if<std::string>(&name.data)) {
                    names.push_back(*text);
                }
            }
        }
    }
    return names;
}

std::optional<entry> from_listed(json::value const& item) {
    auto const wire = lenient::read<listed_wire>(item).value_or(listed_wire{});
    if (wire.id.empty()) {
        return std::nullopt;
    }
    auto const& limits = wire.capabilities.limits;
    return entry{.id = wire.id,
                 .family = wire.capabilities.family,
                 .tokenizer = wire.capabilities.tokenizer,
                 .context = limits.max_context_window_tokens,
                 .prompt = limits.max_prompt_tokens != 0 ? limits.max_prompt_tokens : wire.max_input_tokens,
                 .output = limits.max_output_tokens != 0 ? limits.max_output_tokens : wire.max_tokens,
                 .policy = reflect::enum_from_name<access>(wire.policy.state).value_or(access::unknown),
                 .endpoints = endpoints_of(item)};
}

std::optional<entry> from_plan(json::value const& item) {
    auto const wire = lenient::read<plan_wire>(item).value_or(plan_wire{});
    if (wire.slug.empty()) {
        return std::nullopt;
    }
    return entry{.id = wire.slug, .context = wire.context_window, .hidden = wire.visibility == hidden_visibility};
}

std::vector<entry> collect(json::array const& items, std::optional<entry> (*const read)(json::value const&)) {
    std::vector<entry> found;
    for (auto const& item : items) {
        if (auto const model = read(item)) {
            found.push_back(*model);
        }
    }
    return found;
}

}  // namespace llmgate::model_list::detail

namespace llmgate::model_list {

std::expected<std::vector<entry>, std::string> parse(std::string_view const body) {
    auto const document = json::parse(body, {.jsonc = false});
    if (!document) {
        return std::unexpected{std::string{"the reply is not JSON"}};
    }
    if (auto const* listed = json::at(*document, {"data"})) {
        if (auto const* items = std::get_if<json::array>(&listed->data)) {
            return detail::collect(*items, &detail::from_listed);
        }
    }
    if (auto const* plan = json::at(*document, {"models"})) {
        if (auto const* items = std::get_if<json::array>(&plan->data)) {
            return detail::collect(*items, &detail::from_plan);
        }
    }
    return std::unexpected{std::string{"the reply holds neither a data nor a models list"}};
}

std::string printable(std::string_view const text, std::size_t const limit) {
    std::string clean;
    for (auto const c : text.substr(0, limit)) {
        clean.push_back((static_cast<unsigned char>(c) < ' ' || c == '\x7f') ? '?' : c);
    }
    return clean;
}

std::string render(std::span<entry const> const models) {
    std::string table;
    for (auto const& model : models) {
        std::format_to(std::back_inserter(table), "{}  context {}  prompt {}  output {}", printable(model.id, longest_text), model.context, model.prompt, model.output);
        if (model.hidden) {
            table.append("  hidden");
        }
        if (model.policy == access::disabled) {
            table.append("  disabled by the account's policy");
        }
        table.push_back('\n');
    }
    return table;
}

}  // namespace llmgate::model_list

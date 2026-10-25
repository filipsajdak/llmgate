export module llmgate.offer;

import std;
import llmgate.config_edit;
import llmgate.gateway;
import llmgate.json;
import llmgate.model_list;
import llmgate.reflect;

export namespace llmgate::offer {

enum class wire { messages, responses, unsupported };

constexpr std::string_view messages_endpoint = "/v1/messages";
constexpr std::string_view responses_endpoint = "/responses";

constexpr bool lists(model_list::entry const& model, std::string_view const endpoint) {
    return std::ranges::contains(model.endpoints, endpoint);
}

constexpr wire wire_of(model_list::entry const& model, gateway::wire_protocol const provider) {
    if (model.endpoints.empty()) {
        return provider == gateway::wire_protocol::openai_responses ? wire::responses : wire::messages;
    }
    if (lists(model, messages_endpoint)) {
        return wire::messages;
    }
    return lists(model, responses_endpoint) ? wire::responses : wire::unsupported;
}

constexpr bool recommended(model_list::entry const& model, gateway::wire_protocol const provider) {
    return model_list::usable(model) && !model.hidden && wire_of(model, provider) != wire::unsupported;
}

}  // namespace llmgate::offer

namespace llmgate::offer::detail {

constexpr gateway::wire_protocol protocol_of(wire const found) {
    return found == wire::responses ? gateway::wire_protocol::openai_responses : gateway::wire_protocol::anthropic;
}

std::string limits_json(model_list::entry const& model) {
    json::object out;
    if (model.context != 0) {
        out.emplace_back("context_tokens", json::value{static_cast<std::int64_t>(model.context)});
    }
    if (model.prompt != 0) {
        out.emplace_back("prompt_tokens", json::value{static_cast<std::int64_t>(model.prompt)});
    }
    if (model.output != 0) {
        out.emplace_back("output_tokens", json::value{static_cast<std::int64_t>(model.output)});
    }
    return json::write(json::value{std::move(out)});
}

std::optional<std::string> refusal(model_list::entry const& model, wire const found) {
    if (!model_list::usable(model)) {
        return std::format("{} is disabled by the account's policy: enable it for the account, then offer it", model.id);
    }
    if (found == wire::unsupported) {
        return std::format("{} answers only on {}, which llmgate does not translate yet", model.id, model.endpoints.empty() ? std::string{"an unknown endpoint"} : model.endpoints.front());
    }
    return std::nullopt;
}

void add_model_edits(std::vector<config_edit::change>& out, std::string const& name, gateway::provider_config const& provider, model_list::entry const& model, wire const found) {
    if (auto const limits = limits_json(model); limits != "{}") {
        out.push_back({.path = {"providers", name, "limits", model.id}, .value = limits});
    }
    if (protocol_of(found) != provider.protocol) {
        out.push_back({.path = {"providers", name, "model_protocols", model.id}, .value = json::write(json::make_string(std::string{reflect::enum_name(protocol_of(found))}))});
    }
}

}  // namespace llmgate::offer::detail

export namespace llmgate::offer {

std::expected<std::vector<config_edit::change>, std::string> plan(std::string const& name, gateway::provider_config const& provider, std::vector<model_list::entry> const& listed,
                                                                 std::vector<std::string> const& ids) {
    json::array offered;
    std::vector<config_edit::change> out;
    for (auto const& id : ids) {
        auto const model = std::ranges::find(listed, id, &model_list::entry::id);
        if (model == listed.end()) {
            return std::unexpected{std::format("provider {} does not list {}", name, id)};
        }
        auto const found = wire_of(*model, provider.protocol);
        if (auto const refused = detail::refusal(*model, found)) {
            return std::unexpected{*refused};
        }
        offered.push_back(json::make_string(id));
        detail::add_model_edits(out, name, provider, *model, found);
    }
    out.insert(out.begin(), {.path = {"providers", name, "offer"}, .value = json::write(json::value{std::move(offered)})});
    return out;
}

}  // namespace llmgate::offer

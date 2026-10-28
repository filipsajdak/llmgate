export module llmgate.network;

import std;
import llmgate.config;
import llmgate.proxy;

export namespace llmgate::network {

constexpr auto proxy_variables = std::to_array<std::string_view>({"HTTPS_PROXY", "https_proxy", "ALL_PROXY", "all_proxy"});
constexpr auto bypass_variables = std::to_array<std::string_view>({"NO_PROXY", "no_proxy"});

template <class Lookup>
concept environment_lookup = std::invocable<Lookup const&, std::string_view> && std::convertible_to<std::invoke_result_t<Lookup const&, std::string_view>, std::optional<std::string>>;

template <environment_lookup Lookup>
std::string first_set(Lookup const& lookup, std::span<std::string_view const> const names) {
    for (auto const name : names) {
        if (std::optional<std::string> const value = lookup(name); value && !value->empty()) {
            return *value;
        }
    }
    return {};
}

inline std::string joined(std::span<std::string const> const entries) {
    return std::ranges::to<std::string>(entries | std::views::join_with(','));
}

template <environment_lookup Lookup>
std::expected<proxy::policy, std::string> policy_of(gateway::network_config const& chosen, Lookup const& lookup) {
    proxy::policy out;
    auto const text = chosen.proxy.empty() ? first_set(lookup, proxy_variables) : chosen.proxy;
    if (!text.empty()) {
        auto parsed = proxy::parse(text);
        if (!parsed) {
            return std::unexpected{std::move(parsed.error())};
        }
        out.via = std::move(*parsed);
    }
    out.bypass = proxy::split_list(chosen.no_proxy.empty() ? first_set(lookup, bypass_variables) : joined(chosen.no_proxy));
    return out;
}

}  // namespace llmgate::network

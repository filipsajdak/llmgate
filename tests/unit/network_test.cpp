#include <boost/ut.hpp>

import llmgate.config;
import llmgate.network;
import llmgate.proxy;
import std;

namespace {

using namespace boost::ut;
namespace nw = llmgate::network;
namespace gw = llmgate::gateway;

auto environment_of(std::map<std::string, std::string> values) {
    return [values = std::move(values)](std::string_view const name) -> std::optional<std::string> {
        auto const found = values.find(std::string{name});
        return found == values.end() ? std::nullopt : std::optional{found->second};
    };
}

suite<"network"> tests = [] {
    "capability_the_proxy_comes_from_the_first_environment_variable_that_is_set"_test = [] {
        auto const policy = nw::policy_of({}, environment_of({{"ALL_PROXY", "http://third:1"}, {"https_proxy", "http://second:2"}}));

        expect(policy.has_value() && policy->via && policy->via->host == "second" && policy->via->port == 2_u);
    };

    "capability_the_configuration_wins_over_the_environment_for_the_proxy_and_for_the_bypass_list"_test = [] {
        gw::network_config const chosen{.proxy = "http://configured:3128", .no_proxy = {"Direct.Test", " 10.0.0.0/8 "}};
        auto const policy = nw::policy_of(chosen, environment_of({{"HTTPS_PROXY", "http://env:1"}, {"NO_PROXY", "other.test"}}));

        expect(policy.has_value() && policy->via->host == "configured");
        expect(policy->bypass == std::vector<std::string>{"direct.test", "10.0.0.0/8"});
    };

    "capability_the_bypass_list_comes_from_no_proxy_when_the_configuration_has_none"_test = [] {
        auto const policy = nw::policy_of({}, environment_of({{"NO_PROXY", "a.test, b.test"}}));

        expect(policy.has_value() && !policy->via && policy->bypass.size() == 2_u);
    };

    "limit_a_proxy_url_that_cannot_be_used_is_an_error_naming_the_reason_not_a_silent_direct_connection"_test = [] {
        auto const policy = nw::policy_of({}, environment_of({{"HTTPS_PROXY", "socks5://proxy:1080"}}));

        expect(!policy.has_value() && policy.error().contains("http://"));
    };

    "capability_nothing_set_means_no_proxy_and_an_empty_bypass_list"_test = [] {
        auto const policy = nw::policy_of({}, environment_of({}));

        expect(policy.has_value() && !policy->via && policy->bypass.empty());
    };
};

}  // namespace

int main() {}

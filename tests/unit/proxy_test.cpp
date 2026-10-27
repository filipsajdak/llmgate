#include <boost/ut.hpp>

import llmgate.proxy;
import std;

namespace {

using namespace boost::ut;
namespace px = llmgate::proxy;

suite<"proxy"> tests = [] {
    "capability_a_proxy_url_gives_host_port_and_basic_credentials_and_masks_the_password"_test = [] {
        auto const found = px::parse("http://alice:p%3Ass@proxy.corp:3128/");

        expect(found.has_value() && found->host == "proxy.corp" && found->port == 3128_u);
        expect(found->authorization == "Basic YWxpY2U6cDpzcw==") << found->authorization;
        expect(px::mask("http://alice:p%3Ass@proxy.corp:3128/") == "http://alice:***@proxy.corp:3128/");
    };

    "limit_a_proxy_url_that_is_not_http_or_has_a_bad_port_or_escape_is_refused_with_a_reason"_test = [] {
        expect(px::parse("https://proxy.corp").error().contains("http://"));
        expect(px::parse("http://proxy.corp:70000").error().contains("port"));
        expect(px::parse("http://a:%G1@proxy.corp").error().contains("%-escape"));
        expect(px::parse("http://").error().contains("host"));
    };

    "capability_no_proxy_matches_names_suffixes_literals_cidrs_ports_and_the_star"_test = [] {
        auto const entries = px::split_list("Internal.Corp, 10.0.0.0/8 ,fd00::/8,svc.test:8443,*.skipped");

        expect(px::bypasses(entries, "build.internal.corp", 443) && px::bypasses(entries, "10.1.2.3", 80) && px::bypasses(entries, "fd00::5", 80));
        expect(px::bypasses(entries, "svc.test", 8443) && !px::bypasses(entries, "svc.test", 443) && !px::bypasses(entries, "external.example", 443));
        expect(px::bypasses(px::split_list("*"), "anything", 1));
    };

    "capability_loopback_and_bypassed_hosts_go_direct_and_others_through_the_proxy"_test = [] {
        px::policy const chosen{.via = px::proxy_url{.host = "p", .port = 3128}, .bypass = px::split_list("skip.test")};

        expect(px::route_for(chosen, "api.example", 443).has_value() && !px::route_for(chosen, "skip.test", 443).has_value());
        expect(!px::route_for(chosen, "127.0.0.1", 9) && !px::route_for(chosen, "localhost", 9) && !px::route_for(chosen, "::1", 9));
        expect(!px::route_for({}, "api.example", 443).has_value());
    };

    "capability_the_connect_request_names_the_target_and_carries_the_credentials_only_when_given"_test = [] {
        auto const plain = px::connect_request("api.example", 443, "");
        auto const secret = px::connect_request("api.example", 443, "Basic eDp5");

        expect(plain.starts_with("CONNECT api.example:443 HTTP/1.1\r\nHost: api.example:443\r\n") && !plain.contains("Proxy-Authorization"));
        expect(secret.contains("Proxy-Authorization: Basic eDp5\r\n") && secret.ends_with("\r\n\r\n"));
    };

    "capability_the_connect_answer_is_read_in_parts_and_classified"_test = [] {
        std::string const whole = "HTTP/1.1 200 Connection established\r\nVia: 1.1 p\r\n\r\n";

        expect(px::parse_reply(whole.substr(0, 20)).which == px::outcome::incomplete);
        expect(px::parse_reply(whole).which == px::outcome::tunnel && px::parse_reply(whole).consumed == whole.size());
        expect(px::parse_reply("HTTP/1.1 407 Proxy Authentication Required\r\n\r\n").which == px::outcome::authentication_required);
        auto const refused = px::parse_reply("HTTP/1.1 403 Forbidden\x01 by policy\r\n\r\n");
        expect(refused.which == px::outcome::refused && refused.status == 403 && refused.reason == "Forbidden by policy") << refused.reason;
    };

    "limit_an_answer_that_is_not_http_or_never_ends_is_refused_instead_of_waited_for"_test = [] {
        expect(px::parse_reply("garbage\r\n\r\n").which == px::outcome::refused);
        expect(px::parse_reply(std::string(px::longest_reply + 1, 'x')).which == px::outcome::refused);
        expect(px::parse_reply(std::string(px::longest_reply, 'x')).which == px::outcome::incomplete);
    };
};

}  // namespace

int main() {}

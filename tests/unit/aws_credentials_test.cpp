#include <boost/ut.hpp>

import llmgate.async;
import llmgate.aws_credentials;
import llmgate.gateway;
import std;

namespace {

using namespace boost::ut;
namespace aws = llmgate::aws;
namespace gw = llmgate::gateway;

constexpr std::string_view credentials_text = "# comment\n[default]\naws_access_key_id = AKIDDEFAULT\naws_secret_access_key = secretdefault\n\n[work]\naws_access_key_id=AKIDWORK\naws_secret_access_key=secretwork\naws_session_token = tokenwork\n";
constexpr std::string_view config_text = "[default]\nregion = eu-west-1\n\n[profile sso]\ncredential_process = aws configure export-credentials --profile \"my sso\" --format process\n";

struct world {
    std::map<std::string, std::string> env{};
    std::map<std::string, std::string> files{};
    std::vector<std::vector<std::string>> ran{};
    std::expected<std::string, std::string> output = std::unexpected{std::string{"not run"}};

    aws::sources sources() {
        return {.read = {.env = [this](std::string const& name) -> std::optional<std::string> {
                             auto const found = env.find(name);
                             return found == env.end() ? std::nullopt : std::optional<std::string>{found->second};
                         },
                         .file = [this](std::string const& path) -> std::optional<std::string> {
                             auto const found = files.find(path);
                             return found == files.end() ? std::nullopt : std::optional<std::string>{found->second};
                         }},
                .run = [this](std::vector<std::string> const& command) {
                    ran.push_back(command);
                    return output;
                }};
    }
};

world standard() {
    return {.files = {{"~/.aws/credentials", std::string{credentials_text}}, {"~/.aws/config", std::string{config_text}}}};
}

suite<"aws_credentials"> tests = [] {
    "capability_ini_sections_keys_and_comments_are_read"_test = [] {
        auto const table = aws::parse_ini(credentials_text);

        expect(table.size() == 2_u && table.at("work").at("aws_session_token") == "tokenwork" && table.at("default").at("aws_access_key_id") == "AKIDDEFAULT");
    };

    "capability_a_command_is_split_on_blanks_and_quotes_group_words"_test = [] {
        expect(aws::split_command("aws configure --profile \"my sso\" 'a b' x") == std::vector<std::string>{"aws", "configure", "--profile", "my sso", "a b", "x"});
        expect(aws::split_command("   ").empty());
    };

    "capability_an_iso_expiry_becomes_epoch_seconds_and_junk_does_not"_test = [] {
        expect(aws::parse_expiry("2015-08-30T12:36:00Z") == std::optional<std::int64_t>{1440938160});
        expect(!aws::parse_expiry("tomorrow").has_value() && !aws::parse_expiry("2015-13-30T12:36:00Z").has_value());
    };

    "capability_process_output_gives_keys_and_expiry_and_refuses_what_is_incomplete"_test = [] {
        auto const good = aws::from_process_output(R"({"Version":1,"AccessKeyId":"A","SecretAccessKey":"S","SessionToken":"T","Expiration":"2015-08-30T12:36:00Z"})");
        auto const bare = aws::from_process_output(R"({"AccessKeyId":"A","SecretAccessKey":"S"})");

        expect(good.has_value() && good->keys.session_token == "T" && good->expires == std::optional<std::int64_t>{1440938160} && good->from == aws::origin::process);
        expect(bare.has_value() && !bare->expires.has_value());
        expect(!aws::from_process_output("not json").has_value() && !aws::from_process_output(R"({"AccessKeyId":"A"})").has_value());
        expect(!aws::from_process_output(R"({"AccessKeyId":"A","SecretAccessKey":"S","Expiration":"soon"})").has_value());
    };

    "capability_the_environment_wins_then_the_named_profile_then_the_default"_test = [] {
        auto fixture = standard();
        fixture.env = {{"AWS_ACCESS_KEY_ID", "AKIDENV"}, {"AWS_SECRET_ACCESS_KEY", "secretenv"}, {"AWS_SESSION_TOKEN", "tokenenv"}};
        auto const from_env = aws::resolve(fixture.sources(), "work");
        fixture.env.clear();
        auto const named = aws::resolve(fixture.sources(), "work");
        auto const fallback = aws::resolve(fixture.sources(), "");
        fixture.env = {{"AWS_PROFILE", "work"}};
        auto const by_variable = aws::resolve(fixture.sources(), "");

        expect(from_env->from == aws::origin::environment && from_env->keys.access_key == "AKIDENV" && from_env->keys.session_token == "tokenenv");
        expect(named->from == aws::origin::profile && named->keys.access_key == "AKIDWORK" && named->keys.session_token == "tokenwork");
        expect(fallback->keys.access_key == "AKIDDEFAULT" && by_variable->keys.access_key == "AKIDWORK");
    };

    "capability_a_profile_without_keys_runs_its_credential_process_by_argv"_test = [] {
        auto fixture = standard();
        fixture.output = R"({"AccessKeyId":"AP","SecretAccessKey":"SP","SessionToken":"TP"})";
        auto const got = aws::resolve(fixture.sources(), "sso");

        expect(got.has_value() && got->from == aws::origin::process && got->keys.access_key == "AP");
        expect(fixture.ran.size() == 1_u && fixture.ran.front() == std::vector<std::string>{"aws", "configure", "export-credentials", "--profile", "my sso", "--format", "process"});
    };

    "limit_a_failing_process_and_a_missing_profile_are_named_without_a_value"_test = [] {
        auto fixture = standard();
        fixture.output = std::unexpected{std::string{"exit 255"}};
        auto const failed = aws::resolve(fixture.sources(), "sso");
        auto const absent = aws::resolve(fixture.sources(), "nobody");

        expect(!failed.has_value() && failed.error().contains("profile sso") && failed.error().contains("exit 255"));
        expect(!absent.has_value() && absent.error().contains("no AWS credentials") && absent.error().contains("nobody"));
    };

    "capability_the_region_comes_from_the_provider_then_the_environment"_test = [] {
        auto fixture = standard();
        fixture.env = {{"AWS_DEFAULT_REGION", "ap-south-1"}};
        auto const from = fixture.sources();

        expect(aws::region_of(from, "eu-central-1") == "eu-central-1" && aws::region_of(from, "") == "ap-south-1");
        fixture.env = {{"AWS_REGION", "us-west-2"}, {"AWS_DEFAULT_REGION", "ap-south-1"}};
        expect(aws::region_of(fixture.sources(), "") == "us-west-2");
    };
};

}  // namespace

int main() {}

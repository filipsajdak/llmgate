export module llmgate.aws_credentials;

import std;
import llmgate.async;
import llmgate.gateway;
import llmgate.json;
import llmgate.process;
import llmgate.sigv4;

export namespace llmgate::aws {

using ini = std::map<std::string, std::map<std::string, std::string>>;
using runner = std::function<std::expected<std::string, std::string>(std::vector<std::string> const&)>;

constexpr std::string_view access_key_variable = "AWS_ACCESS_KEY_ID";
constexpr std::string_view secret_key_variable = "AWS_SECRET_ACCESS_KEY";
constexpr std::string_view session_token_variable = "AWS_SESSION_TOKEN";
constexpr std::string_view profile_variable = "AWS_PROFILE";
constexpr std::string_view region_variable = "AWS_REGION";
constexpr std::string_view default_region_variable = "AWS_DEFAULT_REGION";
constexpr std::string_view credentials_file_variable = "AWS_SHARED_CREDENTIALS_FILE";
constexpr std::string_view config_file_variable = "AWS_CONFIG_FILE";
constexpr std::string_view default_credentials_file = "~/.aws/credentials";
constexpr std::string_view default_config_file = "~/.aws/config";
constexpr std::string_view default_profile = "default";
constexpr std::string_view profile_section_prefix = "profile ";
constexpr std::int64_t refresh_margin_seconds = 60;
constexpr std::size_t iso_stamp_chars = 19;

enum class origin { environment, profile, process };

struct resolved {
    sigv4::credentials keys{};
    std::optional<std::int64_t> expires{};
    origin from = origin::environment;
};

struct sources {
    gateway::secret_readers read{};
    runner run{};
};

namespace detail {

constexpr std::string_view trimmed(std::string_view const text) {
    constexpr std::string_view blanks = " \t\r\n";
    auto const first = text.find_first_not_of(blanks);
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(blanks) - first + 1);
}

constexpr bool skipped(std::string_view const line) {
    return line.empty() || line.front() == '#' || line.front() == ';';
}

constexpr void accept_line(ini& out, std::string& section, std::string_view const line) {
    if (line.front() == '[' && line.back() == ']') {
        section = std::string{trimmed(line.substr(1, line.size() - 2))};
        out.try_emplace(section);
        return;
    }
    auto const equals = line.find('=');
    if (equals == std::string_view::npos || section.empty()) {
        return;
    }
    out[section][std::string{trimmed(line.substr(0, equals))}] = std::string{trimmed(line.substr(equals + 1))};
}

constexpr std::optional<std::int64_t> number_of(std::string_view const text) {
    std::int64_t value = 0;
    auto const parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

}  // namespace detail

constexpr ini parse_ini(std::string_view const text) {
    ini out;
    std::string section;
    for (auto const part : std::views::split(text, '\n')) {
        auto const line = detail::trimmed(std::string_view{part.begin(), part.end()});
        if (!detail::skipped(line)) {
            detail::accept_line(out, section, line);
        }
    }
    return out;
}

constexpr bool is_quote(char const c) {
    return c == '"' || c == '\'';
}

constexpr bool is_blank(char const c) {
    return c == ' ' || c == '\t';
}

struct splitter {
    std::vector<std::string> words{};
    std::string current{};
    char quote = '\0';
    bool open = false;

    constexpr void flush() {
        if (open) {
            words.push_back(std::exchange(current, {}));
        }
        open = false;
    }

    constexpr void take(char const c) {
        if (quote != '\0' && c != quote) {
            current.push_back(c);
        } else if (quote != '\0') {
            quote = '\0';
        } else if (is_quote(c)) {
            quote = c;
            open = true;
        } else if (is_blank(c)) {
            flush();
        } else {
            current.push_back(c);
            open = true;
        }
    }
};

constexpr std::vector<std::string> split_command(std::string_view const text) {
    splitter state;
    for (char const c : text) {
        state.take(c);
    }
    state.flush();
    return std::move(state.words);
}

namespace detail {

struct iso_field {
    std::size_t at;
    std::size_t size;
};

constexpr std::array<iso_field, 6> iso_fields{{{0, 4}, {5, 2}, {8, 2}, {11, 2}, {14, 2}, {17, 2}}};
constexpr std::array<iso_field, 4> iso_separators{{{4, '-'}, {7, '-'}, {13, ':'}, {16, ':'}}};

constexpr std::optional<std::array<std::int64_t, 6>> iso_numbers(std::string_view const text) {
    bool const shaped = text.size() >= iso_stamp_chars && (text[10] == 'T' || text[10] == ' ') &&
                        std::ranges::all_of(iso_separators, [text](iso_field const& mark) { return text[mark.at] == static_cast<char>(mark.size); });
    if (!shaped) {
        return std::nullopt;
    }
    std::array<std::int64_t, 6> numbers{};
    for (std::size_t index = 0; index < iso_fields.size(); ++index) {
        auto const value = number_of(text.substr(iso_fields[index].at, iso_fields[index].size));
        if (!value) {
            return std::nullopt;
        }
        numbers[index] = *value;
    }
    return numbers;
}

}  // namespace detail

constexpr std::optional<std::int64_t> parse_expiry(std::string_view const text) {
    auto const numbers = detail::iso_numbers(text);
    if (!numbers) {
        return std::nullopt;
    }
    auto const& [year, month, day, hour, minute, second] = *numbers;
    std::chrono::year_month_day const date{std::chrono::year{static_cast<int>(year)}, std::chrono::month{static_cast<unsigned>(month)}, std::chrono::day{static_cast<unsigned>(day)}};
    if (!date.ok()) {
        return std::nullopt;
    }
    return std::chrono::sys_days{date}.time_since_epoch().count() * 86400 + hour * 3600 + minute * 60 + second;
}

inline std::expected<resolved, std::string> from_process_output(std::string_view const output) {
    auto const parsed = json::parse(output, {.jsonc = false});
    if (!parsed) {
        return std::unexpected{"the credential_process did not print JSON"};
    }
    resolved out{.from = origin::process};
    out.keys = {.access_key = json::text_at(*parsed, {"AccessKeyId"}),
                .secret_key = json::text_at(*parsed, {"SecretAccessKey"}),
                .session_token = json::text_at(*parsed, {"SessionToken"})};
    if (out.keys.access_key.empty() || out.keys.secret_key.empty()) {
        return std::unexpected{"the credential_process output has no AccessKeyId and SecretAccessKey"};
    }
    if (auto const stamp = json::text_at(*parsed, {"Expiration"}); !stamp.empty()) {
        out.expires = parse_expiry(stamp);
        if (!out.expires) {
            return std::unexpected{"the credential_process Expiration is not an ISO 8601 time"};
        }
    }
    return out;
}

namespace detail {

inline std::string variable(sources const& from, std::string_view const name) {
    return from.read.env(std::string{name}).value_or(std::string{});
}

inline std::string file_named_by(sources const& from, std::string_view const variable_name, std::string_view const fallback) {
    auto path = variable(from, variable_name);
    return path.empty() ? std::string{fallback} : path;
}

inline std::optional<resolved> from_environment(sources const& from) {
    auto key = variable(from, access_key_variable);
    auto secret = variable(from, secret_key_variable);
    if (key.empty() || secret.empty()) {
        return std::nullopt;
    }
    return resolved{.keys = {.access_key = std::move(key), .secret_key = std::move(secret), .session_token = variable(from, session_token_variable)},
                    .from = origin::environment};
}

inline std::map<std::string, std::string> section_of(ini const& table, std::string_view const name, bool const prefixed) {
    auto const wanted = prefixed && name != default_profile ? std::string{profile_section_prefix} + std::string{name} : std::string{name};
    auto const found = table.find(wanted);
    return found == table.end() ? std::map<std::string, std::string>{} : found->second;
}

inline std::optional<resolved> from_profile(sources const& from, std::string_view const name) {
    auto const text = from.read.file(file_named_by(from, credentials_file_variable, default_credentials_file));
    if (!text) {
        return std::nullopt;
    }
    auto const section = section_of(parse_ini(*text), name, false);
    auto const key = section.find("aws_access_key_id");
    auto const secret = section.find("aws_secret_access_key");
    if (key == section.end() || secret == section.end()) {
        return std::nullopt;
    }
    auto const token = section.find("aws_session_token");
    return resolved{.keys = {.access_key = key->second, .secret_key = secret->second, .session_token = token == section.end() ? std::string{} : token->second},
                    .from = origin::profile};
}

inline std::optional<std::vector<std::string>> process_of(sources const& from, std::string_view const name) {
    auto const text = from.read.file(file_named_by(from, config_file_variable, default_config_file));
    if (!text) {
        return std::nullopt;
    }
    auto const section = section_of(parse_ini(*text), name, true);
    auto const found = section.find("credential_process");
    if (found == section.end()) {
        return std::nullopt;
    }
    return split_command(found->second);
}

}  // namespace detail

namespace detail {

inline std::string profile_name(sources const& from, std::string_view const requested) {
    for (auto const& candidate : {std::string{requested}, variable(from, profile_variable)}) {
        if (!candidate.empty()) {
            return candidate;
        }
    }
    return std::string{default_profile};
}

inline std::expected<resolved, std::string> from_process(sources const& from, std::string_view const name) {
    auto const command = process_of(from, name);
    if (!command || command->empty()) {
        return std::unexpected{std::format("no AWS credentials: set {} and {}, or add profile {} to {}, or give it a credential_process", access_key_variable, secret_key_variable,
                                           name, default_credentials_file)};
    }
    auto const output = from.run(*command);
    if (!output) {
        return std::unexpected{std::format("the credential_process of profile {} failed: {}", name, output.error())};
    }
    return from_process_output(*output);
}

}  // namespace detail

inline std::expected<resolved, std::string> resolve(sources const& from, std::string_view const requested_profile) {
    if (auto const env = detail::from_environment(from)) {
        return *env;
    }
    auto const name = detail::profile_name(from, requested_profile);
    if (auto const stored = detail::from_profile(from, name)) {
        return *stored;
    }
    return detail::from_process(from, name);
}

inline std::string region_of(sources const& from, std::string configured) {
    for (auto const name : {region_variable, default_region_variable}) {
        if (!configured.empty()) {
            break;
        }
        configured = detail::variable(from, name);
    }
    return configured;
}


inline std::expected<std::string, std::string> run_credential_process(std::vector<std::string> const& command) {
    auto const program = process::find_program(command.front());
    if (!program) {
        return std::unexpected{std::format("{} was not found on PATH", command.front())};
    }
    auto const output = process::run_capturing(*program, std::vector<std::string>(command.begin() + 1, command.end()));
    if (!output) {
        return std::unexpected{output.error().message()};
    }
    return *output;
}

inline gateway::secret_readers process_readers() {
    return {.env = [](std::string const& name) -> std::optional<std::string> {
                auto const* value = std::getenv(name.c_str());
                return value == nullptr ? std::nullopt : std::optional<std::string>{value};
            },
            .file = [](std::string const& path) -> std::optional<std::string> {
                std::ifstream in{process::expand_home(path)};
                if (!in) {
                    return std::nullopt;
                }
                return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
            }};
}

inline sources process_sources() {
    return {.read = process_readers(), .run = run_credential_process};
}

class aws_credential final : public gateway::credential {
public:
    aws_credential(sources from, std::string profile, std::string region, std::function<std::int64_t()> clock)
        : from_{std::move(from)}, profile_{std::move(profile)}, region_{std::move(region)}, clock_{std::move(clock)} {}

    async::task<std::expected<std::string, std::error_code>> bearer() override {
        co_return std::unexpected{std::make_error_code(std::errc::operation_not_supported)};
    }

    async::task<std::expected<gateway::aws_keys, std::string>> signing_keys() override {
        auto const region = region_of(from_, region_);
        if (region.empty()) {
            co_return std::unexpected{std::string{"no AWS region: set region on the provider or AWS_REGION"}};
        }
        if (!current_ || (current_->expires && clock_() + refresh_margin_seconds >= *current_->expires)) {
            auto fresh = resolve(from_, profile_);
            if (!fresh) {
                current_.reset();
                co_return std::unexpected{std::move(fresh.error())};
            }
            current_ = std::move(*fresh);
        }
        co_return gateway::aws_keys{.keys = current_->keys, .region = region};
    }

    void rejected() noexcept override {
        if (current_ && current_->from == origin::process) {
            current_.reset();
        }
    }

    std::optional<origin> origin_now() const {
        return current_ ? std::optional<origin>{current_->from} : std::nullopt;
    }

private:
    sources from_;
    std::string profile_;
    std::string region_;
    std::function<std::int64_t()> clock_;
    std::optional<resolved> current_{};
};

}  // namespace llmgate::aws

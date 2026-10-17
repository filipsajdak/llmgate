export module llmgate.usage;

import std;
import llmgate.json;
import llmgate.jsonscan;
import llmgate.text;

export namespace llmgate::usage {

using text::compact;

constexpr std::int64_t seconds_per_day = 86400;
constexpr std::size_t longest_session = 64;
constexpr unsigned percent_cap = 999;
constexpr unsigned default_warn_percent = 80;
constexpr std::string_view session_marker = "_session_";

constexpr std::uint64_t add_sat(std::uint64_t const left, std::uint64_t const right) noexcept {
    return left > std::numeric_limits<std::uint64_t>::max() - right ? std::numeric_limits<std::uint64_t>::max() : left + right;
}

struct tokens {
    std::uint64_t input = 0;
    std::uint64_t output = 0;
    std::uint64_t cache_read = 0;
    std::uint64_t cache_creation = 0;

    constexpr std::uint64_t counted() const noexcept { return add_sat(add_sat(input, output), cache_creation); }

    constexpr tokens& operator+=(tokens const& other) noexcept {
        input = add_sat(input, other.input);
        output = add_sat(output, other.output);
        cache_read = add_sat(cache_read, other.cache_read);
        cache_creation = add_sat(cache_creation, other.cache_creation);
        return *this;
    }

    friend constexpr bool operator==(tokens const&, tokens const&) = default;
};

struct totals {
    tokens used{};
    std::uint64_t requests = 0;

    friend constexpr bool operator==(totals const&, totals const&) = default;
};

struct record {
    std::int64_t time = 0;
    std::string requested{};
    std::string model{};
    std::string role{};
    std::string session{};
    tokens used{};
    std::int64_t duration_ms = 0;
    int status = 0;
    std::uint64_t repairs = 0;

    friend constexpr bool operator==(record const&, record const&) = default;
};

constexpr tokens read_tokens(json::value const& source) {
    auto const get = [&source](std::string_view const key) { return json::count_at(source, {key}).value_or(0); };
    return {.input = get("input_tokens"),
            .output = get("output_tokens"),
            .cache_read = get("cache_read_input_tokens"),
            .cache_creation = get("cache_creation_input_tokens")};
}

class observer {
public:
    constexpr void on_event(std::string_view const name, std::string_view const data) {
        if (name == "message_start") {
            start(data);
        } else if (name == "message_delta") {
            delta(data);
        }
    }

    constexpr void on_body(std::string_view const body) {
        auto const parsed = json::parse(body, {.jsonc = false});
        if (!parsed) {
            return;
        }
        model_ = json::text_at(*parsed, {"model"});
        if (auto const* usage = json::at(*parsed, {"usage"})) {
            used_ = read_tokens(*usage);
        }
    }

    constexpr std::string const& model() const noexcept { return model_; }
    constexpr tokens const& used() const noexcept { return used_; }

private:
    constexpr void start(std::string_view const data) {
        auto const parsed = json::parse(data, {.jsonc = false});
        if (!parsed) {
            return;
        }
        model_ = json::text_at(*parsed, {"message", "model"});
        if (auto const* usage = json::at(*parsed, {"message", "usage"})) {
            used_ = read_tokens(*usage);
        }
    }

    constexpr void delta(std::string_view const data) {
        auto const parsed = json::parse(data, {.jsonc = false});
        auto const* usage = parsed ? json::at(*parsed, {"usage"}) : nullptr;
        if (usage == nullptr) {
            return;
        }
        auto const seen = read_tokens(*usage);
        used_.input = std::max(used_.input, seen.input);
        used_.output = std::max(used_.output, seen.output);
        used_.cache_read = std::max(used_.cache_read, seen.cache_read);
        used_.cache_creation = std::max(used_.cache_creation, seen.cache_creation);
    }

    std::string model_{};
    tokens used_{};
};

constexpr std::size_t longest_label = 64;

constexpr std::string label(std::string_view const text) {
    std::string clean;
    for (auto const c : text.substr(0, longest_label)) {
        clean += c >= '!' && c <= '~' ? c : '?';
    }
    return clean;
}

constexpr bool session_character(char const c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
}

constexpr std::string checked_session(std::string_view const candidate) {
    if (candidate.empty() || candidate.size() > longest_session || !std::ranges::all_of(candidate, session_character)) {
        return {};
    }
    return std::string{candidate};
}

constexpr std::string session_from_user_id(std::string_view const user_id) {
    if (user_id.starts_with('{')) {
        auto const parsed = json::parse(user_id, {.jsonc = false});
        return parsed ? checked_session(json::text_at(*parsed, {"session_id"})) : std::string{};
    }
    auto const at = user_id.rfind(session_marker);
    return at == std::string_view::npos ? std::string{} : checked_session(user_id.substr(at + session_marker.size()));
}

constexpr std::string session_of(std::string_view const body) {
    auto const members = jsonscan::top_level_members(body);
    if (!members) {
        return {};
    }
    auto const found = jsonscan::members_named(*members, "metadata");
    if (found.size() != 1 || found.front().kind != jsonscan::value_kind::object) {
        return {};
    }
    auto const parsed = json::parse(body.substr(found.front().value_begin, found.front().value_end - found.front().value_begin), {.jsonc = false});
    return parsed ? session_from_user_id(json::text_at(*parsed, {"user_id"})) : std::string{};
}

constexpr bool contains_word(std::string_view const text, std::string_view const word) {
    auto const lower = [](char const c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    return !std::ranges::search(text, word, {}, lower, {}).empty();
}

constexpr std::string_view role_of(std::string_view const requested) {
    for (auto const role : std::to_array<std::string_view>({"opus", "sonnet", "haiku"})) {
        if (contains_word(requested, role)) {
            return role;
        }
    }
    return "other";
}

constexpr json::value number(std::uint64_t const value) {
    return json::value{static_cast<std::int64_t>(std::min<std::uint64_t>(value, std::numeric_limits<std::int64_t>::max()))};
}

constexpr std::string to_line(record const& entry) {
    auto line = json::write(json::make_object({
        {"time", json::value{entry.time}},
        {"requested", json::make_string(entry.requested)},
        {"model", json::make_string(entry.model)},
        {"role", json::make_string(entry.role)},
        {"session", json::make_string(entry.session)},
        {"input", number(entry.used.input)},
        {"output", number(entry.used.output)},
        {"cache_read", number(entry.used.cache_read)},
        {"cache_creation", number(entry.used.cache_creation)},
        {"duration_ms", json::value{entry.duration_ms}},
        {"status", json::value{static_cast<std::int64_t>(entry.status)}},
        {"repairs", number(entry.repairs)},
    }));
    line += '\n';
    return line;
}

constexpr std::optional<record> from_line(std::string_view const line) {
    auto const parsed = json::parse(line, {.jsonc = false});
    if (!parsed) {
        return std::nullopt;
    }
    auto const* time = json::at(*parsed, {"time"});
    auto const* whole = time == nullptr ? nullptr : std::get_if<std::int64_t>(&time->data);
    if (whole == nullptr) {
        return std::nullopt;
    }
    auto const count = [&parsed](std::string_view const key) { return json::count_at(*parsed, {key}).value_or(0); };
    auto const signed_at = [&parsed](std::string_view const key) {
        auto const* found = json::at(*parsed, {key});
        auto const* value = found == nullptr ? nullptr : std::get_if<std::int64_t>(&found->data);
        return value == nullptr ? std::int64_t{0} : *value;
    };
    return record{.time = *whole,
                  .requested = label(json::text_at(*parsed, {"requested"})),
                  .model = label(json::text_at(*parsed, {"model"})),
                  .role = label(json::text_at(*parsed, {"role"})),
                  .session = checked_session(json::text_at(*parsed, {"session"})),
                  .used = {.input = count("input"), .output = count("output"), .cache_read = count("cache_read"), .cache_creation = count("cache_creation")},
                  .duration_ms = signed_at("duration_ms"),
                  .status = static_cast<int>(std::clamp<std::int64_t>(signed_at("status"), 0, 999)),
                  .repairs = count("repairs")};
}

constexpr std::int64_t day_of(std::int64_t const seconds) noexcept {
    auto day = seconds / seconds_per_day;
    if (seconds % seconds_per_day < 0) {
        --day;
    }
    return day;
}

struct civil_date {
    std::int64_t year;
    unsigned month;
    unsigned day;
};

constexpr civil_date civil_from_day(std::int64_t const days_since_epoch) noexcept {
    constexpr std::int64_t era_days = 146097;
    auto const shifted = days_since_epoch + 719468;
    auto const era = (shifted >= 0 ? shifted : shifted - (era_days - 1)) / era_days;
    auto const day_of_era = static_cast<unsigned>(shifted - era * era_days);
    auto const year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    auto const day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    auto const month_index = (5 * day_of_year + 2) / 153;
    auto const day = day_of_year - (153 * month_index + 2) / 5 + 1;
    auto const month = month_index < 10 ? month_index + 3 : month_index - 9;
    return {.year = static_cast<std::int64_t>(year_of_era) + era * 400 + (month <= 2 ? 1 : 0), .month = month, .day = day};
}

constexpr std::int64_t month_of(std::int64_t const day) noexcept {
    auto const date = civil_from_day(day);
    return date.year * 12 + static_cast<std::int64_t>(date.month) - 1;
}

constexpr std::string two_digits(unsigned const value) {
    return (value < 10 ? "0" : "") + text::decimal(value);
}

constexpr std::string iso_date(std::int64_t const day) {
    auto const date = civil_from_day(day);
    return text::decimal(date.year) + "-" + two_digits(date.month) + "-" + two_digits(date.day);
}

struct group {
    std::int64_t day = 0;
    std::string model{};
    totals sum{};
};

class ledger {
public:
    constexpr void add(record const& entry) {
        auto const day = day_of(entry.time);
        auto const name = label(entry.model.empty() ? entry.requested : entry.model);
        auto found = std::ranges::find_if(groups_, [&](group const& item) { return item.day == day && item.model == name; });
        if (found == groups_.end()) {
            groups_.push_back({.day = day, .model = name});
            found = std::prev(groups_.end());
        }
        found->sum.used += entry.used;
        found->sum.requests = add_sat(found->sum.requests, 1);
    }

    constexpr totals on_day(std::int64_t const day) const {
        return sum_where([day](group const& item) { return item.day == day; });
    }

    constexpr totals in_month(std::int64_t const month) const {
        return sum_where([month](group const& item) { return month_of(item.day) == month; });
    }

    constexpr std::span<group const> groups() const noexcept { return groups_; }

private:
    template <class Pick>
    constexpr totals sum_where(Pick&& pick) const {
        totals all;
        for (auto const& item : groups_) {
            if (pick(item)) {
                all.used += item.sum.used;
                all.requests = add_sat(all.requests, item.sum.requests);
            }
        }
        return all;
    }

    std::vector<group> groups_{};
};

struct load_result {
    ledger loaded{};
    std::size_t skipped = 0;
};

constexpr load_result load(std::string_view text) {
    load_result result;
    while (!text.empty()) {
        auto const end = text.find('\n');
        auto const line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (line.empty()) {
            continue;
        }
        if (auto const entry = from_line(line)) {
            result.loaded.add(*entry);
        } else {
            ++result.skipped;
        }
    }
    return result;
}

struct budget {
    std::uint64_t daily_tokens = 0;
    std::uint64_t monthly_tokens = 0;
    unsigned warn_percent = default_warn_percent;
};

enum class level { ok, warning, exhausted };

struct verdict {
    level state = level::ok;
    std::string_view window{};
    std::uint64_t used = 0;
    std::uint64_t limit = 0;
    unsigned percent = 0;
};

constexpr unsigned percent_of(std::uint64_t const used, std::uint64_t const limit) noexcept {
    if (limit == 0) {
        return 0;
    }
    auto const whole = used / limit;
    if (whole >= percent_cap / 100 + 1) {
        return percent_cap;
    }
    auto const percent = whole * 100 + (used % limit) * 100 / limit;
    return static_cast<unsigned>(std::min<std::uint64_t>(percent, percent_cap));
}

constexpr verdict evaluate(budget const& limits, std::uint64_t const today, std::uint64_t const month) noexcept {
    verdict best;
    auto const consider = [&best](std::string_view const window, std::uint64_t const used, std::uint64_t const limit) {
        if (limit == 0) {
            return;
        }
        auto const percent = percent_of(used, limit);
        if (best.limit == 0 || percent > best.percent) {
            best = {.state = level::ok, .window = window, .used = used, .limit = limit, .percent = percent};
        }
    };
    consider("daily", today, limits.daily_tokens);
    consider("monthly", month, limits.monthly_tokens);
    if (best.percent >= 100) {
        best.state = level::exhausted;
    } else if (best.limit != 0 && best.percent >= limits.warn_percent) {
        best.state = level::warning;
    }
    return best;
}

constexpr std::string_view level_name(level const state) noexcept {
    return state == level::exhausted ? "exhausted" : state == level::warning ? "warning" : "ok";
}

constexpr std::string refusal_message(verdict const& now) {
    auto message = "llmgate budget used up: " + std::string{now.window} + " " + compact(now.used) + " of " + compact(now.limit) + " tokens; ";
    message += now.window == "daily" ? "resets at 00:00 UTC" : "resets on the first day of next month (UTC)";
    return message;
}

constexpr json::value totals_object(totals const& sum) {
    return json::make_object({{"input", number(sum.used.input)},
                              {"output", number(sum.used.output)},
                              {"cache_read", number(sum.used.cache_read)},
                              {"cache_creation", number(sum.used.cache_creation)},
                              {"requests", number(sum.requests)}});
}

constexpr json::value verdict_object(verdict const& now) {
    return json::make_object({{"state", json::make_string(std::string{level_name(now.state)})},
                              {"window", json::make_string(std::string{now.window})},
                              {"used", number(now.used)},
                              {"limit", number(now.limit)},
                              {"percent", number(now.percent)}});
}

constexpr std::string summary_json(ledger const& book, budget const& limits, std::int64_t const now_seconds) {
    auto const day = day_of(now_seconds);
    auto const today = book.on_day(day);
    auto const month = book.in_month(month_of(day));
    return json::write(json::make_object({{"today", totals_object(today)},
                                          {"month", totals_object(month)},
                                          {"budget", verdict_object(evaluate(limits, today.used.counted(), month.used.counted()))}}));
}

struct headline {
    std::uint64_t today = 0;
    std::string state{};
    std::string window{};
    std::uint64_t used = 0;
    std::uint64_t limit = 0;
    unsigned percent = 0;
};

constexpr headline headline_from(std::string_view const stats_body) {
    auto const parsed = json::parse(stats_body, {.jsonc = false});
    if (!parsed) {
        return {};
    }
    auto const* usage = json::at(*parsed, {"usage"});
    if (usage == nullptr) {
        return {};
    }
    auto const count = [usage](std::initializer_list<std::string_view> const path) { return json::count_at(*usage, path).value_or(0); };
    return {.today = add_sat(add_sat(count({"today", "input"}), count({"today", "output"})), count({"today", "cache_creation"})),
            .state = json::text_at(*usage, {"budget", "state"}),
            .window = json::text_at(*usage, {"budget", "window"}),
            .used = count({"budget", "used"}),
            .limit = count({"budget", "limit"}),
            .percent = static_cast<unsigned>(std::min<std::uint64_t>(count({"budget", "percent"}), percent_cap))};
}

constexpr std::string pad_right(std::string text, std::size_t const width) {
    if (text.size() < width) {
        text.append(width - text.size(), ' ');
    }
    return text;
}

constexpr std::string pad_left(std::string text, std::size_t const width) {
    if (text.size() < width) {
        text.insert(0, width - text.size(), ' ');
    }
    return text;
}

constexpr std::vector<group> rows_between(ledger const& book, std::int64_t const first_day, std::int64_t const last_day) {
    std::vector<group> rows;
    std::ranges::copy_if(book.groups(), std::back_inserter(rows), [&](group const& item) { return item.day >= first_day && item.day <= last_day; });
    std::ranges::sort(rows, [](group const& left, group const& right) { return left.day != right.day ? left.day < right.day : left.model < right.model; });
    return rows;
}

constexpr std::string report(ledger const& book, std::int64_t const first_day, std::int64_t const last_day) {
    constexpr std::size_t model_width = 22;
    constexpr std::size_t number_width = 12;
    std::string out = pad_right("date", 12) + pad_right("model", model_width) + pad_left("requests", 9) + pad_left("input", number_width) +
                      pad_left("output", number_width) + pad_left("cache read", number_width) + pad_left("cache write", number_width) + "\n";
    totals all;
    for (auto const& item : rows_between(book, first_day, last_day)) {
        out += pad_right(iso_date(item.day), 12) + pad_right(item.model, model_width) + pad_left(text::decimal(item.sum.requests), 9) +
               pad_left(compact(item.sum.used.input), number_width) + pad_left(compact(item.sum.used.output), number_width) +
               pad_left(compact(item.sum.used.cache_read), number_width) + pad_left(compact(item.sum.used.cache_creation), number_width) + "\n";
        all.used += item.sum.used;
        all.requests = add_sat(all.requests, item.sum.requests);
    }
    out += pad_right("total", 12 + model_width) + pad_left(text::decimal(all.requests), 9) + pad_left(compact(all.used.input), number_width) +
           pad_left(compact(all.used.output), number_width) + pad_left(compact(all.used.cache_read), number_width) +
           pad_left(compact(all.used.cache_creation), number_width) + "\n";
    return out;
}

constexpr std::string report_json(ledger const& book, std::int64_t const first_day, std::int64_t const last_day) {
    json::array rows;
    for (auto const& item : rows_between(book, first_day, last_day)) {
        auto row = totals_object(item.sum);
        auto& members = std::get<json::object>(row.data);
        members.insert(members.begin(), {"model", json::make_string(item.model)});
        members.insert(members.begin(), {"date", json::make_string(iso_date(item.day))});
        rows.push_back(std::move(row));
    }
    return json::write(json::make_object({{"days", json::value{static_cast<std::int64_t>(last_day - first_day + 1)}}, {"rows", json::value{std::move(rows)}}}), 2);
}

constexpr bool both_observed(std::string_view const start, std::string_view const delta, tokens const expected) {
    observer seen;
    seen.on_event("message_start", start);
    seen.on_event("message_delta", delta);
    return seen.used() == expected;
}

static_assert(both_observed(R"({"type":"message_start","message":{"model":"m","usage":{"input_tokens":10,"cache_read_input_tokens":4,"output_tokens":1}}})",
                            R"({"type":"message_delta","delta":{},"usage":{"output_tokens":7}})",
                            {.input = 10, .output = 7, .cache_read = 4}));
static_assert(both_observed(R"({"message":{"usage":{"output_tokens":9}}})", R"({"usage":{"output_tokens":3}})", {.output = 9}));
static_assert(both_observed("not json", "[]", {}));
static_assert([] {
    observer seen;
    seen.on_body(R"({"model":"claude-x","usage":{"input_tokens":5,"output_tokens":2,"cache_creation_input_tokens":1}})");
    return seen.model() == "claude-x" && seen.used() == tokens{.input = 5, .output = 2, .cache_creation = 1};
}());
static_assert(label("claude-x") == "claude-x" && label("a\x1b[31mb") == "a?[31mb" && label("sp ace\n") == "sp?ace?" && label(std::string(100, 'a')).size() == longest_label);
static_assert(from_line(R"({"time":1,"model":"m\u001b[2Jx","requested":"\u0007r"})")->model == "m?[2Jx" && from_line(R"({"time":1,"session":"bad\u001bid"})")->session.empty());
static_assert(session_of(R"({"model":"m","metadata":{"user_id":"user_ab_account_cd_session_5f3a-90"}})") == "5f3a-90");
static_assert(session_of(R"({"metadata":{"user_id":"{\"device_id\":\"d\",\"session_id\":\"s-1\"}"}})") == "s-1");
static_assert(session_of(R"({"metadata":{"user_id":"no marker"}})").empty() && session_of("[]").empty() && session_of(R"({"metadata":3})").empty());
static_assert(session_of(R"({"metadata":{"user_id":"x_session_bad id!"}})").empty());
static_assert(role_of("claude-Sonnet-5-5") == "sonnet" && role_of("claude-haiku-5-5") == "haiku" && role_of("gpt") == "other");
static_assert(from_line(to_line({.time = 1760000000, .requested = "a", .model = "b", .role = "opus", .session = "s", .used = {1, 2, 3, 4}, .duration_ms = 9, .status = 200, .repairs = 2})) ==
              record{.time = 1760000000, .requested = "a", .model = "b", .role = "opus", .session = "s", .used = {1, 2, 3, 4}, .duration_ms = 9, .status = 200, .repairs = 2});
static_assert(!from_line("{}") && !from_line("garbage") && from_line(R"({"time":5})")->time == 5);
static_assert(day_of(0) == 0 && day_of(86399) == 0 && day_of(86400) == 1 && day_of(-1) == -1 && day_of(-86400) == -1 && day_of(-86401) == -2);
static_assert(iso_date(0) == "1970-01-01" && iso_date(day_of(1760659200)) == "2025-10-17" && iso_date(day_of(951782400)) == "2000-02-29" && iso_date(-1) == "1969-12-31");
static_assert(month_of(0) == 1970 * 12 && month_of(31) == 1970 * 12 + 1);
static_assert(percent_of(50, 100) == 50 && percent_of(1, 0) == 0 && percent_of(300, 100) == 300 && percent_of(std::numeric_limits<std::uint64_t>::max(), 1) == percent_cap);
static_assert(evaluate({}, 5, 5).limit == 0 && evaluate({.daily_tokens = 100}, 79, 0).state == level::ok);
static_assert(evaluate({.daily_tokens = 100}, 80, 0).state == level::warning && evaluate({.daily_tokens = 100}, 100, 0).state == level::exhausted);
static_assert(evaluate({.daily_tokens = 100, .monthly_tokens = 1000}, 10, 950).window == "monthly");
static_assert(compact(999) == "999" && compact(1000) == "1k" && compact(412345) == "412k" && compact(1250000) == "1.2M");
static_assert(refusal_message({.state = level::exhausted, .window = "daily", .used = 1200000, .limit = 1000000, .percent = 120}) ==
              "llmgate budget used up: daily 1.2M of 1.0M tokens; resets at 00:00 UTC");
static_assert([] {
    ledger book;
    book.add({.time = 100, .model = "m", .used = {.input = 2, .output = 3}});
    book.add({.time = 200, .model = "m", .used = {.input = 1, .cache_read = 50}});
    book.add({.time = 90000, .requested = "r", .used = {.output = 7}});
    return book.on_day(0) == totals{.used = {3, 3, 50, 0}, .requests = 2} && book.on_day(1).requests == 1 && book.in_month(month_of(0)).requests == 3 &&
           book.groups().size() == 2 + 0 && book.groups()[1].model == "r";
}());
static_assert([] {
    ledger book;
    book.add({.time = 100, .model = "b", .used = {.input = 2}});
    book.add({.time = 90000, .model = "a", .used = {.output = 1500}});
    book.add({.time = 90100, .model = "a", .used = {.output = 10}});
    auto const text = report(book, 0, 1);
    auto const json_text = report_json(book, 1, 1);
    return rows_between(book, 0, 1).size() == 2 && rows_between(book, 5, 6).empty() && text.contains("1970-01-02") && text.contains("1k") &&
           text.starts_with("date") && json_text.contains("\"date\": \"1970-01-02\"") && json_text.contains("\"requests\": 2") && !json_text.contains("1970-01-01");
}());
static_assert(load(to_line({.time = 5, .model = "m"}) + "junk\n\n" + to_line({.time = 6, .model = "m"})).skipped == 1);
static_assert(headline_from(R"({"usage":{"today":{"input":5,"output":2,"cache_read":99,"cache_creation":1},"budget":{"state":"warning","window":"daily","used":8,"limit":10,"percent":80}}})").today == 8);
static_assert(headline_from("x").state.empty() && headline_from("{}").limit == 0);

}  // namespace llmgate::usage

module;

#include <meta>

export module llmgate.usage;

import std;
import llmgate.doc;
import llmgate.json;
import llmgate.jsonscan;
import llmgate.lenient;
import llmgate.members;
import llmgate.reflect;
import llmgate.role;
import llmgate.text;

export namespace llmgate::usage {

using text::compact;

constexpr std::int64_t seconds_per_day = 86400;
constexpr std::size_t longest_session = 64;
constexpr unsigned percent_cap = 999;
constexpr int status_cap = 999;
constexpr unsigned default_warn_percent = 80;
constexpr std::string_view session_marker = "_session_";

constexpr std::uint64_t add_sat(std::uint64_t const left, std::uint64_t const right) noexcept {
    return left > std::numeric_limits<std::uint64_t>::max() - right ? std::numeric_limits<std::uint64_t>::max() : left + right;
}

constexpr std::size_t column_capacity = 16;

struct column {
    char text[column_capacity]{};

    consteval column(std::string_view const heading) {
        if (heading.size() >= column_capacity) {
            throw "usage::column: the heading does not fit in column_capacity";
        }
        std::ranges::copy(heading, text);
    }
};

struct uncounted_t {};
inline constexpr uncounted_t uncounted{};

struct tokens {
    [[= column{"input"}]] std::uint64_t input = 0;
    [[= column{"output"}]] std::uint64_t output = 0;
    [[= column{"cache read"}]] [[= uncounted]] std::uint64_t cache_read = 0;
    [[= column{"cache write"}]] std::uint64_t cache_creation = 0;

    constexpr std::uint64_t counted() const noexcept;
    constexpr tokens& operator+=(tokens const& other) noexcept;
    constexpr void raise_to(tokens const& seen) noexcept;

    friend constexpr bool operator==(tokens const&, tokens const&) = default;
};

template <class T>
inline constexpr auto members_of = std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()));

template <class T>
inline constexpr std::meta::info type_info = ^^T;

consteval bool every_member_counts(std::span<std::meta::info const> const fields) {
    return std::ranges::all_of(fields, [](std::meta::info const field) { return std::meta::type_of(field) == type_info<std::uint64_t>; });
}

static_assert(every_member_counts(members_of<tokens>));

constexpr std::uint64_t tokens::counted() const noexcept {
    std::uint64_t sum = 0;
    template for (constexpr auto field : members_of<tokens>) {
        if constexpr (!reflect::has_annotation<uncounted_t>(field)) {
            sum = add_sat(sum, this->[:field:]);
        }
    }
    return sum;
}

constexpr tokens& tokens::operator+=(tokens const& other) noexcept {
    template for (constexpr auto field : members_of<tokens>) {
        this->[:field:] = add_sat(this->[:field:], other.[:field:]);
    }
    return *this;
}

constexpr void tokens::raise_to(tokens const& seen) noexcept {
    template for (constexpr auto field : members_of<tokens>) {
        this->[:field:] = std::max(this->[:field:], seen.[:field:]);
    }
}

consteval std::string_view heading_of(std::meta::info const field) {
    auto const named = reflect::annotation_of<column>(field);
    std::string_view const heading{named->text};
    return {std::define_static_string(heading), heading.size()};
}

template <class To, class From>
    requires(members_of<To>.size() == members_of<From>.size())
constexpr To copy_by_position(From const& from) {
    To to{};
    template for (constexpr auto at : std::define_static_array(std::views::iota(std::size_t{0}, members_of<To>.size()))) {
        to.[:members_of<To>[at]:] = from.[:members_of<From>[at]:];
    }
    return to;
}

struct totals {
    [[= json::flatten]] tokens used{};
    std::uint64_t requests = 0;

    friend constexpr bool operator==(totals const&, totals const&) = default;
};

struct record {
    [[= json::required]] std::int64_t time = 0;
    std::string requested{};
    std::string model{};
    std::string role{};
    std::string provider{};
    std::string session{};
    [[= json::flatten]] tokens used{};
    std::int64_t duration_ms = 0;
    int status = 0;
    std::uint64_t repairs = 0;
    std::string stop_reason{};
    std::string error{};
    std::string kind{};
    std::string verdict{};
    [[= json::key{"class"}]] std::string request_class{};
    std::string agent_type{};
    bool fallback = false;
    std::string labels{};

    friend constexpr bool operator==(record const&, record const&) = default;
};

struct usage_counts {
    [[= json::key{"input_tokens"}]] std::uint64_t input = 0;
    [[= json::key{"output_tokens"}]] std::uint64_t output = 0;
    [[= json::key{"cache_read_input_tokens"}]] std::uint64_t cache_read = 0;
    [[= json::key{"cache_creation_input_tokens"}]] std::uint64_t cache_creation = 0;
};

constexpr tokens read_tokens(json::value const& source) {
    return copy_by_position<tokens>(lenient::read<usage_counts>(source).value_or(usage_counts{}));
}

constexpr std::size_t longest_label = 64;
constexpr std::size_t longest_error = 200;
constexpr std::string_view classifier_marker = "</block>";
constexpr std::string_view severity_marker = "</severity>";

constexpr std::size_t longest_reply_text = 4096;
constexpr std::string_view classifier_kind = "classifier";

enum class reply_verdict { block, allow, unparseable };
static_assert(reflect::unique_values<reply_verdict>());

constexpr std::string_view skip_blanks(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\n' || text.front() == '\t' || text.front() == '\r')) {
        text.remove_prefix(1);
    }
    return text;
}

constexpr bool ends_verdict(std::string_view const rest) {
    return rest.empty() || rest == "</block>";
}

constexpr reply_verdict verdict_of(std::string_view text) {
    constexpr std::string_view thinking_open = "<thinking>";
    constexpr std::string_view thinking_close = "</thinking>";
    constexpr std::string_view block_open = "<block>";
    text = skip_blanks(text);
    if (text.starts_with(thinking_open)) {
        auto const end = text.find(thinking_close);
        if (end == std::string_view::npos) {
            return reply_verdict::unparseable;
        }
        text = skip_blanks(text.substr(end + thinking_close.size()));
    }
    if (!text.starts_with(block_open)) {
        return reply_verdict::unparseable;
    }
    text.remove_prefix(block_open.size());
    if (text.starts_with("yes") && ends_verdict(text.substr(std::string_view{"yes"}.size()))) {
        return reply_verdict::block;
    }
    return text.starts_with("no") && ends_verdict(text.substr(std::string_view{"no"}.size())) ? reply_verdict::allow : reply_verdict::unparseable;
}

constexpr std::string clean_text(std::string_view const text, std::size_t const limit) {
    std::string clean;
    for (auto const c : text.substr(0, limit)) {
        clean += c >= '!' && c <= '~' ? c : (c == ' ' ? ' ' : '?');
    }
    return clean;
}

constexpr std::string label(std::string_view const text) {
    std::string clean;
    for (auto const c : text.substr(0, longest_label)) {
        clean += c >= '!' && c <= '~' ? c : '?';
    }
    return clean;
}

constexpr std::string error_text(json::value const& parsed) {
    auto message = json::text_at(parsed, {"error", "message"});
    if (message.empty()) {
        message = json::text_at(parsed, {"error", "type"});
    }
    return clean_text(message, longest_error);
}

constexpr std::string error_of_body(std::string_view const body) {
    auto const parsed = json::parse(body, {.jsonc = false});
    return parsed ? error_text(*parsed) : std::string{};
}

constexpr std::string kind_of(std::string_view const body) {
    auto const members = jsonscan::top_level_members(body);
    if (!members) {
        return "main";
    }
    for (auto const& item : jsonscan::members_named(*members, "stop_sequences")) {
        auto const stops = body.substr(item.value_begin, item.value_end - item.value_begin);
        if (stops.contains(classifier_marker)) {
            return "classifier";
        }
        if (stops.contains(severity_marker)) {
            return "severity";
        }
    }
    return "main";
}

class observer {
public:
    constexpr void on_event(std::string_view const name, std::string_view const data) {
        if (name == "message_start") {
            start(data);
        } else if (name == "message_delta") {
            delta(data);
        } else if (name == "error") {
            failure(data);
        } else if (collecting_ && name == "content_block_delta") {
            collect(data);
        }
    }

    constexpr void collect_text(bool const on) noexcept { collecting_ = on; }
    constexpr std::string const& text() const noexcept { return text_; }

    constexpr void on_body(std::string_view const body) {
        auto const parsed = json::parse(body, {.jsonc = false});
        if (!parsed) {
            return;
        }
        model_ = json::text_at(*parsed, {"model"});
        stop_reason_ = label(json::text_at(*parsed, {"stop_reason"}));
        if (collecting_) {
            collect_body_text(*parsed);
        }
        if (auto const* usage = json::at(*parsed, {"usage"})) {
            used_ = read_tokens(*usage);
        }
    }

    constexpr std::string const& model() const noexcept { return model_; }
    constexpr std::string const& stop_reason() const noexcept { return stop_reason_; }
    constexpr std::string const& error() const noexcept { return error_; }
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
        if (!parsed) {
            return;
        }
        if (auto reason = json::text_at(*parsed, {"delta", "stop_reason"}); !reason.empty()) {
            stop_reason_ = label(reason);
        }
        if (auto const* usage = json::at(*parsed, {"usage"})) {
            used_.raise_to(read_tokens(*usage));
        }
    }

    constexpr void collect(std::string_view const data) {
        auto const parsed = json::parse(data, {.jsonc = false});
        if (parsed && json::text_at(*parsed, {"delta", "type"}) == "text_delta") {
            append_reply_text(json::text_at(*parsed, {"delta", "text"}));
        }
    }

    constexpr void collect_body_text(json::value const& message) {
        auto const* content = json::at(message, {"content"});
        auto const* blocks = content == nullptr ? nullptr : std::get_if<json::array>(&content->data);
        for (auto const& block : blocks == nullptr ? json::array{} : *blocks) {
            if (json::text_at(block, {"type"}) == "text") {
                append_reply_text(json::text_at(block, {"text"}));
            }
        }
    }

    constexpr void append_reply_text(std::string_view const piece) {
        text_.append(piece.substr(0, longest_reply_text - std::min(text_.size(), longest_reply_text)));
    }

    constexpr void failure(std::string_view const data) {
        auto const parsed = json::parse(data, {.jsonc = false});
        error_ = parsed ? error_text(*parsed) : std::string{};
    }

    bool collecting_ = false;
    std::string text_{};
    std::string stop_reason_{};
    std::string error_{};
    std::string model_{};
    tokens used_{};
};

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

constexpr std::string_view role_of(std::string_view const requested) {
    return roles::name_of(roles::of(requested));
}

constexpr std::string to_line(record const& entry) {
    auto line = json::write(json::to_value(entry));
    line += '\n';
    return line;
}

constexpr record cleaned(record entry) {
    entry.requested = label(entry.requested);
    entry.model = label(entry.model);
    entry.role = label(entry.role);
    entry.provider = label(entry.provider);
    entry.session = checked_session(entry.session);
    entry.stop_reason = label(entry.stop_reason);
    entry.error = clean_text(entry.error, longest_error);
    entry.kind = label(entry.kind);
    entry.verdict = label(entry.verdict);
    entry.status = std::clamp(entry.status, 0, status_cap);
    return entry;
}

constexpr std::optional<record> from_line(std::string_view const line) {
    auto const parsed = json::parse(line, {.jsonc = false});
    if (!parsed) {
        return std::nullopt;
    }
    auto entry = lenient::read<record>(*parsed);
    if (!entry) {
        return std::nullopt;
    }
    return cleaned(std::move(*entry));
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
    std::string provider{};
    std::string model{};
    totals sum{};
};

constexpr std::string_view unclassified = "unclassified";

struct class_group {
    std::int64_t day = 0;
    std::string request_class{};
    totals sum{};
};

struct score_stats {
    totals sum{};
    std::uint64_t duration_ms = 0;
    std::uint64_t slowest_ms = 0;
    std::uint64_t failures = 0;
    std::uint64_t fallbacks = 0;
};

struct detail_group {
    std::int64_t day = 0;
    std::string provider{};
    std::string model{};
    std::string request_class{};
    std::string agent_type{};
    std::string label{};
    score_stats stats{};
};

constexpr std::string_view unlabelled = "unlabelled";

constexpr bool failed(record const& entry) { return entry.status >= 400 || !entry.error.empty(); }

class ledger {
public:
    constexpr void add(record const& entry) {
        auto const day = day_of(entry.time);
        auto const name = label(entry.model.empty() ? entry.requested : entry.model);
        auto const who = label(entry.provider);
        auto found = std::ranges::find_if(groups_, [&](group const& item) { return item.day == day && item.model == name && item.provider == who; });
        if (found == groups_.end()) {
            groups_.push_back({.day = day, .provider = who, .model = name});
            found = std::prev(groups_.end());
        }
        found->sum.used += entry.used;
        found->sum.requests = add_sat(found->sum.requests, 1);
        add_class(day, entry);
        add_detail(details_, day, entry, {});
        auto const names = labels_of(entry.labels);
        for (auto const& one : names.empty() ? std::vector<std::string>{std::string{}} : names) {
            add_detail(label_details_, day, entry, one);
        }
    }

    constexpr totals on_day(std::int64_t const day) const {
        return sum_where([day](group const& item) { return item.day == day; });
    }

    constexpr totals in_month(std::int64_t const month) const {
        return sum_where([month](group const& item) { return month_of(item.day) == month; });
    }

    constexpr std::span<group const> groups() const noexcept { return groups_; }
    constexpr std::span<class_group const> classes() const noexcept { return classes_; }
    constexpr std::span<detail_group const> details() const noexcept { return details_; }
    constexpr std::span<detail_group const> label_details() const noexcept { return label_details_; }

private:
    static constexpr std::vector<std::string> labels_of(std::string_view const csv) {
        std::vector<std::string> out;
        for (auto const part : std::views::split(csv, ',')) {
            if (std::string_view const one{part.begin(), part.end()}; !one.empty()) {
                out.emplace_back(label(one));
            }
        }
        return out;
    }

    static constexpr std::string or_unclassified(std::string const& value) { return value.empty() ? std::string{unclassified} : label(value); }

    static constexpr void count_into(score_stats& stats, record const& entry) {
        auto const took = static_cast<std::uint64_t>(std::max<std::int64_t>(entry.duration_ms, 0));
        stats.sum.used += entry.used;
        stats.sum.requests = add_sat(stats.sum.requests, 1);
        stats.duration_ms = add_sat(stats.duration_ms, took);
        stats.slowest_ms = std::max(stats.slowest_ms, took);
        stats.failures = add_sat(stats.failures, failed(entry) ? 1 : 0);
        stats.fallbacks = add_sat(stats.fallbacks, entry.fallback ? 1 : 0);
    }

    static constexpr void add_detail(std::vector<detail_group>& into, std::int64_t const day, record const& entry, std::string const& one_label) {
        detail_group key{.day = day,
                         .provider = label(entry.provider),
                         .model = label(entry.model.empty() ? entry.requested : entry.model),
                         .request_class = or_unclassified(entry.request_class),
                         .agent_type = or_unclassified(entry.agent_type),
                         .label = one_label};
        auto found = std::ranges::find_if(into, [&key](detail_group const& item) {
            return item.day == key.day && item.provider == key.provider && item.model == key.model && item.request_class == key.request_class &&
                   item.agent_type == key.agent_type && item.label == key.label;
        });
        if (found == into.end()) {
            into.push_back(std::move(key));
            found = std::prev(into.end());
        }
        count_into(found->stats, entry);
    }

    constexpr void add_class(std::int64_t const day, record const& entry) {
        auto const name = entry.request_class.empty() ? std::string{unclassified} : label(entry.request_class);
        auto found = std::ranges::find_if(classes_, [&](class_group const& item) { return item.day == day && item.request_class == name; });
        if (found == classes_.end()) {
            classes_.push_back({.day = day, .request_class = name});
            found = std::prev(classes_.end());
        }
        found->sum.used += entry.used;
        found->sum.requests = add_sat(found->sum.requests, 1);
    }

    template <std::predicate<group const&> Pick>
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
    std::vector<class_group> classes_{};
    std::vector<detail_group> details_{};
    std::vector<detail_group> label_details_{};
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
    [[= reflect::doc{"Tokens allowed per UTC day; 0 means no limit."}]] std::uint64_t daily_tokens = 0;
    [[= reflect::doc{"Tokens allowed per UTC month; 0 means no limit."}]] std::uint64_t monthly_tokens = 0;
    [[= reflect::doc{"Percent of a budget after which the status line turns to high."}]] unsigned warn_percent = default_warn_percent;
};

enum class level { ok, warning, exhausted };
static_assert(reflect::unique_values<level>());

struct verdict {
    level state = level::ok;
    std::string window{};
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

constexpr verdict evaluate(budget const& limits, std::uint64_t const today, std::uint64_t const month) {
    verdict best;
    auto const consider = [&best](std::string_view const window, std::uint64_t const used, std::uint64_t const limit) {
        if (limit == 0) {
            return;
        }
        auto const percent = percent_of(used, limit);
        if (best.limit == 0 || percent > best.percent) {
            best = {.state = level::ok, .window = std::string{window}, .used = used, .limit = limit, .percent = percent};
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
    return reflect::enum_name(state);
}

constexpr std::string refusal_message(verdict const& now) {
    auto message = "llmgate budget used up: " + std::string{now.window} + " " + compact(now.used) + " of " + compact(now.limit) + " tokens; ";
    message += now.window == "daily" ? "resets at 00:00 UTC" : "resets on the first day of next month (UTC)";
    return message;
}

struct summary {
    totals today{};
    totals month{};
    verdict budget{};
};

constexpr summary summary_of(ledger const& book, budget const& limits, std::int64_t const now_seconds) {
    auto const day = day_of(now_seconds);
    auto const today = book.on_day(day);
    auto const month = book.in_month(month_of(day));
    return {.today = today, .month = month, .budget = evaluate(limits, today.used.counted(), month.used.counted())};
}

constexpr std::string summary_json(ledger const& book, budget const& limits, std::int64_t const now_seconds) {
    return json::write(json::to_value(summary_of(book, limits, now_seconds)));
}

constexpr std::int64_t first_day_of_month(std::int64_t const day) noexcept {
    return day - (civil_from_day(day).day - 1);
}


struct headline {
    std::uint64_t today = 0;
    std::string state{};
    std::string window{};
    std::uint64_t used = 0;
    std::uint64_t limit = 0;
    unsigned percent = 0;
};

struct budget_reading {
    std::string state{};
    std::string window{};
    std::uint64_t used = 0;
    std::uint64_t limit = 0;
    unsigned percent = 0;
};

struct usage_reading {
    totals today{};
    budget_reading budget{};
};

struct stats_reading {
    usage_reading usage{};
};

constexpr headline headline_from(std::string_view const stats_body) {
    auto const parsed = json::parse(stats_body, {.jsonc = false});
    if (!parsed) {
        return {};
    }
    auto const [usage] = lenient::read<stats_reading>(*parsed).value_or(stats_reading{});
    return {.today = usage.today.used.counted(),
            .state = usage.budget.state,
            .window = usage.budget.window,
            .used = usage.budget.used,
            .limit = usage.budget.limit,
            .percent = std::min(usage.budget.percent, percent_cap)};
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
    for (auto const& item : book.groups()) {
        if (item.day < first_day || item.day > last_day) {
            continue;
        }
        auto const same = std::ranges::find_if(rows, [&](group const& seen) { return seen.day == item.day && seen.model == item.model; });
        if (same == rows.end()) {
            rows.push_back({.day = item.day, .model = item.model, .sum = item.sum});
        } else {
            same->sum.used += item.sum.used;
            same->sum.requests = add_sat(same->sum.requests, item.sum.requests);
        }
    }
    std::ranges::sort(rows, [](group const& left, group const& right) { return left.day != right.day ? left.day < right.day : left.model < right.model; });
    return rows;
}

struct price {
    [[= reflect::doc{"US dollars per million input tokens."}]] double input = 0;
    [[= reflect::doc{"US dollars per million output tokens."}]] double output = 0;
    [[= reflect::doc{"US dollars per million cache-read tokens."}]] double cache_read = 0;
    [[= reflect::doc{"US dollars per million cache-write tokens."}]] double cache_creation = 0;
};

constexpr double tokens_per_million = 1000000.0;

constexpr double cost_of(tokens const& used, price const& rate) {
    return (static_cast<double>(used.input) * rate.input + static_cast<double>(used.output) * rate.output + static_cast<double>(used.cache_read) * rate.cache_read +
            static_cast<double>(used.cache_creation) * rate.cache_creation) /
           tokens_per_million;
}

enum class dimension { provider, request_class, agent_type, label };

struct score_row {
    std::string name{};
    [[= json::flatten]] totals sum{};
    std::uint64_t average_ms = 0;
    std::uint64_t max_ms = 0;
    std::uint64_t failures = 0;
    std::uint64_t fallbacks = 0;
    std::uint64_t share_percent = 0;
    std::optional<double> cost{};
};

struct class_total {
    [[= json::key{"class"}]] std::string request_class{};
    [[= json::flatten]] totals sum{};
};

constexpr std::vector<class_total> classes_between(ledger const& book, std::int64_t const first_day, std::int64_t const last_day) {
    std::vector<class_total> rows;
    for (auto const& item : book.classes()) {
        if (item.day < first_day || item.day > last_day) {
            continue;
        }
        auto const same = std::ranges::find(rows, item.request_class, &class_total::request_class);
        if (same == rows.end()) {
            rows.push_back({.request_class = item.request_class, .sum = item.sum});
        } else {
            same->sum.used += item.sum.used;
            same->sum.requests = add_sat(same->sum.requests, item.sum.requests);
        }
    }
    std::ranges::sort(rows, {}, &class_total::request_class);
    return rows;
}

struct spend {
    std::string provider{};
    std::string model{};
    [[= json::flatten]] totals sum{};
    std::optional<double> cost{};
};

constexpr void merge_spend(std::vector<spend>& rows, group const& item) {
    auto const same = std::ranges::find_if(rows, [&](spend const& seen) { return seen.provider == item.provider && seen.model == item.model; });
    if (same == rows.end()) {
        rows.push_back({.provider = item.provider, .model = item.model, .sum = item.sum});
        return;
    }
    same->sum.used += item.sum.used;
    same->sum.requests = add_sat(same->sum.requests, item.sum.requests);
}

constexpr void price_spend(std::vector<spend>& rows, std::map<std::string, price> const& prices) {
    for (auto& row : rows) {
        if (auto const rate = prices.find(row.model); rate != prices.end()) {
            row.cost = cost_of(row.sum.used, rate->second);
        }
    }
}

constexpr std::vector<spend> spend_between(ledger const& book, std::int64_t const first_day, std::int64_t const last_day, std::map<std::string, price> const& prices) {
    std::vector<spend> rows;
    for (auto const& item : book.groups()) {
        if (item.day >= first_day && item.day <= last_day) {
            merge_spend(rows, item);
        }
    }
    price_spend(rows, prices);
    std::ranges::sort(rows, [](spend const& left, spend const& right) { return left.provider != right.provider ? left.provider < right.provider : left.model < right.model; });
    return rows;
}

constexpr std::string const& name_in(detail_group const& item, dimension const which) {
    switch (which) {
        case dimension::provider: return item.provider;
        case dimension::request_class: return item.request_class;
        case dimension::agent_type: return item.agent_type;
        case dimension::label: break;
    }
    return item.label;
}

struct score_total {
    score_row row{};
    std::uint64_t duration_ms = 0;
    bool priced = true;
    double cost = 0;
};

constexpr void merge_score(std::vector<score_total>& rows, detail_group const& item, dimension const which, std::map<std::string, price> const& prices) {
    auto const name = item.label.empty() && which == dimension::label ? std::string{unlabelled} : name_in(item, which);
    auto found = std::ranges::find(rows, name, [](score_total const& each) { return each.row.name; });
    if (found == rows.end()) {
        rows.push_back({.row = {.name = name}});
        found = std::prev(rows.end());
    }
    found->row.sum.used += item.stats.sum.used;
    found->row.sum.requests = add_sat(found->row.sum.requests, item.stats.sum.requests);
    found->duration_ms = add_sat(found->duration_ms, item.stats.duration_ms);
    found->row.max_ms = std::max(found->row.max_ms, item.stats.slowest_ms);
    found->row.failures = add_sat(found->row.failures, item.stats.failures);
    found->row.fallbacks = add_sat(found->row.fallbacks, item.stats.fallbacks);
    if (auto const rate = prices.find(item.model); rate != prices.end()) {
        found->cost += cost_of(item.stats.sum.used, rate->second);
    } else {
        found->priced = false;
    }
}

constexpr score_row finished_row(score_total& each, std::uint64_t const everything) {
    each.row.average_ms = each.row.sum.requests == 0 ? 0 : each.duration_ms / each.row.sum.requests;
    each.row.share_percent = everything == 0 ? 0 : each.row.sum.requests * 100 / everything;
    each.row.cost = each.priced ? std::optional<double>{each.cost} : std::nullopt;
    return std::move(each.row);
}

constexpr std::vector<score_row> scorecard(ledger const& book, dimension const which, std::int64_t const first_day, std::int64_t const last_day,
                                           std::map<std::string, price> const& prices) {
    std::vector<score_total> totals_by_name;
    std::uint64_t everything = 0;
    for (auto const& item : which == dimension::label ? book.label_details() : book.details()) {
        if (item.day >= first_day && item.day <= last_day) {
            merge_score(totals_by_name, item, which, prices);
            everything = add_sat(everything, item.stats.sum.requests);
        }
    }
    std::vector<score_row> rows;
    for (auto& each : totals_by_name) {
        rows.push_back(finished_row(each, everything));
    }
    std::ranges::sort(rows, {}, &score_row::name);
    return rows;
}

constexpr std::size_t date_width = 12;
constexpr std::size_t model_width = 22;
constexpr std::size_t request_width = 9;
constexpr std::size_t number_width = 12;

constexpr std::string token_headings() {
    std::string out;
    template for (constexpr auto field : members_of<tokens>) {
        text::append_all(out, pad_left(reflect::copy_of(heading_of(field)), number_width));
    }
    return out;
}

constexpr std::string token_cells(tokens const& values) {
    std::string out;
    template for (constexpr auto field : members_of<tokens>) {
        text::append_all(out, pad_left(compact(values.[:field:]), number_width));
    }
    return out;
}

constexpr std::string report(ledger const& book, std::int64_t const first_day, std::int64_t const last_day) {
    std::string out;
    text::append_all(out, pad_right("date", date_width), pad_right("model", model_width), pad_left("requests", request_width), token_headings(), "\n");
    totals all;
    for (auto const& item : rows_between(book, first_day, last_day)) {
        text::append_all(out, pad_right(iso_date(item.day), date_width), pad_right(item.model, model_width), pad_left(text::decimal(item.sum.requests), request_width),
                         token_cells(item.sum.used), "\n");
        all.used += item.sum.used;
        all.requests = add_sat(all.requests, item.sum.requests);
    }
    text::append_all(out, pad_right("total", date_width + model_width), pad_left(text::decimal(all.requests), request_width), token_cells(all.used), "\n");
    return out;
}

struct report_row {
    std::string date{};
    std::string model{};
    [[= json::flatten]] totals sum{};
};

struct report_body {
    std::int64_t days = 0;
    std::vector<report_row> rows{};
};

constexpr std::string report_json(ledger const& book, std::int64_t const first_day, std::int64_t const last_day) {
    auto rows = rows_between(book, first_day, last_day) |
                std::views::transform([](group const& item) { return report_row{.date = iso_date(item.day), .model = item.model, .sum = item.sum}; }) |
                std::ranges::to<std::vector>();
    return json::write(json::to_value(report_body{.days = last_day - first_day + 1, .rows = std::move(rows)}), 2);
}

static_assert(first_day_of_month(day_of(1760659200)) == day_of(1759276800) && first_day_of_month(0) == 0);
static_assert(record{} == record{} && totals{} == totals{});
static_assert(label("claude-x") == "claude-x" && label("a\x1b[31mb") == "a?[31mb" && label("sp ace\n") == "sp?ace?" && label(std::string(100, 'a')).size() == longest_label);
static_assert(session_of(R"({"model":"m","metadata":{"user_id":"user_ab_account_cd_session_5f3a-90"}})") == "5f3a-90");
static_assert(session_of(R"({"metadata":{"user_id":"{\"device_id\":\"d\",\"session_id\":\"s-1\"}"}})") == "s-1");
static_assert(session_of(R"({"metadata":{"user_id":"no marker"}})").empty() && session_of("[]").empty() && session_of(R"({"metadata":3})").empty());
static_assert(session_of(R"({"metadata":{"user_id":"x_session_bad id!"}})").empty());
static_assert(role_of("claude-Sonnet-5-5") == "sonnet" && role_of("claude-haiku-5-5") == "haiku" && role_of("gpt") == "other");
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
    return rows_between(book, 0, 1).size() == 2 && rows_between(book, 5, 6).empty() && text.contains("1970-01-02") && text.contains("1k") && text.starts_with("date");
}());

}  // namespace llmgate::usage

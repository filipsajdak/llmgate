export module llmgate.statusline;

import std;
import llmgate.text;

export namespace llmgate::statusline {

using text::decimal;

struct snapshot {
    std::string model{};
    std::string directory{};
    std::string branch{};
    int context_percent = 0;
    bool gateway_up = false;
    std::uint64_t requests = 0;
    std::uint64_t repairs = 0;
    bool usage_known = false;
    std::uint64_t today_tokens = 0;
    std::uint64_t budget_limit = 0;
    int budget_percent = 0;
    std::string budget_window{};
    std::string budget_state{};
};

constexpr std::size_t bar_width = 10;
constexpr std::string_view filled_cell = "#";
constexpr std::string_view empty_cell = "-";
constexpr int warn_percent = 80;
constexpr int full_percent = 100;

constexpr std::string bar(int const percent) {
    auto const clamped = std::clamp(percent, 0, full_percent);
    auto const filled = static_cast<std::size_t>(clamped) * bar_width / static_cast<std::size_t>(full_percent);
    std::string out;
    for (std::size_t cell = 0; cell < bar_width; ++cell) {
        out += cell < filled ? filled_cell : empty_cell;
    }
    return out;
}

constexpr std::string base_name(std::string_view const directory) {
    auto trimmed = directory;
    while (trimmed.size() > 1 && trimmed.ends_with('/')) {
        trimmed.remove_suffix(1);
    }
    auto const slash = trimmed.rfind('/');
    return std::string{slash == std::string_view::npos || trimmed.size() == 1 ? trimmed : trimmed.substr(slash + 1)};
}

constexpr std::string branch_from_head(std::string_view const head) {
    constexpr std::string_view prefix = "ref: refs/heads/";
    auto line = head.substr(0, head.find('\n'));
    while (line.ends_with('\r')) {
        line.remove_suffix(1);
    }
    if (line.starts_with(prefix)) {
        return std::string{line.substr(prefix.size())};
    }
    return line.size() >= 7 ? std::string{line.substr(0, 7)} : std::string{};
}

constexpr std::string_view level_of(int const percent) {
    return percent >= full_percent ? "full" : percent >= warn_percent ? "high" : "ok";
}

constexpr std::string usage_line(snapshot const& now) {
    std::string line = "usage today " + text::compact(now.today_tokens);
    if (now.budget_limit == 0) {
        return line + " tokens";
    }
    line += "/" + text::compact(now.budget_limit) + " " + now.budget_window;
    line += " [" + bar(now.budget_percent) + "] " + decimal(static_cast<std::uint64_t>(std::clamp(now.budget_percent, 0, 999))) + "%";
    if (now.budget_state == "exhausted") {
        line += " EXHAUSTED";
    } else if (now.budget_state == "warning") {
        line += " high";
    }
    return line;
}

constexpr std::string render(snapshot const& now) {
    std::string first = "[llmgate] ";
    first += now.model.empty() ? "model?" : now.model;
    first += " | " + base_name(now.directory);
    if (!now.branch.empty()) {
        first += " | " + now.branch;
    }
    std::string second = "ctx [" + bar(now.context_percent) + "] " + decimal(static_cast<std::uint64_t>(std::clamp(now.context_percent, 0, 999))) + "%";
    if (now.context_percent >= warn_percent) {
        second += std::string{" "} + std::string{level_of(now.context_percent)};
    }
    second += now.gateway_up ? " | gateway ok" : " | gateway DOWN";
    if (now.gateway_up) {
        second += " | " + decimal(now.requests) + " req";
        if (now.repairs > 0) {
            second += " | " + decimal(now.repairs) + " repaired";
        }
    }
    auto text = first + "\n" + second + "\n";
    if (now.usage_known) {
        text += usage_line(now) + "\n";
    }
    return text;
}

static_assert(bar(0) == "----------" && bar(50) == "#####-----" && bar(100) == "##########" && bar(250) == "##########" && bar(-4) == "----------");
static_assert(base_name("/a/b/proj/") == "proj" && base_name("/") == "/" && base_name("proj") == "proj" && base_name("").empty());
static_assert(branch_from_head("ref: refs/heads/feat/x\n") == "feat/x" && branch_from_head("0123456789abcdef\n") == "0123456" && branch_from_head("x").empty());
static_assert(render({.model = "Opus", .directory = "/w/p", .gateway_up = true, .usage_known = true, .today_tokens = 412345}) ==
              "[llmgate] Opus | p\nctx [----------] 0% | gateway ok | 0 req\nusage today 412k tokens\n");
static_assert(usage_line({.today_tokens = 412345, .budget_limit = 1000000, .budget_percent = 41, .budget_window = "daily", .budget_state = "ok"}) ==
              "usage today 412k/1.0M daily [####------] 41%");
static_assert(usage_line({.today_tokens = 1200000, .budget_limit = 1000000, .budget_percent = 120, .budget_window = "daily", .budget_state = "exhausted"}) ==
              "usage today 1.2M/1.0M daily [##########] 120% EXHAUSTED");
static_assert(render({.model = "Opus", .directory = "/w/p", .branch = "main", .context_percent = 42, .gateway_up = true, .requests = 3, .repairs = 1}) ==
              "[llmgate] Opus | p | main\nctx [####------] 42% | gateway ok | 3 req | 1 repaired\n");

}  // namespace llmgate::statusline

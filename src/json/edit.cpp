module llmgate.jsonedit;

import std;

namespace llmgate::jsonedit {

namespace {

constexpr std::size_t npos = std::string_view::npos;
constexpr std::size_t indent_step = 2;
constexpr std::size_t diff_context = 2;
constexpr std::size_t longest_diff = 60;

struct located {
    std::size_t key_begin = 0;
    std::size_t value_begin = 0;
    std::size_t value_end = 0;
};

struct scan {
    std::string_view text;

    std::size_t skip_space(std::size_t at) const {
        while (at < text.size()) {
            if (std::isspace(static_cast<unsigned char>(text[at]))) {
                ++at;
            } else if (text.substr(at, 2) == "//") {
                at = std::min(text.find('\n', at), text.size());
            } else if (text.substr(at, 2) == "/*") {
                auto const close = text.find("*/", at + 2);
                at = close == npos ? text.size() : close + 2;
            } else {
                break;
            }
        }
        return at;
    }

    std::size_t string_end(std::size_t at) const {
        for (++at; at < text.size(); ++at) {
            if (text[at] == '\\') {
                ++at;
            } else if (text[at] == '"') {
                return at + 1;
            }
        }
        return npos;
    }

    std::size_t scalar_end(std::size_t at) const {
        while (at < text.size() && !std::string_view{",}] \t\r\n/"}.contains(text[at])) {
            ++at;
        }
        return at;
    }

    static constexpr int depth_change(char const c) { return (c == '{' || c == '[') ? 1 : (c == '}' || c == ']') ? -1 : 0; }

    std::size_t container_end(std::size_t at) const {
        std::size_t depth = 0;
        while ((at = skip_space(at)) < text.size()) {
            if (text[at] == '"') {
                at = string_end(at);
                if (at == npos) {
                    return npos;
                }
                continue;
            }
            depth += static_cast<std::size_t>(depth_change(text[at]));
            ++at;
            if (depth == 0) {
                return at;
            }
        }
        return npos;
    }

    std::size_t value_end(std::size_t const at) const {
        if (at >= text.size()) {
            return npos;
        }
        if (text[at] == '"') {
            return string_end(at);
        }
        return text[at] == '{' || text[at] == '[' ? container_end(at) : scalar_end(at);
    }
};

struct members {
    std::vector<located> items;
    std::vector<std::string> keys;
    std::size_t close = npos;
    std::size_t last_end = npos;
};

struct one_member {
    located place;
    std::string key;
};

std::expected<one_member, std::string> read_one(scan const& in, std::size_t const at) {
    if (in.text[at] != '"') {
        return std::unexpected{"the file is not an object of quoted keys where an edit is needed"};
    }
    auto const key_end = in.string_end(at);
    if (key_end == npos) {
        return std::unexpected{"an unterminated string in the file"};
    }
    auto const colon = in.skip_space(key_end);
    if (colon >= in.text.size() || in.text[colon] != ':') {
        return std::unexpected{"a key without a colon in the file"};
    }
    auto const begin = in.skip_space(colon + 1);
    auto const end = in.value_end(begin);
    if (end == npos) {
        return std::unexpected{"an unterminated value in the file"};
    }
    return one_member{.place = {.key_begin = at, .value_begin = begin, .value_end = end}, .key = std::string{in.text.substr(at + 1, key_end - at - 2)}};
}

std::size_t after_value(scan const& in, std::size_t const end) {
    auto const at = in.skip_space(end);
    return at < in.text.size() && in.text[at] == ',' ? in.skip_space(at + 1) : at;
}

std::expected<members, std::string> read_members(scan const& in, std::size_t const open) {
    members out;
    std::size_t at = in.skip_space(open + 1);
    while (at < in.text.size() && in.text[at] != '}') {
        auto one = read_one(in, at);
        if (!one) {
            return std::unexpected{std::move(one.error())};
        }
        out.items.push_back(one->place);
        out.keys.push_back(std::move(one->key));
        out.last_end = one->place.value_end;
        at = after_value(in, one->place.value_end);
    }
    if (at >= in.text.size()) {
        return std::unexpected{"an object without a closing brace in the file"};
    }
    out.close = at;
    return out;
}

std::string escaped(std::string_view const key) {
    std::string out{"\""};
    for (auto const c : key) {
        out += (c == '"' || c == '\\') ? std::string{'\\', c} : std::string{c};
    }
    return out + "\"";
}

std::string line_indent(std::string_view const text, std::size_t const at) {
    auto const start = text.rfind('\n', at == 0 ? 0 : at - 1);
    auto const from = start == npos ? 0 : start + 1;
    auto const stop = text.find_first_not_of(" \t", from);
    return std::string{text.substr(from, std::min(stop == npos ? text.size() : stop, at) - from)};
}

std::string member_text(std::span<std::string const> const keys, std::string_view const value, std::string const& indent) {
    if (keys.size() == 1) {
        return escaped(keys.front()) + ": " + std::string{value};
    }
    auto const inner = indent + std::string(indent_step, ' ');
    auto const rest = member_text(keys.subspan(1), value, inner);
    return escaped(keys.front()) + ": {\n" + inner + rest + "\n" + indent + "}";
}

std::string with_member(std::string_view const text, members const& found, std::span<std::string const> const keys, std::string_view const value) {
    auto const closing_indent = line_indent(text, found.close);
    auto const only_space_before = text.find_last_not_of(" \t", found.close - 1) == text.rfind('\n', found.close - 1);
    auto const child = closing_indent + std::string(indent_step, ' ');
    auto const added = member_text(keys, value, child);
    std::string out;
    if (found.last_end == npos) {
        out.append(text.substr(0, found.close));
        out.erase(out.find_last_not_of(" \t\r\n") + 1);
        std::format_to(std::back_inserter(out), "\n{}{}\n{}", child, added, closing_indent);
        out.append(text.substr(found.close));
        return out;
    }
    auto const after_last = text.substr(found.last_end, found.close - found.last_end);
    auto const comma = after_last.find(',') != npos;
    out.append(text.substr(0, found.last_end));
    if (!comma) {
        out += ',';
    }
    if (!only_space_before) {
        out.append(text.substr(found.last_end, found.close - found.last_end));
        out += (comma ? " " : " ") + added;
        out.append(text.substr(found.close));
        return out;
    }
    auto const line = text.rfind('\n', found.close - 1);
    out.append(text.substr(found.last_end, line - found.last_end + 1));
    std::format_to(std::back_inserter(out), "{}{}\n{}", child, added, closing_indent);
    out.append(text.substr(found.close));
    return out;
}

std::size_t drop_comma(std::string& out, std::string_view const text, scan const& in, located const& at) {
    auto tail = in.skip_space(at.value_end);
    if (tail < text.size() && text[tail] == ',') {
        return tail + 1;
    }
    if (auto const before = out.find_last_not_of(" \t\r\n"); before != npos && out[before] == ',') {
        out.erase(before, 1);
    }
    return tail;
}

std::size_t drop_blank_line(std::string& out, std::string_view const text, std::size_t const tail) {
    auto const line_start = out.rfind('\n');
    if (line_start == npos || out.find_first_not_of(" \t", line_start + 1) != npos) {
        return tail;
    }
    out.erase(line_start + 1);
    auto const newline = text.find('\n', tail);
    auto const rest_blank = newline != npos && text.substr(tail, newline - tail).find_first_not_of(" \t\r") == npos;
    return rest_blank ? newline + 1 : tail;
}

std::string without_member(std::string_view const text, scan const& in, located const& at) {
    std::string out{text.substr(0, at.key_begin)};
    auto const tail = drop_blank_line(out, text, drop_comma(out, text, in, at));
    out.append(text.substr(tail));
    return out;
}

std::string with_value(std::string_view const text, located const& at, std::string_view const value) {
    std::string out{text.substr(0, at.value_begin)};
    out.append(value);
    out.append(text.substr(at.value_end));
    return out;
}

using result = std::expected<std::string, std::string>;

struct visit {
    std::optional<result> done{};
    std::size_t open = 0;
};

struct target {
    std::string_view text;
    scan const& in;
    std::span<std::string const> path;
    std::string_view value;
    bool removing;
};

visit visit_level(target const& goal, members const& found, std::size_t const level) {
    auto const index = static_cast<std::size_t>(std::ranges::find(found.keys, goal.path[level]) - found.keys.begin());
    if (index == found.keys.size()) {
        return {.done = goal.removing ? result{std::unexpected{"nothing named " + goal.path[level] + " to remove"}}
                                      : result{with_member(goal.text, found, goal.path.subspan(level), goal.value)}};
    }
    auto const& at = found.items[index];
    if (level + 1 == goal.path.size()) {
        return {.done = goal.removing ? without_member(goal.text, goal.in, at) : with_value(goal.text, at, goal.value)};
    }
    if (goal.text[at.value_begin] != '{') {
        return {.done = result{std::unexpected{goal.path[level] + " is not an object, so " + goal.path[level + 1] + " cannot be set inside it"}}};
    }
    return {.open = at.value_begin};
}

result edit(std::string_view const text, std::span<std::string const> const path, std::string_view const value, bool const removing) {
    scan const in{text};
    auto open = in.skip_space(0);
    if (path.empty()) {
        return std::unexpected{"an empty path"};
    }
    if (open >= text.size() || text[open] != '{') {
        return std::unexpected{"the file does not hold an object"};
    }
    target const goal{.text = text, .in = in, .path = path, .value = value, .removing = removing};
    for (std::size_t level = 0; level < path.size(); ++level) {
        auto const found = read_members(in, open);
        if (!found) {
            return std::unexpected{found.error()};
        }
        auto const next = visit_level(goal, *found, level);
        if (next.done) {
            return *next.done;
        }
        open = next.open;
    }
    return std::unexpected{"unreachable"};
}

std::vector<std::string_view> lines_of(std::string_view const text) {
    std::vector<std::string_view> out;
    for (auto const line : std::views::split(text, '\n')) {
        out.emplace_back(line.begin(), line.end());
    }
    return out;
}

std::size_t common_head(std::span<std::string_view const> const first, std::span<std::string_view const> const second) {
    return static_cast<std::size_t>(std::ranges::mismatch(first, second).in1 - first.begin());
}

std::size_t common_tail(std::span<std::string_view const> const first, std::span<std::string_view const> const second) {
    auto const ends = std::ranges::mismatch(first | std::views::reverse, second | std::views::reverse);
    return static_cast<std::size_t>(ends.in1 - (first | std::views::reverse).begin());
}

void append_marked(std::string& out, std::span<std::string_view const> const lines, char const mark) {
    for (auto const line : lines | std::views::take(longest_diff)) {
        std::format_to(std::back_inserter(out), "{} {}\n", mark, line);
    }
    if (lines.size() > longest_diff) {
        std::format_to(std::back_inserter(out), "{} ...\n", mark);
    }
}

void append_context(std::string& out, std::span<std::string_view const> const lines) {
    for (auto const line : lines) {
        std::format_to(std::back_inserter(out), "  {}\n", line);
    }
}

}  // namespace

std::expected<std::string, std::string> set(std::string_view const text, std::span<std::string const> const path, std::string_view const value) {
    return edit(text, path, value, false);
}

std::expected<std::string, std::string> remove(std::string_view const text, std::span<std::string const> const path) {
    return edit(text, path, {}, true);
}

std::string diff(std::string_view const before, std::string_view const after) {
    auto const old_lines = lines_of(before);
    auto const new_lines = lines_of(after);
    auto const head = common_head(old_lines, new_lines);
    if (head == old_lines.size() && head == new_lines.size()) {
        return "no change\n";
    }
    auto const tail = std::min({common_tail(old_lines, new_lines), old_lines.size() - head, new_lines.size() - head});
    std::string out;
    append_context(out, std::span{old_lines}.subspan(head > diff_context ? head - diff_context : 0, std::min(head, diff_context)));
    append_marked(out, std::span{old_lines}.subspan(head, old_lines.size() - head - tail), '-');
    append_marked(out, std::span{new_lines}.subspan(head, new_lines.size() - head - tail), '+');
    auto const after_old = old_lines.size() - tail;
    append_context(out, std::span{old_lines}.subspan(after_old, std::min(diff_context, tail)));
    return out;
}

}  // namespace llmgate::jsonedit

export module llmgate.org_pack;

import std;
import llmgate.json;
import llmgate.text;

export namespace llmgate::org {

constexpr int pack_schema = 1;
constexpr std::string_view pack_file = "llmgate-org.jsonc";
constexpr std::string_view allowed_key = "providers_allowed";

struct marketplace_source {
    std::string source{};
    std::string repo{};
    std::string url{};
    std::string path{};
};

struct claude_policy {
    std::vector<std::string> deny{};
    std::vector<std::string> announcements{};
    std::map<std::string, marketplace_source> marketplaces{};
    std::map<std::string, bool> plugins{};
};

struct pack {
    int schema = 0;
    std::string name{};
    std::string version{};
    std::vector<std::string> locked{};
    std::vector<std::string> providers_allowed{};
    claude_policy claude{};
    std::string recipe{};
};

struct loaded {
    pack described{};
    json::value layer{json::object{}};
};

struct applied {
    std::vector<std::string> deny{};
    std::vector<std::string> announcements{};
    std::vector<std::string> marketplaces{};
    std::vector<std::string> plugins{};
};

struct policy_result {
    json::value settings{};
    applied added{};
};

namespace detail {

constexpr char version_dot = '.';

constexpr bool version_ok(std::string_view const text) {
    std::size_t parts = 0;
    for (auto const part : std::views::split(text, version_dot)) {
        std::string_view const piece{part.begin(), part.end()};
        std::size_t number = 0;
        auto const parsed = std::from_chars(piece.data(), piece.data() + piece.size(), number);
        if (piece.empty() || parsed.ec != std::errc{} || parsed.ptr != piece.data() + piece.size()) {
            return false;
        }
        ++parts;
    }
    return parts == 3;
}

constexpr json::object* object_of(json::value& source) {
    return std::get_if<json::object>(&source.data);
}

constexpr json::value* find_member(json::value& source, std::string_view const name) {
    auto* members = object_of(source);
    if (members == nullptr) {
        return nullptr;
    }
    auto const found = std::ranges::find(*members, name, &json::object::value_type::first);
    return found == members->end() ? nullptr : &found->second;
}

constexpr json::value& member_or_add(json::value& source, std::string_view const name) {
    if (auto* found = find_member(source, name)) {
        return *found;
    }
    object_of(source)->emplace_back(std::string{name}, json::value{json::object{}});
    return object_of(source)->back().second;
}

constexpr json::value& ensure_object(json::value& source) {
    if (object_of(source) == nullptr) {
        source = json::value{json::object{}};
    }
    return source;
}

constexpr json::value& ensure_array(json::value& source) {
    if (std::get_if<json::array>(&source.data) == nullptr) {
        source = json::value{json::array{}};
    }
    return source;
}

constexpr bool lists_text(json::value const& list, std::string_view const text) {
    auto const* items = std::get_if<json::array>(&list.data);
    return items != nullptr && std::ranges::any_of(*items, [text](json::value const& each) {
        auto const* held = std::get_if<std::string>(&each.data);
        return held != nullptr && *held == text;
    });
}

constexpr void remove_text(json::value& list, std::string_view const text) {
    if (auto* items = std::get_if<json::array>(&list.data)) {
        std::erase_if(*items, [text](json::value const& each) {
            auto const* held = std::get_if<std::string>(&each.data);
            return held != nullptr && *held == text;
        });
    }
}

constexpr std::vector<std::string_view> segments_of(std::string_view const path) {
    std::vector<std::string_view> out;
    for (auto const part : std::views::split(path, '.')) {
        out.emplace_back(part.begin(), part.end());
    }
    return out;
}

constexpr json::value const* at_path(json::value const& root, std::string_view const path) {
    json::value const* current = &root;
    for (auto const piece : segments_of(path)) {
        auto const* members = std::get_if<json::object>(&current->data);
        if (members == nullptr) {
            return nullptr;
        }
        auto const found = std::ranges::find(*members, piece, &json::object::value_type::first);
        if (found == members->end()) {
            return nullptr;
        }
        current = &found->second;
    }
    return current;
}

constexpr void set_path(json::value& root, std::string_view const path, json::value const& wanted) {
    json::value* current = &ensure_object(root);
    for (auto const piece : segments_of(path)) {
        current = &member_or_add(ensure_object(*current), piece);
    }
    *current = wanted;
}

constexpr json::value merged(json::value base, json::value const& over) {
    auto* base_members = std::get_if<json::object>(&base.data);
    auto const* over_members = std::get_if<json::object>(&over.data);
    if (base_members == nullptr || over_members == nullptr) {
        return over;
    }
    for (auto const& [name, each] : *over_members) {
        if (auto* present = find_member(base, name)) {
            *present = merged(*present, each);
        } else {
            base_members->emplace_back(name, each);
        }
    }
    return base;
}

constexpr json::value text_array(std::vector<std::string> const& items) {
    json::array out;
    for (auto const& item : items) {
        out.push_back(json::make_string(item));
    }
    return json::value{std::move(out)};
}

constexpr bool owns(std::vector<std::string> const& names, std::string_view const name) {
    return std::ranges::contains(names, name);
}

}  // namespace detail

constexpr bool printable_text(std::string_view const text) {
    for (std::size_t at = 0; at < text.size(); ++at) {
        auto const byte = static_cast<unsigned char>(text[at]);
        bool const c1 = byte == 0xC2 && at + 1 < text.size() && static_cast<unsigned char>(text[at + 1]) >= 0x80 && static_cast<unsigned char>(text[at + 1]) <= 0x9F;
        if (byte < 0x20 || byte == 0x7F || c1) {
            return false;
        }
    }
    return true;
}

constexpr bool tree_printable(json::value const& node) {
    if (auto const* text = std::get_if<std::string>(&node.data)) {
        return printable_text(*text);
    }
    if (auto const* list = std::get_if<json::array>(&node.data)) {
        return std::ranges::all_of(*list, [](json::value const& each) { return tree_printable(each); });
    }
    if (auto const* members = std::get_if<json::object>(&node.data)) {
        return std::ranges::all_of(*members, [](auto const& entry) { return printable_text(entry.first) && tree_printable(entry.second); });
    }
    return true;
}

constexpr std::vector<std::string_view> texts_of(pack const& described) {
    std::vector<std::string_view> out{described.name, described.version, described.recipe};
    for (auto const* list : {&described.locked, &described.providers_allowed, &described.claude.deny, &described.claude.announcements}) {
        out.append_range(*list);
    }
    for (auto const& [name, each] : described.claude.marketplaces) {
        out.insert(out.end(), {std::string_view{name}, each.source, each.repo, each.url, each.path});
    }
    for (auto const& entry : described.claude.plugins) {
        out.emplace_back(entry.first);
    }
    return out;
}

constexpr std::string pack_problem(pack const& described) {
    if (described.schema != pack_schema) {
        return std::format("schema must be {}", pack_schema);
    }
    if (described.name.empty() || !detail::version_ok(described.version)) {
        return "name is required and version must be MAJOR.MINOR.PATCH";
    }
    return std::ranges::all_of(texts_of(described), printable_text) ? std::string{} : std::string{"a text in the pack holds a control character"};
}

constexpr std::expected<loaded, std::string> read_pack(std::string_view const text) {
    auto parsed = json::parse(text, {.jsonc = true});
    if (!parsed) {
        return std::unexpected{std::format("{}: {} (at byte {})", pack_file, parsed.error().message, parsed.error().offset)};
    }
    loaded out;
    if (auto* config = detail::find_member(*parsed, "config")) {
        out.layer = std::move(*config);
        std::erase_if(*detail::object_of(*parsed), [](auto const& entry) { return entry.first == "config"; });
    }
    auto described = json::read<pack>(*parsed);
    if (!described) {
        return std::unexpected{std::format("{}: {} ({})", pack_file, described.error().message, described.error().path)};
    }
    if (auto const problem = pack_problem(*described); !problem.empty()) {
        return std::unexpected{std::format("{}: {}", pack_file, problem)};
    }
    if (!tree_printable(out.layer)) {
        return std::unexpected{std::format("{}: a name or text in config holds a control character", pack_file)};
    }
    out.described = std::move(*described);
    return out;
}

constexpr json::value effective_config(loaded const& installed, json::value const& user) {
    auto result = detail::merged(installed.layer, user);
    for (auto const& path : installed.described.locked) {
        if (auto const* wanted = detail::at_path(installed.layer, path)) {
            detail::set_path(result, path, *wanted);
        }
    }
    if (!installed.described.providers_allowed.empty()) {
        detail::set_path(result, allowed_key, detail::text_array(installed.described.providers_allowed));
    }
    return result;
}

constexpr std::vector<std::string> overridden_keys(loaded const& installed, json::value const& user) {
    std::vector<std::string> out;
    for (auto const& path : installed.described.locked) {
        auto const* mine = detail::at_path(user, path);
        auto const* theirs = detail::at_path(installed.layer, path);
        if (mine != nullptr && theirs != nullptr && json::write(*mine) != json::write(*theirs)) {
            out.push_back(path);
        }
    }
    return out;
}

constexpr json::value source_value(marketplace_source const& each) {
    json::object members{{"source", json::make_string(each.source)}};
    for (auto const& [key, value] : {std::pair<std::string_view, std::string const&>{"repo", each.repo}, {"url", each.url}, {"path", each.path}}) {
        if (!value.empty()) {
            members.emplace_back(std::string{key}, json::make_string(value));
        }
    }
    return json::make_object({{"source", json::value{std::move(members)}}});
}

namespace detail {

constexpr void add_texts(json::value& list, std::vector<std::string> const& wanted, std::vector<std::string> const& before, std::vector<std::string>& added) {
    ensure_array(list);
    for (auto const& line : wanted) {
        if (!lists_text(list, line)) {
            std::get<json::array>(list.data).push_back(json::make_string(line));
            added.push_back(line);
        } else if (owns(before, line)) {
            added.push_back(line);
        }
    }
}

template <class Entries, class Make>
    requires std::invocable<Make&, typename Entries::mapped_type const&>
constexpr void add_entries(json::value& table, Entries const& wanted, std::vector<std::string> const& before, std::vector<std::string>& added, Make&& make) {
    ensure_object(table);
    for (auto const& [name, each] : wanted) {
        if (find_member(table, name) == nullptr || owns(before, name)) {
            set_path(table, name, make(each));
            added.push_back(name);
        }
    }
}

}  // namespace detail

constexpr policy_result apply_policy(json::value settings, claude_policy const& wanted, applied const& before) {
    policy_result out;
    detail::ensure_object(settings);
    detail::add_texts(detail::member_or_add(detail::ensure_object(detail::member_or_add(settings, "permissions")), "deny"), wanted.deny, before.deny, out.added.deny);
    detail::add_texts(detail::member_or_add(settings, "companyAnnouncements"), wanted.announcements, before.announcements, out.added.announcements);
    detail::add_entries(detail::member_or_add(settings, "extraKnownMarketplaces"), wanted.marketplaces, before.marketplaces, out.added.marketplaces, source_value);
    detail::add_entries(detail::member_or_add(settings, "enabledPlugins"), wanted.plugins, before.plugins, out.added.plugins, [](bool const on) { return json::value{on}; });
    out.settings = std::move(settings);
    return out;
}

constexpr json::value remove_policy(json::value settings, applied const& before) {
    detail::ensure_object(settings);
    if (auto* permissions = detail::find_member(settings, "permissions")) {
        if (auto* deny = detail::find_member(*permissions, "deny")) {
            std::ranges::for_each(before.deny, [deny](std::string const& rule) { detail::remove_text(*deny, rule); });
        }
    }
    if (auto* notes = detail::find_member(settings, "companyAnnouncements")) {
        std::ranges::for_each(before.announcements, [notes](std::string const& line) { detail::remove_text(*notes, line); });
    }
    for (auto const& [key, names] : {std::pair<std::string_view, std::vector<std::string> const&>{"extraKnownMarketplaces", before.marketplaces}, {"enabledPlugins", before.plugins}}) {
        if (auto* table = detail::find_member(settings, key); table != nullptr && detail::object_of(*table) != nullptr) {
            std::erase_if(*detail::object_of(*table), [&names](auto const& entry) { return detail::owns(names, entry.first); });
        }
    }
    return settings;
}

namespace detail {

constexpr void config_lines(json::value const& layer, std::vector<std::string>& lines) {
    auto const* members = std::get_if<json::object>(&layer.data);
    for (auto const& [key, value] : members == nullptr ? json::object{} : *members) {
        auto const* inner = std::get_if<json::object>(&value.data);
        if (key != "providers" || inner == nullptr) {
            lines.push_back(std::format("config {}: {}", key, json::write(value)));
            continue;
        }
        for (auto const& [name, each] : *inner) {
            lines.push_back(std::format("provider {}: {}", name, json::write(each)));
        }
    }
}

constexpr void each_line(std::vector<std::string>& lines, std::string_view const label, std::vector<std::string> const& items) {
    for (auto const& item : items) {
        lines.push_back(std::format("{}: {}", label, item));
    }
}

}  // namespace detail

constexpr std::vector<std::string> summary_of(loaded const& installed) {
    std::vector<std::string> lines;
    auto const& described = installed.described;
    detail::config_lines(installed.layer, lines);
    detail::each_line(lines, "allowed provider", described.providers_allowed);
    detail::each_line(lines, "locked", described.locked);
    detail::each_line(lines, "deny rule", described.claude.deny);
    detail::each_line(lines, "announcement", described.claude.announcements);
    for (auto const& [name, each] : described.claude.marketplaces) {
        lines.push_back(std::format("marketplace {}: {}", name, json::write(source_value(each))));
    }
    for (auto const& [name, on] : described.claude.plugins) {
        lines.push_back(std::format("plugin {}: {}", name, on ? "enabled" : "disabled"));
    }
    detail::each_line(lines, "recipe", described.recipe.empty() ? std::vector<std::string>{} : std::vector<std::string>{described.recipe});
    return lines;
}

constexpr std::vector<std::string> difference(std::vector<std::string> const& before, std::vector<std::string> const& after) {
    std::vector<std::string> out;
    for (auto const& line : before) {
        if (!std::ranges::contains(after, line)) {
            out.push_back("- " + line);
        }
    }
    for (auto const& line : after) {
        if (!std::ranges::contains(before, line)) {
            out.push_back("+ " + line);
        }
    }
    return out;
}

}  // namespace llmgate::org

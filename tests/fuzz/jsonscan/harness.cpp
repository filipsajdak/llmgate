import llmgate.fuzz_driver;
import llmgate.jsonscan;
import std;

namespace {

using namespace llmgate::jsonscan;

constexpr std::string_view marker = "REPLACED";

void check(std::span<std::byte const> input) {
    std::string_view const text{reinterpret_cast<char const*>(input.data()), input.size()};
    auto const members = top_level_members(text);
    if (!members) {
        return;
    }
    for (auto const& item : *members) {
        if (item.value_begin > item.value_end || item.value_end > text.size()) {
            std::abort();
        }
    }
    auto const replaced = replace_string_member(text, "model", marker);
    if (!replaced) {
        return;
    }
    auto const again = top_level_members(*replaced);
    if (!again || again->size() != members->size()) {
        std::abort();
    }
    if (members_named(*again, "model").size() != members_named(*members, "model").size()) {
        std::abort();
    }
    for (auto const& item : members_named(*again, "model")) {
        if (std::string_view{*replaced}.substr(item.value_begin, item.value_end - item.value_begin) != "\"REPLACED\"") {
            std::abort();
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    return llmgate::fuzz::run(argc, argv, check);
}

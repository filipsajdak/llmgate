module llmgate.reflect;

import std;

namespace llmgate::reflect::detail {

std::error_category const& interned_category(char const* const key, std::error_category const* (*make)()) {
    static std::mutex guard;
    static std::map<std::string, std::error_category const*, std::less<>> known;
    std::lock_guard const lock{guard};
    auto found = known.find(std::string_view{key});
    if (found == known.end()) {
        found = known.emplace(std::string{key}, make()).first;
    }
    return *found->second;
}

}  // namespace llmgate::reflect::detail

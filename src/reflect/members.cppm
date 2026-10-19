module;

#include <meta>

export module llmgate.members;

import std;

export namespace llmgate::reflect {

template <class Annotation>
consteval std::optional<Annotation> annotation_of(std::meta::info const member) {
    auto const found = std::meta::annotations_of_with_type(member, ^^Annotation);
    if (found.empty()) {
        return std::nullopt;
    }
    return std::meta::extract<Annotation>(found.front());
}

template <class Annotation>
consteval bool has_annotation(std::meta::info const member) {
    return !std::meta::annotations_of_with_type(member, ^^Annotation).empty();
}

consteval std::optional<std::meta::info> member_named(std::meta::info const owner, std::string_view const name) {
    for (auto const member : std::meta::nonstatic_data_members_of(owner, std::meta::access_context::unchecked())) {
        if (std::meta::identifier_of(member) == name) {
            return member;
        }
    }
    return std::nullopt;
}

}  // namespace llmgate::reflect

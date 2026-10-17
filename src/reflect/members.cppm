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

}  // namespace llmgate::reflect

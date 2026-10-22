export module llmgate.jsonedit;

import std;

export namespace llmgate::jsonedit {

std::expected<std::string, std::string> set(std::string_view text, std::span<std::string const> path, std::string_view value);

std::expected<std::string, std::string> remove(std::string_view text, std::span<std::string const> path);

std::string diff(std::string_view before, std::string_view after);

}  // namespace llmgate::jsonedit

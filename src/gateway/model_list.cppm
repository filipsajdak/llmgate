export module llmgate.model_list;

import std;

export namespace llmgate::model_list {

constexpr std::size_t longest_text = 200;

enum class access { unknown, enabled, disabled };

struct entry {
    std::string id{};
    std::string family{};
    std::string tokenizer{};
    std::uint64_t context = 0;
    std::uint64_t prompt = 0;
    std::uint64_t output = 0;
    access policy = access::unknown;
    bool hidden = false;
    std::vector<std::string> endpoints{};
};

constexpr bool usable(entry const& model) {
    return model.policy != access::disabled;
}

}  // namespace llmgate::model_list

export namespace llmgate::model_list {

std::expected<std::vector<entry>, std::string> parse(std::string_view body);

std::string printable(std::string_view text, std::size_t limit);

std::string render(std::span<entry const> models);

}  // namespace llmgate::model_list

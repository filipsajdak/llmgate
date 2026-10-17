import llmgate.properties;
import std;

namespace {

using namespace llmgate::properties;

constexpr std::string_view json_alphabet = R"({}[]":,\a1 u)";
constexpr std::string_view config_alphabet = R"({}[]":,/*a1 -t)";
constexpr std::size_t config_length = LLMGATE_CONFIG_LENGTH;
constexpr std::string_view routing_alphabet = R"({}":,/?:smodelopustrue.h1 )";
constexpr std::size_t routing_length = LLMGATE_ROUTING_LENGTH;
constexpr std::string_view sse_alphabet = "dat: \n\re";
constexpr std::size_t json_length = LLMGATE_JSON_LENGTH;
constexpr std::size_t sse_length = LLMGATE_SSE_LENGTH;
constexpr std::size_t guard_full_length = LLMGATE_GUARD_FULL_LENGTH;
constexpr std::size_t guard_core_length = LLMGATE_GUARD_CORE_LENGTH;
constexpr std::array<std::size_t, 6> core_tokens{
    static_cast<std::size_t>(token::start),         static_cast<std::size_t>(token::block_start_0),
    static_cast<std::size_t>(token::delta_0),       static_cast<std::size_t>(token::stop_0),
    static_cast<std::size_t>(token::message_delta), static_cast<std::size_t>(token::message_stop),
};

static_assert(for_all_texts(json_alphabet, json_length, &jsonscan_holds), "a short JSON-like text breaks the scanner");
static_assert(for_all_texts(config_alphabet, config_length, &json_holds), "a short config-like text breaks the JSON parser or reader");
static_assert(for_all_texts(routing_alphabet, routing_length, &routing_holds), "a short text breaks routing or request preparation");
static_assert(for_all_texts(sse_alphabet, sse_length, &sse_holds), "a short SSE-like text breaks the parser");
static_assert(for_all_sequences(token_count, guard_full_length, &guard_holds_for_tokens),
              "an event sequence makes the guard emit an invalid stream");
static_assert(for_all_sequences(core_tokens.size(), guard_core_length,
                                [](std::span<std::size_t const> const digits) {
                                    std::vector<std::size_t> mapped;
                                    for (auto const digit : digits) {
                                        mapped.push_back(core_tokens[digit]);
                                    }
                                    return guard_holds_for_tokens(mapped);
                                }),
              "a longer core event sequence makes the guard emit an invalid stream");

}  // namespace

int main() {}

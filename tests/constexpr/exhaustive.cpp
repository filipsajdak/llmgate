import llmgate.properties;
import std;

namespace {

using namespace llmgate::properties;

constexpr std::string_view json_alphabet = R"({}[]":,\a1 u)";
constexpr std::string_view config_alphabet = R"({}[]":,/*a1 -t)";
constexpr std::string_view routing_alphabet = R"({}":/?h1 )";
constexpr std::string_view settings_alphabet = R"({}[]":,pdeffortLvs1 )";
constexpr std::string_view launcher_alphabet = R"({}":,/.0-9a ref)";
constexpr std::string_view usage_alphabet = R"({}":,0-9a_ep)";
constexpr std::string_view selection_alphabet = R"(fsatT :,b)";
constexpr std::string_view quota_alphabet = R"({}":,1 qstru)";
constexpr std::string_view doctor_alphabet = "\x01\x02\x04\x08\x10\x20\x40";
constexpr std::string_view writer_alphabet = R"(a,"\)";
constexpr std::string_view lenient_alphabet = "\x01\x02\x03\x04\x05\x06\x07";
constexpr std::string_view sse_alphabet = "dat: \n\re";
constexpr std::array<std::size_t, 6> core_tokens{
    static_cast<std::size_t>(token::start),         static_cast<std::size_t>(token::block_start_0),
    static_cast<std::size_t>(token::delta_0),       static_cast<std::size_t>(token::stop_0),
    static_cast<std::size_t>(token::message_delta), static_cast<std::size_t>(token::message_stop),
};

#if LLMGATE_CHECK == 1
static_assert(for_all_texts(json_alphabet, LLMGATE_JSON_LENGTH, &jsonscan_holds), "a short JSON-like text breaks the scanner");
#elif LLMGATE_CHECK == 2
static_assert(for_all_texts(config_alphabet, LLMGATE_CONFIG_LENGTH, &json_holds), "a short config-like text breaks the JSON parser or reader");
#elif LLMGATE_CHECK == 3
static_assert(for_all_texts(routing_alphabet, LLMGATE_ROUTING_LENGTH, &routing_holds), "a short text breaks routing, access checks or request preparation");
#elif LLMGATE_CHECK == 4
static_assert(for_all_texts(sse_alphabet, LLMGATE_SSE_LENGTH, &sse_holds), "a short SSE-like text breaks the parser");
#elif LLMGATE_CHECK == 6
static_assert(for_all_texts(settings_alphabet, LLMGATE_CONFIG_LENGTH, &settings_holds), "a short text breaks the settings merge");
#elif LLMGATE_CHECK == 7
static_assert(for_all_texts(launcher_alphabet, LLMGATE_CONFIG_LENGTH, &launcher_holds), "a short text breaks the launcher helpers");
#elif LLMGATE_CHECK == 8
static_assert(for_all_texts(usage_alphabet, LLMGATE_USAGE_LENGTH, &usage_holds), "a short text breaks the usage ledger helpers");
#elif LLMGATE_CHECK == 9
static_assert(for_all_texts(selection_alphabet, LLMGATE_SELECTION_LENGTH, &selection_holds), "a short text breaks the circuit breaker or the route order");
#elif LLMGATE_CHECK == 10
static_assert(for_all_texts(quota_alphabet, LLMGATE_QUOTA_LENGTH, &quota_holds), "a short text breaks the quota reader");
#elif LLMGATE_CHECK == 11
static_assert(for_all_texts(doctor_alphabet, LLMGATE_DOCTOR_LENGTH, &doctor_holds), "a combination of facts breaks the doctor checks");
#elif LLMGATE_CHECK == 12
static_assert(for_all_texts(writer_alphabet, LLMGATE_WRITER_LENGTH, &writer_holds), "a short text breaks the write and read round trip");
#elif LLMGATE_CHECK == 13
static_assert(for_all_texts(lenient_alphabet, LLMGATE_LENIENT_LENGTH, &lenient_holds), "a short text makes the lenient reader disagree with the strict one");
#elif LLMGATE_CHECK == 5
static_assert(for_all_sequences(token_count, LLMGATE_GUARD_FULL_LENGTH, &guard_holds_for_tokens),
              "an event sequence makes the guard emit an invalid stream");
static_assert(for_all_sequences(core_tokens.size(), LLMGATE_GUARD_CORE_LENGTH,
                                [](std::span<std::size_t const> const digits) {
                                    std::vector<std::size_t> mapped;
                                    for (auto const digit : digits) {
                                        mapped.push_back(core_tokens[digit]);
                                    }
                                    return guard_holds_for_tokens(mapped);
                                }),
              "a longer core event sequence makes the guard emit an invalid stream");
#endif

}  // namespace

int main() {}

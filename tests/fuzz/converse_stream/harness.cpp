import llmgate.anthropic;
import llmgate.converse_stream;
import llmgate.fuzz_driver;
import llmgate.sse;
import std;

namespace {

namespace cv = llmgate::converse;
namespace sse = llmgate::sse;

struct digest {
    std::string frames{};
    bool complete = false;
    bool grammar_ok = false;
    bool operator==(digest const&) const = default;
};

digest translate(std::string_view const script, std::size_t const piece) {
    cv::stream_translator translator{"m", {}};
    llmgate::anthropic::stream_guard guard;
    std::vector<llmgate::anthropic::output_event> out;
    auto emit = [&out](llmgate::anthropic::output_event const& item) { out.push_back(item); };
    auto ignore = [](llmgate::anthropic::repair const&) {};
    auto through = [&](sse::event const& item) { guard.on_event(item, emit, ignore); };
    std::string pending;
    for (std::size_t at = 0; at < script.size(); at += piece) {
        pending.append(script.substr(at, piece));
        for (auto newline = pending.find('\n'); newline != std::string::npos; newline = pending.find('\n')) {
            auto const line = std::string_view{pending}.substr(0, newline);
            auto const space = line.find(' ');
            if (space != std::string_view::npos) {
                translator.on_event(line.substr(0, space), line.substr(space + 1), through);
            }
            pending.erase(0, newline + 1);
        }
    }
    translator.finish(through);
    guard.on_end(emit, ignore);
    digest seen{.complete = guard.complete(), .grammar_ok = llmgate::anthropic::validate_grammar(out).has_value()};
    for (auto const& item : out) {
        std::format_to(std::back_inserter(seen.frames), "{}{};", item.name, item.data);
    }
    return seen;
}

void check(std::span<std::byte const> input) {
    std::string const script(reinterpret_cast<char const*>(input.data()), input.size());
    auto const whole = translate(script, std::max<std::size_t>(script.size(), 1));
    auto const bytewise = translate(script, 1);
    if (!(whole == bytewise) || !whole.grammar_ok) {
        std::abort();
    }
}

}  // namespace

int main(int argc, char** argv) {
    return llmgate::fuzz::run(argc, argv, check);
}

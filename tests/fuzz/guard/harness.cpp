import llmgate.anthropic;
import llmgate.fuzz_driver;
import llmgate.sse;
import std;

namespace {

using namespace llmgate;

void check(std::span<std::byte const> input) {
    sse::parser parser;
    anthropic::stream_guard guard;
    std::vector<anthropic::output_event> out;
    auto emit = [&out](anthropic::output_event const& item) { out.push_back(item); };
    auto ignore = [](anthropic::repair const&) {};
    parser.feed(input, [&](sse::event const& item) { guard.on_event(item, emit, ignore); });
    parser.finish([&](sse::event const& item) { guard.on_event(item, emit, ignore); });
    guard.on_end(emit, ignore);
    if (!guard.complete() || !anthropic::validate_grammar(out).has_value()) {
        std::abort();
    }
}

}  // namespace

int main(int argc, char** argv) {
    return llmgate::fuzz::run(argc, argv, check);
}

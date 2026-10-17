import llmgate.fuzz_driver;
import llmgate.sse;
import std;

namespace {

using namespace llmgate::sse;

struct owned {
    std::string name;
    std::string data;
    std::string id;
    bool operator==(owned const&) const = default;
};

std::vector<owned> parse_with_pieces(std::span<std::byte const> data, std::size_t const piece) {
    parser reader;
    std::vector<owned> events;
    auto sink = [&events](event const& item) {
        events.push_back({.name = std::string{item.name}, .data = std::string{item.data}, .id = std::string{item.id}});
    };
    for (std::size_t at = 0; at < data.size(); at += piece) {
        reader.feed(data.subspan(at, std::min(piece, data.size() - at)), sink);
    }
    reader.finish(sink);
    return events;
}

void check(std::span<std::byte const> input) {
    auto const whole = parse_with_pieces(input, std::max<std::size_t>(input.size(), 1));
    auto const bytewise = parse_with_pieces(input, 1);
    if (!std::ranges::equal(whole, bytewise)) {
        std::abort();
    }
    for (auto const& item : whole) {
        std::string wire;
        write_event(wire, item.name, item.data);
        auto const again = parse_with_pieces(std::as_bytes(std::span{wire}), std::max<std::size_t>(wire.size(), 1));
        if (again.size() != 1 || again.front().name != item.name || again.front().data != item.data) {
            std::abort();
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    return llmgate::fuzz::run(argc, argv, check);
}

import llmgate.eventstream;
import llmgate.fuzz_driver;
import std;

namespace {

namespace es = llmgate::eventstream;

struct digest {
    std::string frames{};
    es::failure failure = es::failure::none;
    bool operator==(digest const&) const = default;
};

digest decode_in_pieces(std::string_view const wire, std::size_t const piece) {
    es::decoder reader;
    digest seen;
    for (std::size_t at = 0; at < wire.size() && seen.failure == es::failure::none; at += piece) {
        seen.failure = reader.feed(wire.substr(at, piece), [&seen](es::frame const& each) {
            seen.frames += std::format("{}|{}|{}|{};", each.message_type, each.event_type, each.exception_type, each.payload.size());
            if (auto const made = es::event_of(each)) {
                seen.frames += made->name + made->data;
            }
        });
    }
    return seen;
}

void check(std::span<std::byte const> input) {
    std::string const wire(reinterpret_cast<char const*>(input.data()), input.size());
    auto const whole = decode_in_pieces(wire, std::max<std::size_t>(wire.size(), 1));
    auto const bytewise = decode_in_pieces(wire, 1);
    auto const chunked = decode_in_pieces(wire, 7);
    if (!(whole == bytewise) || !(whole == chunked)) {
        std::abort();
    }
}

}  // namespace

int main(int argc, char** argv) {
    return llmgate::fuzz::run(argc, argv, check);
}

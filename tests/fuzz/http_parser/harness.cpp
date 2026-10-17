import llmgate.fuzz_driver;
import llmgate.http;
import std;

namespace {

using namespace llmgate::http;

constexpr std::size_t head_limit = 1024;

struct digest {
    std::string heads;
    std::string body;
    int completed = 0;
    int error = 0;
    bool operator==(digest const&) const = default;
};

digest run_with_pieces(kind const which, std::span<std::byte const> data, std::size_t const piece) {
    parser reader{which, {.max_head_bytes = head_limit}};
    digest seen;
    auto sink = [&seen](event const& item) {
        if (auto const* head = std::get_if<message_head>(&item)) {
            seen.heads += std::format("{}|{}|{}|{}|{}|{};", head->method, head->target, head->status, head->keep_alive,
                                      head->chunked, head->content_length.value_or(0));
            for (auto const& field : head->fields.all()) {
                seen.heads += std::format("{}={},", field.name, field.value);
            }
        } else if (auto const* chunk = std::get_if<body_chunk>(&item)) {
            seen.body.append(reinterpret_cast<char const*>(chunk->bytes.data()), chunk->bytes.size());
        } else {
            ++seen.completed;
        }
    };
    for (std::size_t at = 0; at < data.size() && seen.error == 0; at += piece) {
        auto part = data.subspan(at, std::min(piece, data.size() - at));
        while (!part.empty() && seen.error == 0) {
            auto const consumed = reader.feed(part, sink);
            if (!consumed) {
                seen.error = consumed.error().value();
                break;
            }
            part = part.subspan(*consumed);
        }
    }
    if (seen.error == 0) {
        auto const finished = reader.finish(sink);
        seen.error = finished ? 0 : finished.error().value();
    }
    return seen;
}

void check(std::span<std::byte const> input) {
    if (input.empty()) {
        return;
    }
    auto const which = (std::to_integer<unsigned>(input.front()) & 1U) != 0 ? kind::request : kind::response;
    auto const data = input.subspan(1);
    auto const whole = run_with_pieces(which, data, std::max<std::size_t>(data.size(), 1));
    auto const bytewise = run_with_pieces(which, data, 1);
    if (!(whole == bytewise)) {
        std::abort();
    }
}

}  // namespace

int main(int argc, char** argv) {
    return llmgate::fuzz::run(argc, argv, check);
}

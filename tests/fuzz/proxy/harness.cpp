import llmgate.fuzz_driver;
import llmgate.proxy;
import std;

namespace {

using namespace llmgate::proxy;

constexpr unsigned reply_mode = 0;
constexpr unsigned url_mode = 1;
constexpr unsigned list_mode = 2;
constexpr unsigned mode_count = 3;

void settled_replies_do_not_change(std::string_view const text) {
    std::optional<reply> decided;
    for (std::size_t length = 0; length <= text.size(); ++length) {
        auto const now = parse_reply(text.substr(0, length));
        if (decided && (now.which != decided->which || now.consumed != decided->consumed || now.status != decided->status)) {
            std::abort();
        }
        if (!decided && now.which != outcome::incomplete) {
            decided = now;
        }
        if (now.consumed > length) {
            std::abort();
        }
    }
}

void a_masked_url_never_shows_the_password(std::string_view const text) {
    auto const shown = mask(text);
    auto const found = parse(text);
    auto const at = text.rfind('@');
    auto const colon = text.find(':', std::string_view{"http://"}.size());
    if (found && !found->authorization.empty() && colon < at && at != std::string_view::npos) {
        auto const password = text.substr(colon + 1, at - colon - 1);
        if (password.size() > 3 && shown.contains(password)) {
            std::abort();
        }
    }
}

void a_bypass_list_never_reads_out_of_range(std::string_view const text) {
    auto const split = text.find('\n');
    auto const entries = split_list(text.substr(0, split));
    auto const host = split == std::string_view::npos ? std::string_view{} : text.substr(split + 1);
    static_cast<void>(bypasses(entries, host, 443));
    static_cast<void>(bypasses(entries, host, 0));
}

void check(std::span<std::byte const> input) {
    if (input.empty()) {
        return;
    }
    auto const mode = std::to_integer<unsigned>(input.front()) % mode_count;
    auto const body = input.subspan(1);
    std::string const text(reinterpret_cast<char const*>(body.data()), body.size());
    if (mode == reply_mode) {
        settled_replies_do_not_change(text);
    } else if (mode == url_mode) {
        a_masked_url_never_shows_the_password(text);
    } else if (mode == list_mode) {
        a_bypass_list_never_reads_out_of_range(text);
    }
}

}  // namespace

int main(int argc, char** argv) {
    return llmgate::fuzz::run(argc, argv, check);
}

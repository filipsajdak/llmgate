module llmgate.fetch;

import std;
import llmgate.http;
import llmgate.httpio;
import llmgate.net;
import llmgate.routing;

namespace llmgate::fetch {

async::task<std::expected<reply, std::error_code>> request(async::reactor& loop, tls::client_context const& tls_context,
                                                           std::string base_url, httpio::request_spec spec,
                                                           std::chrono::milliseconds const limit) {
    auto const target = routing::parse_url(base_url);
    if (!target) {
        co_return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
    }
    spec.host = target->host;
    spec.target = target->base_path + spec.target;
    std::optional<net::any_stream> stream;
    if (target->secure) {
        auto secure = co_await net::tls_stream::connect(loop, tls_context, target->host, target->port, limit);
        if (!secure) {
            co_return std::unexpected{secure.error()};
        }
        stream.emplace(std::move(*secure));
    } else {
        auto plain = co_await net::tcp_stream::connect(loop, target->host, target->port, limit);
        if (!plain) {
            co_return std::unexpected{plain.error()};
        }
        stream.emplace(std::move(*plain));
    }
    reply result{.status = 0, .body = {}};
    auto const finished = co_await httpio::exchange(
        *stream, std::move(spec),
        [&result](http::event const& item) {
            if (auto const* head = std::get_if<http::message_head>(&item)) {
                result.status = head->status;
            } else if (auto const* chunk = std::get_if<http::body_chunk>(&item)) {
                result.body.append(reinterpret_cast<char const*>(chunk->bytes.data()), chunk->bytes.size());
            }
        },
        httpio::limits{.read_idle = limit});
    if (!finished) {
        co_return std::unexpected{finished.error()};
    }
    co_return result;
}

}  // namespace llmgate::fetch

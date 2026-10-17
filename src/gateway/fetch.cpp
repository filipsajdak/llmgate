module llmgate.fetch;

import std;
import llmgate.http;
import llmgate.httpio;
import llmgate.net;
import llmgate.routing;

namespace llmgate::fetch {

namespace {

async::task<std::expected<net::any_stream, std::error_code>> open_stream(async::reactor& loop, tls::client_context const& tls_context,
                                                                         routing::endpoint_url const target,
                                                                         std::chrono::milliseconds const limit) {
    if (target.secure) {
        auto secure = co_await net::tls_stream::connect(loop, tls_context, target.host, target.port, limit);
        if (!secure) {
            co_return std::unexpected{secure.error()};
        }
        co_return net::any_stream{std::move(*secure)};
    }
    auto plain = co_await net::tcp_stream::connect(loop, target.host, target.port, limit);
    if (!plain) {
        co_return std::unexpected{plain.error()};
    }
    co_return net::any_stream{std::move(*plain)};
}

void absorb(reply& result, http::event const& item) {
    if (auto const* head = std::get_if<http::message_head>(&item)) {
        result.status = head->status;
    } else if (auto const* chunk = std::get_if<http::body_chunk>(&item)) {
        result.body.append(reinterpret_cast<char const*>(chunk->bytes.data()), chunk->bytes.size());
    }
}

}  // namespace

async::task<std::expected<reply, std::error_code>> request(async::reactor& loop, tls::client_context const& tls_context,
                                                           std::string base_url, httpio::request_spec spec,
                                                           std::chrono::milliseconds const limit) {
    auto const target = routing::parse_url(base_url);
    if (!target) {
        co_return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
    }
    spec.host = target->host;
    spec.target = target->base_path + spec.target;
    auto stream = co_await open_stream(loop, tls_context, *target, limit);
    if (!stream) {
        co_return std::unexpected{stream.error()};
    }
    reply result{.status = 0, .body = {}};
    auto const finished = co_await httpio::exchange(*stream, std::move(spec), [&result](http::event const& item) { absorb(result, item); },
                                                    httpio::limits{.read_idle = limit});
    if (!finished) {
        co_return std::unexpected{finished.error()};
    }
    co_return result;
}

}  // namespace llmgate::fetch

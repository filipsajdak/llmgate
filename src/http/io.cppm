export module llmgate.httpio;

import std;
import llmgate.async;
import llmgate.http;
import llmgate.net;

export namespace llmgate::httpio {

using net::result_void;

struct limits {
    std::size_t max_head_bytes = 65536;
    std::size_t max_body_bytes = 33554432;
    net::idle_limit read_idle = async::duration{300000};
};

struct request {
    http::message_head head;
    std::string body;
};

struct request_spec {
    std::string method;
    std::string target;
    std::string host;
    http::header_map fields{};
    std::string body{};
};

}  // namespace llmgate::httpio

namespace llmgate::httpio::detail {

constexpr std::size_t read_buffer_bytes = 16384;
constexpr std::string_view crlf = "\r\n";

constexpr bool is_token_char(char const c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           std::string_view{"!#$%&'*+-.^_`|~"}.contains(c);
}

bool valid_field(http::header const& field) noexcept {
    auto const clean_value = std::ranges::none_of(field.value, [](char const c) { return c == '\r' || c == '\n' || c == '\0'; });
    return !field.name.empty() && std::ranges::all_of(field.name, is_token_char) && clean_value;
}

std::string_view reason_phrase(int const status) noexcept {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 413: return "Payload Too Large";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 504: return "Gateway Timeout";
        default: return "Status";
    }
}

std::expected<void, std::error_code> append_fields(std::string& out, http::header_map const& fields) {
    for (auto const& field : fields.all()) {
        if (!valid_field(field)) {
            return std::unexpected{http::make_error_code(http::errc::invalid_header)};
        }
        std::format_to(std::back_inserter(out), "{}: {}\r\n", field.name, field.value);
    }
    return {};
}

std::span<std::byte const> as_bytes(std::string const& text) noexcept {
    return std::as_bytes(std::span{text});
}

}  // namespace llmgate::httpio::detail

export namespace llmgate::httpio {

std::expected<std::string, std::error_code> format_request(request_spec const& spec) {
    auto head = std::format("{} {} HTTP/1.1\r\nHost: {}\r\nContent-Length: {}\r\n", spec.method, spec.target, spec.host,
                            spec.body.size());
    if (auto const added = detail::append_fields(head, spec.fields); !added) {
        return std::unexpected{added.error()};
    }
    head += detail::crlf;
    return head + spec.body;
}

std::expected<std::string, std::error_code> format_response_head(int const status, http::header_map const& fields,
                                                                 std::optional<std::size_t> const content_length) {
    auto head = std::format("HTTP/1.1 {} {}\r\n", status, detail::reason_phrase(status));
    if (content_length) {
        std::format_to(std::back_inserter(head), "Content-Length: {}\r\n", *content_length);
    } else {
        head += "Transfer-Encoding: chunked\r\n";
    }
    if (auto const added = detail::append_fields(head, fields); !added) {
        return std::unexpected{added.error()};
    }
    head += detail::crlf;
    return head;
}

std::string frame_chunk(std::span<std::byte const> bytes) {
    auto framed = std::format("{:x}\r\n", bytes.size());
    framed.append(reinterpret_cast<char const*>(bytes.data()), bytes.size());
    framed += detail::crlf;
    return framed;
}

template <net::byte_stream Stream>
class message_reader {
public:
    message_reader(Stream& stream, http::kind const which, limits const& bounds)
        : stream_{&stream}, parser_{which, {.max_head_bytes = bounds.max_head_bytes}},
          buffer_(detail::read_buffer_bytes) {}

    template <std::invocable<http::event const&> Sink>
    async::task<result_void> read(Sink on_event, net::idle_limit const idle) {
        bool done = false;
        auto track = [&done, &on_event](http::event const& item) {
            done = done || std::holds_alternative<http::message_complete>(item);
            on_event(item);
        };
        while (!done) {
            if (begin_ == end_) {
                auto const received = co_await stream_->read_some(buffer_, idle);
                if (!received) {
                    co_return std::unexpected{received.error()};
                }
                if (*received == 0) {
                    co_return co_await end_of_stream(track, done);
                }
                begin_ = 0;
                end_ = *received;
            }
            auto const consumed = parser_.feed(std::span<std::byte const>{buffer_}.subspan(begin_, end_ - begin_), track);
            if (!consumed) {
                co_return std::unexpected{consumed.error()};
            }
            fresh_ = false;
            begin_ += *consumed;
        }
        fresh_ = true;
        co_return result_void{};
    }

private:
    template <class Sink>
    async::task<result_void> end_of_stream(Sink& track, bool& done) {
        if (fresh_) {
            co_return std::unexpected{http::make_error_code(http::errc::connection_closed)};
        }
        auto const finished = parser_.finish(track);
        if (!finished) {
            co_return finished;
        }
        if (!done) {
            co_return std::unexpected{http::make_error_code(http::errc::truncated_message)};
        }
        co_return result_void{};
    }

    Stream* stream_;
    http::parser parser_;
    std::vector<std::byte> buffer_;
    std::size_t begin_ = 0;
    std::size_t end_ = 0;
    bool fresh_ = true;
};

template <net::byte_stream Stream, std::invocable<http::event const&> Sink>
async::task<result_void> exchange(Stream& stream, request_spec spec, Sink on_event, limits bounds = {}) {
    auto const wire = format_request(spec);
    if (!wire) {
        co_return std::unexpected{wire.error()};
    }
    if (auto const sent = co_await stream.write_all(detail::as_bytes(*wire)); !sent) {
        co_return sent;
    }
    message_reader<Stream> reader{stream, http::kind::response, bounds};
    co_return co_await reader.read(std::move(on_event), bounds.read_idle);
}

template <net::byte_stream Stream>
class response_writer {
public:
    explicit response_writer(Stream& stream) noexcept : stream_{&stream} {}

    bool started() const noexcept { return started_; }

    async::task<result_void> send(int const status, http::header_map const& fields, std::string_view const body) {
        auto head = format_response_head(status, fields, body.size());
        if (!head) {
            co_return std::unexpected{head.error()};
        }
        started_ = true;
        head->append(body);
        co_return co_await stream_->write_all(detail::as_bytes(*head));
    }

    async::task<result_void> begin_stream(int const status, http::header_map const& fields) {
        auto head = format_response_head(status, fields, std::nullopt);
        if (!head) {
            co_return std::unexpected{head.error()};
        }
        started_ = true;
        co_return co_await stream_->write_all(detail::as_bytes(*head));
    }

    async::task<result_void> write_chunk(std::span<std::byte const> bytes) {
        if (bytes.empty()) {
            co_return result_void{};
        }
        auto const framed = frame_chunk(bytes);
        co_return co_await stream_->write_all(detail::as_bytes(framed));
    }

    async::task<result_void> end_stream() {
        static constexpr std::string_view last_chunk = "0\r\n\r\n";
        co_return co_await stream_->write_all(std::as_bytes(std::span{last_chunk}));
    }

private:
    Stream* stream_;
    bool started_ = false;
};

template <net::byte_stream Stream, class Handler>
async::task<void> serve_connection(Stream stream, Handler& handler, limits bounds) {
    message_reader<Stream> reader{stream, http::kind::request, bounds};
    response_writer<Stream> writer{stream};
    for (;;) {
        request incoming{};
        bool oversized = false;
        auto collect = [&incoming, &oversized, &bounds](http::event const& item) {
            if (auto const* head = std::get_if<http::message_head>(&item)) {
                incoming.head = *head;
            } else if (auto const* chunk = std::get_if<http::body_chunk>(&item)) {
                oversized = oversized || incoming.body.size() + chunk->bytes.size() > bounds.max_body_bytes;
                if (!oversized) {
                    incoming.body.append(reinterpret_cast<char const*>(chunk->bytes.data()), chunk->bytes.size());
                }
            }
        };
        auto const received = co_await reader.read(collect, bounds.read_idle);
        if (!received) {
            if (received.error() == http::errc::head_too_large) {
                (void)co_await writer.send(431, {}, "");
            } else if (received.error() == http::errc::malformed_message) {
                (void)co_await writer.send(400, {}, "");
            }
            co_return;
        }
        if (oversized) {
            (void)co_await writer.send(413, {}, "");
            co_return;
        }
        auto const handled = co_await handler(incoming, writer);
        if (!handled) {
            if (!writer.started()) {
                (void)co_await writer.send(500, {}, "");
            }
            co_return;
        }
        if (!incoming.head.keep_alive) {
            co_return;
        }
    }
}

template <class Handler>
async::task<void> serve(net::listener& listening, Handler& handler, limits bounds = {}) {
    for (;;) {
        auto accepted = co_await listening.accept();
        if (!accepted) {
            co_return;
        }
        async::spawn(serve_connection(std::move(*accepted), handler, bounds));
    }
}

}  // namespace llmgate::httpio

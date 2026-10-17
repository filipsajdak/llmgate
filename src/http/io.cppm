export module llmgate.httpio;

import std;
import llmgate.async;
import llmgate.http;
import llmgate.net;
import llmgate.shutdown;

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

constexpr bool valid_field(http::header const& field) noexcept {
    auto const clean_value = std::ranges::none_of(field.value, [](char const c) { return c == '\r' || c == '\n' || c == '\0'; });
    return !field.name.empty() && std::ranges::all_of(field.name, is_token_char) && clean_value;
}

constexpr auto reason_phrases = std::to_array<std::pair<int, std::string_view>>({
    {200, "OK"},
    {400, "Bad Request"},
    {404, "Not Found"},
    {413, "Payload Too Large"},
    {431, "Request Header Fields Too Large"},
    {500, "Internal Server Error"},
    {502, "Bad Gateway"},
    {504, "Gateway Timeout"},
});

constexpr std::string_view reason_phrase(int const status) noexcept {
    auto const match = std::ranges::find(reason_phrases, status, &std::pair<int, std::string_view>::first);
    return match == reason_phrases.end() ? std::string_view{"Status"} : match->second;
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

constexpr std::span<std::byte const> as_bytes(std::string const& text) noexcept {
    return std::as_bytes(std::span{text});
}

static_assert(reason_phrase(404) == "Not Found");
static_assert(reason_phrase(299) == "Status");
static_assert(is_token_char('-') && !is_token_char(':') && !is_token_char(' '));

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
    async::task<result_void> read(Sink on_event, net::read_options const options) {
        if (reading_) {
            co_return std::unexpected{std::make_error_code(std::errc::operation_in_progress)};
        }
        async::exclusive_scope const exclusive{reading_};
        for (;;) {
            auto const finished = co_await advance(on_event, options);
            if (!finished) {
                co_return std::unexpected{finished.error()};
            }
            if (*finished) {
                co_return result_void{};
            }
        }
    }

    template <std::invocable<http::event const&> Sink>
    async::task<std::expected<bool, std::error_code>> step(Sink& on_event, net::read_options const options) {
        if (reading_) {
            co_return std::unexpected{std::make_error_code(std::errc::operation_in_progress)};
        }
        async::exclusive_scope const exclusive{reading_};
        co_return co_await advance(on_event, options);
    }

private:
    template <std::invocable<http::event const&> Sink>
    async::task<std::expected<bool, std::error_code>> advance(Sink& on_event, net::read_options const options) {
        bool done = false;
        auto track = [&done, &on_event](http::event const& item) {
            done = done || std::holds_alternative<http::message_complete>(item);
            on_event(item);
        };
        if (begin_ == end_) {
            auto const received = co_await stream_->read_some(buffer_, options);
            if (!received) {
                co_return std::unexpected{received.error()};
            }
            if (*received == 0) {
                auto const closed = co_await end_of_stream(track, done);
                if (!closed) {
                    co_return std::unexpected{closed.error()};
                }
                co_return true;
            }
            begin_ = 0;
            end_ = *received;
        }
        auto const consumed = parser_.feed(std::span<std::byte const>{buffer_}.subspan(begin_, end_ - begin_), track);
        if (!consumed) {
            co_return std::unexpected{consumed.error()};
        }
        begin_ += *consumed;
        fresh_ = done;
        co_return done;
    }

    template <std::invocable<http::event const&> Sink>
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
    bool reading_ = false;
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
    co_return co_await reader.read(std::move(on_event), {.idle = bounds.read_idle});
}

template <net::byte_stream Stream>
class response_writer {
public:
    explicit response_writer(Stream& stream) noexcept : stream_{&stream} {}

    bool started() const noexcept { return started_; }
    int descriptor() const noexcept { return stream_->descriptor(); }

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

struct request_collector {
    request* incoming;
    bool* oversized;
    std::size_t max_body_bytes;

    void operator()(http::event const& item) const { std::visit(*this, item); }

    void operator()(http::message_head const& head) const { incoming->head = head; }

    void operator()(http::body_chunk const& chunk) const {
        *oversized = *oversized || incoming->body.size() + chunk.bytes.size() > max_body_bytes;
        if (!*oversized) {
            incoming->body.append(reinterpret_cast<char const*>(chunk.bytes.data()), chunk.bytes.size());
        }
    }

    void operator()(http::message_complete const&) const {}
};

std::optional<int> status_for_unreadable(std::error_code const error) noexcept {
    if (error == http::errc::head_too_large) {
        return 431;
    }
    if (error == http::errc::malformed_message) {
        return 400;
    }
    return std::nullopt;
}

template <class Handler, class Stream>
concept request_handler = net::byte_stream<Stream> && requires(Handler& handler, request const& incoming, response_writer<Stream>& out) {
    { handler(incoming, out) } -> std::same_as<async::task<result_void>>;
};

template <class Writer>
concept error_responder = requires(Writer& out) {
    { out.send(0, http::header_map{}, std::string_view{}) } -> std::same_as<async::task<result_void>>;
};

template <net::byte_stream Stream, request_handler<Stream> Handler>
async::task<bool> dispatch(Handler& handler, request const& incoming, response_writer<Stream>& writer) {
    auto const handled = co_await handler(incoming, writer);
    if (handled) {
        co_return true;
    }
    if (!writer.started()) {
        (void)co_await writer.send(500, {}, "");
    }
    co_return false;
}

template <net::byte_stream Stream, request_handler<Stream> Handler>
async::task<void> serve_connection(Stream stream, Handler& handler, limits bounds) {
    message_reader<Stream> reader{stream, http::kind::request, bounds};
    response_writer<Stream> writer{stream};
    for (;;) {
        request incoming{};
        bool oversized = false;
        request_collector const collect{.incoming = &incoming, .oversized = &oversized, .max_body_bytes = bounds.max_body_bytes};
        auto const received = co_await reader.read(collect, {.idle = bounds.read_idle});
        if (!received) {
            if (auto const status = status_for_unreadable(received.error())) {
                (void)co_await writer.send(*status, {}, "");
            }
            co_return;
        }
        if (oversized) {
            (void)co_await writer.send(413, {}, "");
            co_return;
        }
        if (!co_await dispatch(handler, incoming, writer) || !incoming.head.keep_alive) {
            co_return;
        }
    }
}

template <request_handler<net::tcp_stream> Handler>
async::task<void> serve(net::listener& listening, Handler& handler, limits bounds = {}) {
    for (;;) {
        auto accepted = co_await listening.accept();
        if (!accepted) {
            co_return;
        }
        listening.reactor().spawn(serve_connection(std::move(*accepted), handler, bounds));
    }
}

constexpr int status_unavailable = 503;

template <class Handler>
class counted_handler {
public:
    counted_handler(Handler& inner, std::shared_ptr<shutdown::drain_gate> gate) noexcept : inner_{&inner}, gate_{std::move(gate)} {}

    template <error_responder Writer>
    async::task<result_void> operator()(request const& incoming, Writer& out) {
        if (gate_->closed()) {
            (void)co_await out.send(status_unavailable, {}, "");
            co_return std::unexpected{std::make_error_code(std::errc::operation_canceled)};
        }
        shutdown::drain_gate::scope const counting{*gate_};
        co_return co_await (*inner_)(incoming, out);
    }

private:
    Handler* inner_;
    std::shared_ptr<shutdown::drain_gate> gate_;
};

template <request_handler<net::tcp_stream> Handler>
async::task<void> serve_owned(net::tcp_stream stream, std::shared_ptr<counted_handler<Handler>> counted, limits bounds) {
    co_await serve_connection(std::move(stream), *counted, bounds);
}

template <request_handler<net::tcp_stream> Handler>
async::task<std::size_t> serve_until(net::listener& listening, Handler& handler, limits bounds, int const stop_descriptor,
                                     async::duration const drain) {
    auto const gate = std::make_shared<shutdown::drain_gate>(listening.reactor());
    auto const counted = std::make_shared<counted_handler<Handler>>(handler, gate);
    for (;;) {
        auto accepted = co_await listening.accept(stop_descriptor);
        if (!accepted) {
            break;
        }
        listening.reactor().spawn(serve_owned(std::move(*accepted), counted, bounds));
    }
    listening.stop_listening();
    gate->close();
    if (gate->active() != 0) {
        listening.reactor().spawn(gate->expire_after(drain));
        while (gate->active() != 0 && !gate->expired()) {
            co_await gate->changed();
        }
    }
    co_return gate->active();
}

struct plain_function_object {
    void operator()(request const&, response_writer<net::tcp_stream>&) const;
};

static_assert(!request_handler<int, net::tcp_stream> && !request_handler<plain_function_object, net::tcp_stream>);
static_assert(!error_responder<int>);

}  // namespace llmgate::httpio

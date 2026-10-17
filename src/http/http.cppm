module;

#include <llhttp.h>

export module llmgate.http;

import std;
import llmgate.reflect;

export namespace llmgate::http {

enum class errc { malformed_message = 1, head_too_large, truncated_message };

}  // namespace llmgate::http

export namespace std {
template <>
struct is_error_code_enum<llmgate::http::errc> : true_type {};
}  // namespace std

export namespace llmgate::http {

std::error_code make_error_code(errc const value) noexcept {
    return reflect::make_code(value);
}

enum class kind { request, response };

struct header {
    std::string name;
    std::string value;
};

class header_map {
public:
    void add(std::string name, std::string value) { items_.push_back({.name = std::move(name), .value = std::move(value)}); }

    std::optional<std::string_view> find(std::string_view const name) const noexcept {
        auto const match = std::ranges::find_if(items_, [name](header const& item) { return same_name(item.name, name); });
        if (match == items_.end()) {
            return std::nullopt;
        }
        return std::string_view{match->value};
    }

    std::span<header const> all() const noexcept { return items_; }

private:
    static bool same_name(std::string_view const left, std::string_view const right) noexcept {
        return std::ranges::equal(left, right, [](char const a, char const b) { return lower(a) == lower(b); });
    }

    static constexpr char lower(char const c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

    std::vector<header> items_;
};

struct message_head {
    std::string method{};
    std::string target{};
    int status{};
    std::string reason{};
    int minor_version{};
    header_map fields{};
    bool keep_alive{};
    bool chunked{};
    std::optional<std::uint64_t> content_length{};
};

struct body_chunk {
    std::span<std::byte const> bytes;
};

struct message_complete {};

using event = std::variant<message_head, body_chunk, message_complete>;

using event_sink = std::function_ref<void(event const&)>;

struct parser_limits {
    std::size_t max_head_bytes = 65536;
};

class parser {
public:
    explicit parser(kind const which, parser_limits const limits = {}) : limits_{limits} {
        // Exception: llhttp_settings_t is a C struct initialised by its own function, not a braced expression.
        llhttp_settings_init(&settings_);
        settings_.on_message_begin = &parser::begin;
        settings_.on_url = &parser::url;
        settings_.on_status = &parser::status;
        settings_.on_header_field = &parser::field;
        settings_.on_header_value = &parser::value;
        settings_.on_headers_complete = &parser::headers_complete;
        settings_.on_body = &parser::body;
        settings_.on_message_complete = &parser::complete;
        llhttp_init(&state_, which == kind::request ? HTTP_REQUEST : HTTP_RESPONSE, &settings_);
        state_.data = this;
    }

    parser(parser const&) = delete;
    parser& operator=(parser const&) = delete;

    template <std::invocable<event const&> Sink>
    std::expected<void, std::error_code> feed(std::span<std::byte const> data, Sink&& on_event) {
        event_sink const borrowed{on_event};
        sink_ = &borrowed;
        auto const outcome = llhttp_execute(&state_, reinterpret_cast<char const*>(data.data()), data.size());
        sink_ = nullptr;
        return outcome == HPE_OK ? std::expected<void, std::error_code>{} : failure(outcome);
    }

    template <std::invocable<event const&> Sink>
    std::expected<void, std::error_code> finish(Sink&& on_event) {
        event_sink const borrowed{on_event};
        sink_ = &borrowed;
        auto const outcome = llhttp_finish(&state_);
        sink_ = nullptr;
        if (outcome == HPE_OK) {
            return {};
        }
        diagnostic_ = llhttp_get_error_reason(&state_) ? llhttp_get_error_reason(&state_) : "";
        return std::unexpected{make_error_code(errc::truncated_message)};
    }

    std::string const& diagnostic() const noexcept { return diagnostic_; }

private:
    static parser& from(llhttp_t* const state) noexcept { return *static_cast<parser*>(state->data); }

    static int begin(llhttp_t* const state) noexcept {
        auto& self = from(state);
        self.head_ = {};
        self.head_size_ = 0;
        self.in_value_ = false;
        self.field_.clear();
        self.value_.clear();
        return 0;
    }

    int account(std::size_t const length) noexcept {
        head_size_ += length;
        if (head_size_ > limits_.max_head_bytes) {
            limit_hit_ = true;
            return HPE_USER;
        }
        return 0;
    }

    static int url(llhttp_t* const state, char const* const at, std::size_t const length) noexcept {
        auto& self = from(state);
        self.head_.target.append(at, length);
        return self.account(length);
    }

    static int status(llhttp_t* const state, char const* const at, std::size_t const length) noexcept {
        auto& self = from(state);
        self.head_.reason.append(at, length);
        return self.account(length);
    }

    static int field(llhttp_t* const state, char const* const at, std::size_t const length) noexcept {
        auto& self = from(state);
        self.flush_pair();
        self.field_.append(at, length);
        return self.account(length);
    }

    static int value(llhttp_t* const state, char const* const at, std::size_t const length) noexcept {
        auto& self = from(state);
        self.in_value_ = true;
        self.value_.append(at, length);
        return self.account(length);
    }

    static int headers_complete(llhttp_t* const state) noexcept {
        auto& self = from(state);
        self.flush_pair();
        self.head_.method = llhttp_method_name(static_cast<llhttp_method_t>(state->method));
        self.head_.status = state->status_code;
        self.head_.minor_version = state->http_minor;
        self.head_.keep_alive = llhttp_should_keep_alive(state) != 0;
        self.head_.chunked = (state->flags & F_CHUNKED) != 0;
        if ((state->flags & F_CONTENT_LENGTH) != 0) {
            self.head_.content_length = state->content_length;
        }
        (*self.sink_)(event{std::move(self.head_)});
        return 0;
    }

    static int body(llhttp_t* const state, char const* const at, std::size_t const length) noexcept {
        (*from(state).sink_)(event{body_chunk{.bytes = std::as_bytes(std::span{at, length})}});
        return 0;
    }

    static int complete(llhttp_t* const state) noexcept {
        (*from(state).sink_)(event{message_complete{}});
        return 0;
    }

    void flush_pair() {
        if (in_value_) {
            head_.fields.add(std::move(field_), std::move(value_));
            field_.clear();
            value_.clear();
            in_value_ = false;
        }
    }

    std::unexpected<std::error_code> failure(llhttp_errno_t const outcome) {
        auto const* const reason = llhttp_get_error_reason(&state_);
        diagnostic_ = reason ? reason : "";
        llhttp_resume_after_upgrade(&state_);
        (void)outcome;
        return std::unexpected{make_error_code(limit_hit_ ? errc::head_too_large : errc::malformed_message)};
    }

    parser_limits limits_;
    llhttp_t state_;
    llhttp_settings_t settings_;
    event_sink const* sink_ = nullptr;
    message_head head_{};
    std::string field_;
    std::string value_;
    std::string diagnostic_;
    std::size_t head_size_ = 0;
    bool in_value_ = false;
    bool limit_hit_ = false;
};

}  // namespace llmgate::http

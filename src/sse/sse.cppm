export module llmgate.sse;

import std;

export namespace llmgate::sse {

struct event {
    std::string_view name;
    std::string_view data;
    std::string_view id;
};

class parser {
public:
    template <std::invocable<event const&> Sink>
    void feed(std::span<std::byte const> bytes, Sink&& on_event) {
        for (auto const byte : bytes) {
            consume(static_cast<char>(byte), on_event);
        }
    }

    template <std::invocable<event const&> Sink>
    void finish(Sink&& on_event) {
        if (!line_.empty()) {
            process_line(on_event);
        }
        dispatch(on_event);
    }

private:
    template <class Sink>
    void consume(char const c, Sink& on_event) {
        if (c == '\n' && after_carriage_return_) {
            after_carriage_return_ = false;
            return;
        }
        after_carriage_return_ = c == '\r';
        if (c == '\n' || c == '\r') {
            process_line(on_event);
            return;
        }
        line_.push_back(c);
    }

    template <class Sink>
    void process_line(Sink& on_event) {
        std::string_view const line{line_};
        if (line.empty()) {
            dispatch(on_event);
        } else if (line.front() != ':') {
            assign_field(line);
        }
        line_.clear();
    }

    void assign_field(std::string_view const line) {
        auto const colon = line.find(':');
        auto const name = line.substr(0, colon);
        auto value = colon == std::string_view::npos ? std::string_view{} : line.substr(colon + 1);
        if (value.starts_with(' ')) {
            value.remove_prefix(1);
        }
        if (name == "event") {
            name_ = value;
        } else if (name == "data") {
            data_.append(value);
            data_.push_back('\n');
            has_data_ = true;
        } else if (name == "id" && !value.contains('\0')) {
            id_ = value;
        }
    }

    template <class Sink>
    void dispatch(Sink& on_event) {
        if (has_data_) {
            data_.pop_back();
            on_event(event{.name = name_, .data = data_, .id = id_});
        }
        name_.clear();
        data_.clear();
        has_data_ = false;
    }

    std::string line_;
    std::string name_;
    std::string data_;
    std::string id_;
    bool has_data_ = false;
    bool after_carriage_return_ = false;
};

void write_event(std::string& out, std::string_view const name, std::string_view const data) {
    if (!name.empty()) {
        std::format_to(std::back_inserter(out), "event: {}\n", name);
    }
    std::string_view rest = data;
    for (;;) {
        auto const newline = rest.find('\n');
        std::format_to(std::back_inserter(out), "data: {}\n", rest.substr(0, newline));
        if (newline == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(newline + 1);
    }
    out.push_back('\n');
}

}  // namespace llmgate::sse

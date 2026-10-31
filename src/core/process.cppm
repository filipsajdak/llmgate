module;

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <meta>

#include <unistd.h>

#include "platform.h"
#include "sockets.h"

export module llmgate.process;

import std;
import llmgate.text;

namespace llmgate::commands {}

namespace llmgate::process::detail {

consteval bool within(std::meta::info at, std::meta::info const layer) {
    while (at != ^^::) {
        if (at == layer) {
            return true;
        }
        at = std::meta::parent_of(at);
    }
    return false;
}

consteval bool may_spawn(std::meta::info const caller) {
    return !within(caller, ^^llmgate) || within(caller, ^^llmgate::commands);
}

}  // namespace llmgate::process::detail

export namespace llmgate::process {

using std::chrono::milliseconds;
constexpr std::size_t read_chunk = 4096;
using header_list = std::vector<text::name_value>;

struct http_result {
    int status;
    std::string body;
};

using process_id = platform::process_id;

inline std::error_code errno_error() {
    return {errno, std::system_category()};
}

inline std::error_code write_all(int const fd, std::string_view const text) {
    for (auto pending = text; !pending.empty();) {
        auto const written = ::write(fd, pending.data(), pending.size());
        if (written > 0) {
            pending.remove_prefix(static_cast<std::size_t>(written));
        } else if (written < 0 && errno != EINTR) {
            return errno_error();
        }
    }
    return {};
}

inline std::expected<std::string, std::error_code> read_all(int const fd) {
    std::string collected;
    std::array<char, read_chunk> chunk{};
    for (;;) {
        auto const got = ::read(fd, chunk.data(), chunk.size());
        if (got > 0) {
            collected.append(chunk.data(), static_cast<std::size_t>(got));
        } else if (got == 0) {
            return collected;
        } else if (errno != EINTR) {
            return std::unexpected{errno_error()};
        }
    }
}

struct argument_vector {
    std::vector<std::string> storage;
    std::vector<char const*> pointers;
};

inline argument_vector make_argv(std::filesystem::path const& program, std::vector<std::string> const& arguments) {
    std::vector<std::string> storage;
    storage.emplace_back(program.string());
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    auto pointers = std::ranges::to<std::vector<char const*>>(storage | std::views::transform([](std::string const& item) { return item.c_str(); }));
    pointers.push_back(nullptr);
    return {.storage = std::move(storage), .pointers = std::move(pointers)};
}

inline std::string expand_home(std::string_view const path) {
    if (!path.starts_with("~/")) {
        return std::string{path};
    }
    auto const* home = platform::home_directory();
    return (home == nullptr ? std::string{} : std::string{home}) + std::string{path.substr(1)};
}

inline std::optional<std::filesystem::path> current_executable() {
    std::array<char, 4096> buffer{};
    auto const length = platform::executable_path(buffer.data(), buffer.size());
    if (length == 0) {
        return std::nullopt;
    }
    return std::filesystem::path{std::string_view{buffer.data(), length}};
}

inline std::optional<std::filesystem::path> search_path(std::string_view const name) {
    auto const* search = std::getenv("PATH");
    std::array<char, 4096> resolved{};
    for (auto const directory : std::string_view{search == nullptr ? "" : search} | std::views::split(platform::path_list_separator())) {
        auto const candidate = (std::filesystem::path{std::string_view{directory}} / name).string();
        if (platform::resolve_executable(candidate.c_str(), resolved.data(), resolved.size())) {
            return std::filesystem::path{resolved.data()};
        }
    }
    return std::nullopt;
}

inline std::optional<std::filesystem::path> find_in_path(std::string_view const name) {
    if (name.contains('/')) {
        return std::filesystem::exists(name) ? std::optional{std::filesystem::path{name}} : std::nullopt;
    }
    return search_path(name);
}

}  // namespace llmgate::process

namespace llmgate::process::detail {

bool wait_for(int const fd, bool const for_write, std::chrono::steady_clock::time_point const deadline) {
    for (;;) {
        auto const remaining = std::chrono::duration_cast<milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            return false;
        }
        auto const ready = platform::wait_socket(fd, for_write, static_cast<int>(remaining.count()));
        if (ready > 0) {
            return true;
        }
        if (ready == 0 || !platform::interrupted(platform::socket_error())) {
            return false;
        }
    }
}

struct descriptor {
    int fd;
    ~descriptor() {
        if (fd >= 0) {
            platform::close_socket(fd);
        }
    }
};

constexpr std::size_t status_begin = 9;
constexpr std::size_t status_end = 12;

std::optional<sockaddr_in> ipv4_address(std::string_view const host, std::uint16_t const port) {
    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(port);
    if (::inet_pton(AF_INET, std::string{host}.c_str(), &remote.sin_addr) != 1) {
        return std::nullopt;
    }
    return remote;
}

int nonblocking_socket() {
    int const fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    if (fd >= 0) {
        platform::set_nonblocking(fd);
    }
    return fd;
}

bool connect_completed(int const fd, std::chrono::steady_clock::time_point const deadline) {
    return wait_for(fd, true, deadline) && platform::pending_socket_error(fd) == 0;
}

bool connect_by(int const fd, sockaddr_in const& remote, std::chrono::steady_clock::time_point const deadline) {
    if (::connect(fd, reinterpret_cast<sockaddr const*>(&remote), sizeof remote) == 0) {
        return true;
    }
    auto const error = platform::socket_error();
    return (platform::in_progress(error) || platform::would_block(error)) && connect_completed(fd, deadline);
}

bool retry_after(int const fd, bool const for_write, std::chrono::steady_clock::time_point const deadline) {
    auto const error = platform::socket_error();
    return platform::interrupted(error) || (platform::would_block(error) && wait_for(fd, for_write, deadline));
}

bool send_all(int const fd, std::string_view pending, std::chrono::steady_clock::time_point const deadline) {
    while (!pending.empty()) {
        auto const sent = platform::send_bytes(fd, pending.data(), pending.size());
        if (sent > 0) {
            pending.remove_prefix(static_cast<std::size_t>(sent));
        } else if (sent < 0 && retry_after(fd, true, deadline)) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

std::optional<std::string> receive_all(int const fd, std::chrono::steady_clock::time_point const deadline) {
    std::string answer;
    std::array<char, 4096> buffer{};
    for (;;) {
        auto const received = platform::receive_bytes(fd, buffer.data(), buffer.size());
        if (received > 0) {
            answer.append(buffer.data(), static_cast<std::size_t>(received));
        } else if (received == 0) {
            return answer;
        } else if (!retry_after(fd, false, deadline)) {
            return std::nullopt;
        }
    }
}

std::string request_text(std::string_view const method, std::string_view const host, std::uint16_t const port, std::string_view const target,
                         header_list const& headers, std::string_view const body) {
    std::string request = std::format("{} {} HTTP/1.1\r\nHost: {}:{}\r\nConnection: close\r\n", method, target, host, port);
    for (auto const& [name, value] : headers) {
        std::format_to(std::back_inserter(request), "{}: {}\r\n", name, value);
    }
    if (method == "POST") {
        std::format_to(std::back_inserter(request), "Content-Type: application/json\r\nContent-Length: {}\r\n", body.size());
    }
    request += "\r\n";
    request += body;
    return request;
}

std::optional<int> status_of(std::string_view const answer) {
    if (!answer.starts_with("HTTP/1.") || answer.size() < status_end || answer[status_begin - 1] != ' ') {
        return std::nullopt;
    }
    int status = 0;
    auto const read = std::from_chars(answer.data() + status_begin, answer.data() + status_end, status);
    return read.ec == std::errc{} ? std::optional{status} : std::nullopt;
}

std::optional<http_result> parse_response(std::string_view const answer) {
    auto const split = answer.find("\r\n\r\n");
    auto const status = status_of(answer);
    if (split == std::string_view::npos || !status) {
        return std::nullopt;
    }
    return http_result{.status = *status, .body = std::string{answer.substr(split + 4)}};
}

void prepare_directory(std::filesystem::path const& directory, bool const private_parent) {
    std::error_code failure;
    if (std::filesystem::create_directories(directory, failure) && private_parent) {
        platform::make_directory_private(directory.string().c_str());
    }
}

void collect_output(void* const context, char const* const bytes, std::size_t const size) {
    static_cast<std::string*>(context)->append(bytes, size);
}

std::expected<void, std::error_code> from_code(int const code) {
    return code == 0 ? std::expected<void, std::error_code>{} : std::expected<void, std::error_code>{std::unexpected{std::error_code{code, std::system_category()}}};
}

}  // namespace llmgate::process::detail

export namespace llmgate::process {

inline std::optional<http_result> http_exchange(std::string_view const method, std::string_view const host, std::uint16_t const port,
                                                std::string_view const target, header_list const& headers, std::string_view const body, milliseconds const limit) {
    auto const deadline = std::chrono::steady_clock::now() + limit;
    auto const remote = detail::ipv4_address(host, port);
    if (!remote) {
        return std::nullopt;
    }
    detail::descriptor socket{detail::nonblocking_socket()};
    if (socket.fd < 0 || !detail::connect_by(socket.fd, *remote, deadline) ||
        !detail::send_all(socket.fd, detail::request_text(method, host, port, target, headers, body), deadline)) {
        return std::nullopt;
    }
    auto const answer = detail::receive_all(socket.fd, deadline);
    return answer ? detail::parse_response(*answer) : std::nullopt;
}

inline std::optional<http_result> http_get(std::string_view const host, std::uint16_t const port, std::string_view const target,
                                           header_list const& headers, milliseconds const limit) {
    return http_exchange("GET", host, port, target, headers, {}, limit);
}

inline std::optional<http_result> http_post(std::string_view const host, std::uint16_t const port, std::string_view const target,
                                            header_list const& headers, std::string_view const body, milliseconds const limit) {
    return http_exchange("POST", host, port, target, headers, body, limit);
}

inline bool tcp_connectable(std::string_view const host, std::uint16_t const port, milliseconds const limit) {
    auto const remote = detail::ipv4_address(host, port);
    if (!remote) {
        return false;
    }
    detail::descriptor socket{detail::nonblocking_socket()};
    return socket.fd >= 0 && detail::connect_by(socket.fd, *remote, std::chrono::steady_clock::now() + limit);
}

inline bool gateway_alive(std::string_view const host, std::uint16_t const port) {
    constexpr milliseconds probe_limit{800};
    auto const answer = http_get(host, port, "/healthz", {}, probe_limit);
    return answer && answer->status == 200;
}

template <std::meta::info Caller = std::meta::current_function()>
std::expected<process_id, std::error_code> start_detached(std::filesystem::path const& program, std::vector<std::string> const& arguments,
                                                         std::filesystem::path const& log_file) {
    static_assert(detail::may_spawn(Caller), "start_detached may only be called from llmgate::commands (src/app) or from code outside llmgate");
    std::error_code ignored;
    std::filesystem::create_directories(log_file.parent_path(), ignored);
    auto argv = make_argv(program, arguments);
    process_id child = 0;
    auto const result = platform::start_detached_process(program.string().c_str(), argv.pointers.data(), log_file.string().c_str(), &child);
    if (result != 0) {
        return std::unexpected{std::error_code{result, std::system_category()}};
    }
    return child;
}

inline std::expected<void, std::error_code> write_file_atomic(std::filesystem::path const& target, std::string_view const text,
                                                              unsigned const mode, bool const private_parent) {
    detail::prepare_directory(target.parent_path(), private_parent);
    return detail::from_code(platform::write_file_atomic(target.string().c_str(), text.data(), text.size(), mode));
}

inline std::expected<void, std::error_code> append_file(std::filesystem::path const& target, std::string_view const text, unsigned const mode,
                                                        bool const private_parent) {
    detail::prepare_directory(target.parent_path(), private_parent);
    return detail::from_code(platform::append_to_file(target.string().c_str(), text.data(), text.size(), mode));
}

inline int run_and_wait(std::filesystem::path const& program, std::vector<std::string> const& arguments) {
    auto argv = make_argv(program, arguments);
    return platform::run_process(program.string().c_str(), argv.pointers.data());
}

inline std::optional<std::filesystem::path> find_program(std::string const& name) {
    if (name.contains('/')) {
        return std::filesystem::path{expand_home(name)};
    }
    return search_path(name);
}

inline std::expected<std::string, std::error_code> run_capturing(std::filesystem::path const& program, std::vector<std::string> const& arguments) {
    auto argv = make_argv(program, arguments);
    std::string text;
    if (auto const failure = platform::capture_process(program.string().c_str(), argv.pointers.data(), &text, &detail::collect_output); failure != 0) {
        return std::unexpected{std::error_code{failure, std::system_category()}};
    }
    return text;
}

inline bool process_running(process_id const pid) noexcept {
    return platform::process_alive(pid);
}

template <std::invocable Condition>
bool wait_until(Condition&& condition, milliseconds const limit, milliseconds const step = milliseconds{100}) {
    auto const deadline = std::chrono::steady_clock::now() + limit;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(step);
    }
    return true;
}

inline void apply_environment(std::vector<text::name_value> const& environment) {
    for (auto const& [name, value] : environment) {
        platform::set_environment_variable(name.c_str(), value.c_str());
    }
}

[[noreturn]] inline void exec_program(std::filesystem::path const& program, std::vector<std::string> const& arguments,
                                      std::vector<text::name_value> const& environment) {
    apply_environment(environment);
    auto argv = make_argv(program, arguments);
    platform::replace_process(program.string().c_str(), argv.pointers.data());
}

}  // namespace llmgate::process

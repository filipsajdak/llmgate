module;

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <meta>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

export module llmgate.process;

import std;
import llmgate.text;

extern "C" char** environ;

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
    std::vector<char*> pointers;
};

inline argument_vector make_argv(std::filesystem::path const& program, std::vector<std::string> const& arguments) {
    std::vector<std::string> storage;
    storage.emplace_back(program.string());
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    auto pointers = std::ranges::to<std::vector<char*>>(storage | std::views::transform([](std::string& item) { return item.data(); }));
    pointers.push_back(nullptr);
    return {.storage = std::move(storage), .pointers = std::move(pointers)};
}

inline std::string expand_home(std::string_view const path) {
    if (!path.starts_with("~/")) {
        return std::string{path};
    }
    auto const* home = std::getenv("HOME");
    return (home == nullptr ? std::string{} : std::string{home}) + std::string{path.substr(1)};
}

inline std::optional<std::filesystem::path> current_executable() {
#if defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return std::nullopt;
    }
    std::error_code error;
    auto resolved = std::filesystem::canonical(buffer.c_str(), error);
    return error ? std::nullopt : std::optional{std::move(resolved)};
#else
    std::error_code error;
    auto resolved = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::nullopt : std::optional{std::move(resolved)};
#endif
}

inline std::optional<std::filesystem::path> find_in_path(std::string_view const name) {
    if (name.contains('/')) {
        return std::filesystem::exists(name) ? std::optional{std::filesystem::path{name}} : std::nullopt;
    }
    auto const* search = std::getenv("PATH");
    for (auto const directory : std::string_view{search == nullptr ? "" : search} | std::views::split(':')) {
        std::filesystem::path candidate = std::filesystem::path{std::string_view{directory}} / name;
        if (::access(candidate.c_str(), X_OK) == 0 && std::filesystem::is_regular_file(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

}  // namespace llmgate::process

namespace llmgate::process::detail {

bool wait_for(int const fd, short const events, std::chrono::steady_clock::time_point const deadline) {
    for (;;) {
        auto const remaining = std::chrono::duration_cast<milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            return false;
        }
        pollfd watch{.fd = fd, .events = events, .revents = 0};
        auto const ready = ::poll(&watch, 1, static_cast<int>(remaining.count()));
        if (ready > 0) {
            return true;
        }
        if (ready == 0 || errno != EINTR) {
            return false;
        }
    }
}

struct descriptor {
    int fd;
    ~descriptor() {
        if (fd >= 0) {
            ::close(fd);
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
    int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    }
    return fd;
}

bool connect_completed(int const fd, std::chrono::steady_clock::time_point const deadline) {
    int pending = 0;
    socklen_t length = sizeof pending;
    return wait_for(fd, POLLOUT, deadline) && ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &pending, &length) >= 0 && pending == 0;
}

bool connect_by(int const fd, sockaddr_in const& remote, std::chrono::steady_clock::time_point const deadline) {
    if (::connect(fd, reinterpret_cast<sockaddr const*>(&remote), sizeof remote) == 0) {
        return true;
    }
    return errno == EINPROGRESS && connect_completed(fd, deadline);
}

bool would_block() {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

bool retry_after(int const fd, short const events, std::chrono::steady_clock::time_point const deadline) {
    return errno == EINTR || (would_block() && wait_for(fd, events, deadline));
}

bool send_all(int const fd, std::string_view pending, std::chrono::steady_clock::time_point const deadline) {
    while (!pending.empty()) {
        auto const sent = ::send(fd, pending.data(), pending.size(), 0);
        if (sent > 0) {
            pending.remove_prefix(static_cast<std::size_t>(sent));
        } else if (sent < 0 && retry_after(fd, POLLOUT, deadline)) {
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
        auto const received = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (received > 0) {
            answer.append(buffer.data(), static_cast<std::size_t>(received));
        } else if (received == 0) {
            return answer;
        } else if (!retry_after(fd, POLLIN, deadline)) {
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

std::expected<void, std::error_code> as_result(std::error_code const error) {
    return error ? std::expected<void, std::error_code>{std::unexpected{error}} : std::expected<void, std::error_code>{};
}

void prepare_directory(std::filesystem::path const& directory, bool const private_parent) {
    std::error_code failure;
    if (std::filesystem::create_directories(directory, failure) && private_parent) {
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, failure);
    }
}

std::error_code write_synced(int const fd, std::string_view const text) {
    auto error = write_all(fd, text);
    if (!error && ::fsync(fd) < 0) {
        error = errno_error();
    }
    ::close(fd);
    return error;
}

std::error_code write_and_rename(int const fd, std::string_view const text, std::filesystem::path const& temporary,
                                 std::filesystem::path const& target) {
    auto error = write_synced(fd, text);
    if (!error && ::rename(temporary.c_str(), target.c_str()) < 0) {
        error = errno_error();
    }
    if (error) {
        ::unlink(temporary.c_str());
    }
    return error;
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
std::expected<pid_t, std::error_code> start_detached(std::filesystem::path const& program, std::vector<std::string> const& arguments,
                                                    std::filesystem::path const& log_file) {
    static_assert(detail::may_spawn(Caller), "start_detached may only be called from llmgate::commands (src/app) or from code outside llmgate");
    std::error_code ignored;
    std::filesystem::create_directories(log_file.parent_path(), ignored);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log_file.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
    auto argv = make_argv(program, arguments);
    pid_t child = 0;
    auto const result = ::posix_spawn(&child, program.c_str(), &actions, &attributes, argv.pointers.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (result != 0) {
        return std::unexpected{std::error_code{result, std::system_category()}};
    }
    return child;
}

inline std::expected<void, std::error_code> write_file_atomic(std::filesystem::path const& target, std::string_view const text,
                                                              mode_t const mode, bool const private_parent) {
    auto const directory = target.parent_path();
    detail::prepare_directory(directory, private_parent);
    auto const temporary = directory / std::format(".{}.{}.tmp", target.filename().string(), ::getpid());
    auto const descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
    if (descriptor < 0) {
        return std::unexpected{errno_error()};
    }
    return detail::as_result(detail::write_and_rename(descriptor, text, temporary, target));
}

inline std::expected<void, std::error_code> append_file(std::filesystem::path const& target, std::string_view const text, mode_t const mode,
                                                        bool const private_parent) {
    detail::prepare_directory(target.parent_path(), private_parent);
    auto const descriptor = ::open(target.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, mode);
    if (descriptor < 0) {
        return std::unexpected{errno_error()};
    }
    auto const error = write_all(descriptor, text);
    ::close(descriptor);
    return detail::as_result(error);
}

inline int wait_status(pid_t const child) {
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

inline int run_and_wait(std::filesystem::path const& program, std::vector<std::string> const& arguments) {
    auto argv = make_argv(program, arguments);
    pid_t child = 0;
    if (::posix_spawn(&child, program.c_str(), nullptr, nullptr, argv.pointers.data(), environ) != 0) {
        return -1;
    }
    return wait_status(child);
}

inline bool process_running(pid_t const pid) noexcept {
    return ::kill(pid, 0) == 0 || errno == EPERM;
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
        ::setenv(name.c_str(), value.c_str(), 1);
    }
}

[[noreturn]] inline void exec_program(std::filesystem::path const& program, std::vector<std::string> const& arguments,
                                      std::vector<text::name_value> const& environment) {
    apply_environment(environment);
    auto argv = make_argv(program, arguments);
    ::execv(program.c_str(), argv.pointers.data());
    std::println(stderr, "llmgate: cannot run {}: {}", program.string(), std::strerror(errno));
    std::_Exit(127);
}

}  // namespace llmgate::process

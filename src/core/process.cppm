module;

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>

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

extern "C" char** environ;

export namespace llmgate::process {

using std::chrono::milliseconds;
using header_list = std::vector<std::pair<std::string, std::string>>;

struct http_result {
    int status;
    std::string body;
};

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

}  // namespace llmgate::process::detail

export namespace llmgate::process {

inline std::optional<http_result> http_get(std::string_view const host, std::uint16_t const port, std::string_view const target,
                                           header_list const& headers, milliseconds const limit) {
    auto const deadline = std::chrono::steady_clock::now() + limit;
    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(port);
    if (::inet_pton(AF_INET, std::string{host}.c_str(), &remote.sin_addr) != 1) {
        return std::nullopt;
    }
    detail::descriptor socket{::socket(AF_INET, SOCK_STREAM, 0)};
    if (socket.fd < 0) {
        return std::nullopt;
    }
    ::fcntl(socket.fd, F_SETFL, ::fcntl(socket.fd, F_GETFL, 0) | O_NONBLOCK);
    if (::connect(socket.fd, reinterpret_cast<sockaddr const*>(&remote), sizeof remote) < 0) {
        int pending = 0;
        socklen_t length = sizeof pending;
        if (errno != EINPROGRESS || !detail::wait_for(socket.fd, POLLOUT, deadline) ||
            ::getsockopt(socket.fd, SOL_SOCKET, SO_ERROR, &pending, &length) < 0 || pending != 0) {
            return std::nullopt;
        }
    }
    std::string request = std::format("GET {} HTTP/1.1\r\nHost: {}:{}\r\nConnection: close\r\n", target, host, port);
    for (auto const& [name, value] : headers) {
        request += name + ": " + value + "\r\n";
    }
    request += "\r\n";
    std::string_view pending_bytes = request;
    while (!pending_bytes.empty()) {
        auto const sent = ::send(socket.fd, pending_bytes.data(), pending_bytes.size(), 0);
        if (sent > 0) {
            pending_bytes.remove_prefix(static_cast<std::size_t>(sent));
        } else if (sent < 0 && errno == EINTR) {
            continue;
        } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && detail::wait_for(socket.fd, POLLOUT, deadline)) {
            continue;
        } else {
            return std::nullopt;
        }
    }
    std::string answer;
    std::array<char, 4096> buffer{};
    for (;;) {
        auto const received = ::recv(socket.fd, buffer.data(), buffer.size(), 0);
        if (received > 0) {
            answer.append(buffer.data(), static_cast<std::size_t>(received));
        } else if (received == 0) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else if ((errno == EAGAIN || errno == EWOULDBLOCK) && detail::wait_for(socket.fd, POLLIN, deadline)) {
            continue;
        } else {
            return std::nullopt;
        }
    }
    auto const split = answer.find("\r\n\r\n");
    int status = 0;
    if (split == std::string::npos || !answer.starts_with("HTTP/1.") || answer.size() < 12 || answer[8] != ' ' ||
        std::from_chars(answer.data() + 9, answer.data() + std::min<std::size_t>(12, answer.size()), status).ec != std::errc{}) {
        return std::nullopt;
    }
    return http_result{.status = status, .body = answer.substr(split + 4)};
}

inline bool gateway_alive(std::string_view const host, std::uint16_t const port) {
    constexpr milliseconds probe_limit{800};
    auto const answer = http_get(host, port, "/healthz", {}, probe_limit);
    return answer && answer->status == 200;
}

inline std::expected<pid_t, std::error_code> start_detached(std::filesystem::path const& program, std::vector<std::string> const& arguments,
                                                           std::filesystem::path const& log_file) {
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
    std::vector<std::string> storage{program.string()};
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& item : storage) {
        argv.push_back(item.data());
    }
    argv.push_back(nullptr);
    pid_t child = 0;
    auto const result = ::posix_spawn(&child, program.c_str(), &actions, &attributes, argv.data(), environ);
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
    std::error_code failure;
    if (std::filesystem::create_directories(directory, failure) && private_parent) {
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, failure);
    }
    auto const temporary = directory / std::format(".{}.{}.tmp", target.filename().string(), ::getpid());
    auto const descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
    if (descriptor < 0) {
        return std::unexpected{std::error_code{errno, std::system_category()}};
    }
    std::error_code error;
    for (auto pending = text; !pending.empty() && !error;) {
        auto const written = ::write(descriptor, pending.data(), pending.size());
        if (written > 0) {
            pending.remove_prefix(static_cast<std::size_t>(written));
        } else if (written < 0 && errno != EINTR) {
            error = {errno, std::system_category()};
        }
    }
    if (!error && ::fsync(descriptor) < 0) {
        error = {errno, std::system_category()};
    }
    ::close(descriptor);
    if (!error && ::rename(temporary.c_str(), target.c_str()) < 0) {
        error = {errno, std::system_category()};
    }
    if (error) {
        ::unlink(temporary.c_str());
        return std::unexpected{error};
    }
    return {};
}

inline std::expected<void, std::error_code> append_file(std::filesystem::path const& target, std::string_view const text, mode_t const mode,
                                                        bool const private_parent) {
    std::error_code failure;
    if (std::filesystem::create_directories(target.parent_path(), failure) && private_parent) {
        std::filesystem::permissions(target.parent_path(), std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, failure);
    }
    auto const descriptor = ::open(target.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, mode);
    if (descriptor < 0) {
        return std::unexpected{std::error_code{errno, std::system_category()}};
    }
    std::error_code error;
    for (auto pending = text; !pending.empty() && !error;) {
        auto const written = ::write(descriptor, pending.data(), pending.size());
        if (written > 0) {
            pending.remove_prefix(static_cast<std::size_t>(written));
        } else if (written < 0 && errno != EINTR) {
            error = {errno, std::system_category()};
        }
    }
    ::close(descriptor);
    if (error) {
        return std::unexpected{error};
    }
    return {};
}

inline int run_and_wait(std::filesystem::path const& program, std::vector<std::string> const& arguments) {
    std::vector<std::string> storage{program.string()};
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& item : storage) {
        argv.push_back(item.data());
    }
    argv.push_back(nullptr);
    pid_t child = 0;
    if (::posix_spawn(&child, program.c_str(), nullptr, nullptr, argv.data(), environ) != 0) {
        return -1;
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
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

[[noreturn]] inline void exec_program(std::filesystem::path const& program, std::vector<std::string> const& arguments,
                                      std::vector<std::pair<std::string, std::string>> const& environment) {
    for (auto const& [name, value] : environment) {
        ::setenv(name.c_str(), value.c_str(), 1);
    }
    std::vector<std::string> storage{program.string()};
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& item : storage) {
        argv.push_back(item.data());
    }
    argv.push_back(nullptr);
    ::execv(program.c_str(), argv.data());
    std::println(stderr, "llmgate: cannot run {}: {}", program.string(), std::strerror(errno));
    std::_Exit(127);
}

}  // namespace llmgate::process

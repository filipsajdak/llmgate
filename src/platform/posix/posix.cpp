#include "platform.h"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <limits.h>
#include <mach-o/dyld.h>
#endif

extern "C" char** environ;

namespace llmgate::platform {

namespace {

constexpr size_t drain_chunk = 16;
constexpr size_t read_chunk = 4096;
constexpr unsigned mode_mask = 0777;
constexpr mode_t owner_only_directory = 0700;
constexpr int missing_program_status = 127;

termios saved_terminal{};
bool terminal_saved = false;

int write_everything(int const fd, char const* data, size_t size) noexcept {
    while (size > 0) {
        auto const written = ::write(fd, data, size);
        if (written > 0) {
            data += written;
            size -= static_cast<size_t>(written);
        } else if (written < 0 && errno != EINTR) {
            return errno;
        }
    }
    return 0;
}

int prepare_pipe_end(int const descriptor) noexcept {
    if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0 || ::fcntl(descriptor, F_SETFL, ::fcntl(descriptor, F_GETFL, 0) | O_NONBLOCK) < 0) {
        return errno;
    }
    return 0;
}

int wait_for_child(pid_t const child) noexcept {
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int read_everything(int const fd, void* context, output_sink const sink) noexcept {
    char chunk[read_chunk];
    for (;;) {
        auto const got = ::read(fd, chunk, sizeof chunk);
        if (got > 0) {
            sink(context, chunk, static_cast<size_t>(got));
        } else if (got == 0) {
            return 0;
        } else if (errno != EINTR) {
            return errno;
        }
    }
}

}  // namespace

int socket_error() noexcept {
    return errno;
}

bool would_block(int const error) noexcept {
    return error == EAGAIN || error == EWOULDBLOCK;
}

bool in_progress(int const error) noexcept {
    return error == EINPROGRESS;
}

bool interrupted(int const error) noexcept {
    return error == EINTR;
}

int close_socket(int const fd) noexcept {
    return ::close(fd);
}

int set_nonblocking(int const fd) noexcept {
    auto const flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return errno;
    }
    return 0;
}

int tune_tcp_socket(int const fd) noexcept {
    if (auto const failure = set_nonblocking(fd)) {
        return failure;
    }
    int const enabled = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof enabled);
#ifdef SO_NOSIGPIPE
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled);
#endif
    return 0;
}

void fail_fast_connect(int) noexcept {}

void allow_address_reuse(int const fd) noexcept {
    int const enabled = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof enabled);
}

long receive_bytes(int const fd, void* const into, size_t const size) noexcept {
    return ::recv(fd, into, size, 0);
}

long send_bytes(int const fd, void const* const from, size_t const size) noexcept {
#ifdef MSG_NOSIGNAL
    return ::send(fd, from, size, MSG_NOSIGNAL);
#else
    return ::send(fd, from, size, 0);
#endif
}

long peek_byte(int const fd, bool const nonblocking) noexcept {
    char probe = 0;
    return ::recv(fd, &probe, 1, MSG_PEEK | (nonblocking ? MSG_DONTWAIT : 0));
}

int pending_socket_error(int const fd) noexcept {
    int pending = 0;
    socklen_t length = sizeof pending;
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &pending, &length);
    return pending;
}

int wait_socket(int const fd, bool const for_write, int const timeout_ms) noexcept {
    pollfd watch{.fd = fd, .events = static_cast<short>(for_write ? POLLOUT : POLLIN), .revents = 0};
    return ::poll(&watch, 1, timeout_ms);
}

int check_descriptor(int const fd) noexcept {
    return ::fcntl(fd, F_GETFD) < 0 ? errno : 0;
}

wake_channel open_wake_channel() noexcept {
    int ends[2]{};
    if (::pipe(ends) < 0) {
        return {.read_end = -1, .write_end = -1, .error = errno};
    }
    for (auto const descriptor : ends) {
        if (auto const failure = prepare_pipe_end(descriptor)) {
            ::close(ends[0]);
            ::close(ends[1]);
            return {.read_end = -1, .write_end = -1, .error = failure};
        }
    }
    return {.read_end = ends[0], .write_end = ends[1], .error = 0};
}

void signal_wake(int const write_end) noexcept {
    char const byte = 1;
    (void)::write(write_end, &byte, 1);
}

void clear_wake(int const read_end) noexcept {
    char sink[drain_chunk];
    while (::read(read_end, sink, sizeof sink) > 0) {
    }
}

void close_wake_end(int const end) noexcept {
    if (end >= 0) {
        ::close(end);
    }
}

int install_stop_handler(stop_handler const handler) noexcept {
    struct sigaction action{};
    action.sa_handler = handler;
    action.sa_flags = SA_RESTART;
    for (auto const number : {SIGINT, SIGTERM}) {
        if (::sigaction(number, &action, nullptr) < 0) {
            auto const failure = errno;
            remove_stop_handler();
            return failure;
        }
    }
    return 0;
}

void remove_stop_handler() noexcept {
    struct sigaction restore{};
    restore.sa_handler = SIG_DFL;
    for (auto const number : {SIGINT, SIGTERM}) {
        ::sigaction(number, &restore, nullptr);
    }
}

[[noreturn]] void exit_immediately(int const status) noexcept {
    ::_exit(status);
}

int start_detached_process(char const* const program, char const* const* const argv, char const* const log_file, process_id* const started) noexcept {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log_file, O_WRONLY | O_CREAT | O_APPEND, 0600);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
    pid_t child = 0;
    auto const result = ::posix_spawn(&child, program, &actions, &attributes, const_cast<char* const*>(argv), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (result == 0) {
        *started = child;
    }
    return result;
}

int run_process(char const* const program, char const* const* const argv) noexcept {
    pid_t child = 0;
    if (::posix_spawn(&child, program, nullptr, nullptr, const_cast<char* const*>(argv), environ) != 0) {
        return -1;
    }
    return wait_for_child(child);
}

int capture_process(char const* const program, char const* const* const argv, void* const context, output_sink const sink) noexcept {
    int ends[2]{};
    if (::pipe(ends) != 0) {
        return errno;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, ends[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&actions, ends[0]);
    posix_spawn_file_actions_addclose(&actions, ends[1]);
    pid_t child = 0;
    auto const result = ::posix_spawn(&child, program, &actions, nullptr, const_cast<char* const*>(argv), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(ends[1]);
    if (result != 0) {
        ::close(ends[0]);
        return result;
    }
    auto const read = read_everything(ends[0], context, sink);
    ::close(ends[0]);
    wait_for_child(child);
    return read;
}

bool process_alive(process_id const pid) noexcept {
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
}

int stop_process(process_id const pid) noexcept {
    return ::kill(static_cast<pid_t>(pid), SIGTERM) == 0 ? 0 : errno;
}

[[noreturn]] void replace_process(char const* const program, char const* const* const argv) noexcept {
    ::execv(program, const_cast<char* const*>(argv));
    std::fprintf(stderr, "llmgate: cannot run %s: %s\n", program, std::strerror(errno));
    std::_Exit(missing_program_status);
}

void set_environment_variable(char const* const name, char const* const value) noexcept {
    ::setenv(name, value, 1);
}

char const* home_directory() noexcept {
    return std::getenv("HOME");
}

size_t executable_path(char* const buffer, size_t const capacity) noexcept {
#if defined(__APPLE__)
    std::uint32_t size = static_cast<std::uint32_t>(capacity);
    char raw[PATH_MAX];
    std::uint32_t raw_size = sizeof raw;
    if (_NSGetExecutablePath(raw, &raw_size) != 0 || ::realpath(raw, buffer) == nullptr) {
        return 0;
    }
    (void)size;
    return std::strlen(buffer);
#else
    auto const length = ::readlink("/proc/self/exe", buffer, capacity - 1);
    if (length < 0) {
        return 0;
    }
    buffer[length] = '\0';
    return static_cast<size_t>(length);
#endif
}

bool resolve_executable(char const* const candidate, char* const resolved, size_t const capacity) noexcept {
    struct stat info{};
    if (::access(candidate, X_OK) != 0 || ::stat(candidate, &info) != 0 || !S_ISREG(info.st_mode) || std::strlen(candidate) >= capacity) {
        return false;
    }
    std::strcpy(resolved, candidate);
    return true;
}

char path_list_separator() noexcept {
    return ':';
}

int write_file_atomic(char const* const target, char const* const data, size_t const size, unsigned const mode) noexcept {
    std::string const path{target};
    auto const slash = path.find_last_of('/');
    std::string const directory = slash == std::string::npos ? std::string{"."} : path.substr(0, slash);
    std::string const name = slash == std::string::npos ? path : path.substr(slash + 1);
    std::string const temporary = directory + "/." + name + "." + std::to_string(::getpid()) + ".tmp";
    auto const descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, static_cast<mode_t>(mode));
    if (descriptor < 0) {
        return errno;
    }
    auto error = write_everything(descriptor, data, size);
    if (!error && ::fsync(descriptor) < 0) {
        error = errno;
    }
    ::close(descriptor);
    if (!error && ::rename(temporary.c_str(), target) < 0) {
        error = errno;
    }
    if (error) {
        ::unlink(temporary.c_str());
    }
    return error;
}

int append_to_file(char const* const target, char const* const data, size_t const size, unsigned const mode) noexcept {
    auto const descriptor = ::open(target, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, static_cast<mode_t>(mode));
    if (descriptor < 0) {
        return errno;
    }
    auto const error = write_everything(descriptor, data, size);
    ::close(descriptor);
    return error;
}

int file_mode(char const* const path, unsigned* const mode) noexcept {
    struct stat info{};
    if (::stat(path, &info) != 0) {
        return errno;
    }
    *mode = static_cast<unsigned>(info.st_mode) & mode_mask;
    return 0;
}

bool can_write_path(char const* const path) noexcept {
    return ::access(path, W_OK) == 0;
}

int make_directory_private(char const* const path) noexcept {
    return ::chmod(path, owner_only_directory) == 0 ? 0 : errno;
}

long current_user_id() noexcept {
    return static_cast<long>(::getuid());
}

long current_process_id() noexcept {
    return static_cast<long>(::getpid());
}

bool stdin_is_terminal() noexcept {
    return ::isatty(STDIN_FILENO) != 0;
}

bool disable_stdin_echo() noexcept {
    if (::tcgetattr(STDIN_FILENO, &saved_terminal) != 0) {
        return false;
    }
    termios hidden = saved_terminal;
    hidden.c_lflag &= ~static_cast<tcflag_t>(ECHO);
    terminal_saved = ::tcsetattr(STDIN_FILENO, TCSANOW, &hidden) == 0;
    return terminal_saved;
}

void restore_stdin_echo() noexcept {
    if (terminal_saved) {
        ::tcsetattr(STDIN_FILENO, TCSANOW, &saved_terminal);
        terminal_saved = false;
    }
}

}  // namespace llmgate::platform

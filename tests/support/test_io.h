#pragma once

#include <stddef.h>
#include <stdlib.h>

#include "platform.h"
#include "sockets.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace test_io {

inline bool open_pair(int* const fds) {
#if defined(_WIN32)
    auto const channel = llmgate::platform::open_wake_channel();
    fds[0] = channel.read_end;
    fds[1] = channel.write_end;
    return channel.error == 0;
#else
    return ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0;
#endif
}

inline long write_fd(int const fd, void const* const data, size_t const size) {
#if defined(_WIN32)
    return ::send(static_cast<SOCKET>(fd), static_cast<char const*>(data), static_cast<int>(size), 0);
#else
    return ::write(fd, data, size);
#endif
}

inline long read_fd(int const fd, void* const data, size_t const size) {
#if defined(_WIN32)
    return ::recv(static_cast<SOCKET>(fd), static_cast<char*>(data), static_cast<int>(size), 0);
#else
    return ::read(fd, data, size);
#endif
}

inline void close_fd(int const fd) {
    llmgate::platform::close_socket(fd);
}

inline void make_nonblocking(int const fd) {
    llmgate::platform::set_nonblocking(fd);
}

inline void set_buffers(int const fd, int const bytes) {
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char const*>(&bytes), sizeof bytes);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char const*>(&bytes), sizeof bytes);
}

inline bool nodelay_enabled(int const fd) {
    int value = 0;
    socklen_t size = sizeof value;
    return ::getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&value), &size) == 0 && value != 0;
}

inline int duplicate_fd(int const fd) {
#if defined(_WIN32)
    HANDLE copy = nullptr;
    return DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(static_cast<SOCKET>(fd)), GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS)
               ? static_cast<int>(reinterpret_cast<SOCKET>(copy))
               : -1;
#else
    return ::dup(fd);
#endif
}

inline void set_env(char const* const name, char const* const value) {
    llmgate::platform::set_environment_variable(name, value);
}

inline void unset_env(char const* const name) {
#if defined(_WIN32)
    _putenv_s(name, "");
#else
    ::unsetenv(name);
#endif
}

}  // namespace test_io

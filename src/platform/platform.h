#pragma once

#include <stddef.h>

namespace llmgate::platform {

using process_id = long;
using stop_handler = void (*)(int) noexcept;
using output_sink = void (*)(void* context, char const* bytes, size_t size);

struct wake_channel {
    int read_end;
    int write_end;
    int error;
};

int socket_error() noexcept;
bool would_block(int error) noexcept;
bool in_progress(int error) noexcept;
bool interrupted(int error) noexcept;
int close_socket(int fd) noexcept;
int set_nonblocking(int fd) noexcept;
int tune_tcp_socket(int fd) noexcept;
void fail_fast_connect(int fd) noexcept;
void allow_address_reuse(int fd) noexcept;
long receive_bytes(int fd, void* into, size_t size) noexcept;
long send_bytes(int fd, void const* from, size_t size) noexcept;
long peek_byte(int fd, bool nonblocking) noexcept;
int pending_socket_error(int fd) noexcept;
int wait_socket(int fd, bool for_write, int timeout_ms) noexcept;
int check_descriptor(int fd) noexcept;

wake_channel open_wake_channel() noexcept;
void signal_wake(int write_end) noexcept;
void clear_wake(int read_end) noexcept;
void close_wake_end(int end) noexcept;
int install_stop_handler(stop_handler handler) noexcept;
void remove_stop_handler() noexcept;
[[noreturn]] void exit_immediately(int status) noexcept;

int start_detached_process(char const* program, char const* const* argv, char const* log_file, process_id* started) noexcept;
int run_process(char const* program, char const* const* argv) noexcept;
int capture_process(char const* program, char const* const* argv, void* context, output_sink sink) noexcept;
bool process_alive(process_id pid) noexcept;
int stop_process(process_id pid) noexcept;
[[noreturn]] void replace_process(char const* program, char const* const* argv) noexcept;
void set_environment_variable(char const* name, char const* value) noexcept;
char const* home_directory() noexcept;
size_t executable_path(char* buffer, size_t capacity) noexcept;
bool resolve_executable(char const* candidate, char* resolved, size_t capacity) noexcept;
char path_list_separator() noexcept;

int write_file_atomic(char const* target, char const* data, size_t size, unsigned mode) noexcept;
int append_to_file(char const* target, char const* data, size_t size, unsigned mode) noexcept;
int file_mode(char const* path, unsigned* mode) noexcept;
bool can_write_path(char const* path) noexcept;
int make_directory_private(char const* path) noexcept;

long current_user_id() noexcept;
long current_process_id() noexcept;
bool stdin_is_terminal() noexcept;
bool disable_stdin_echo() noexcept;
void restore_stdin_echo() noexcept;

}  // namespace llmgate::platform

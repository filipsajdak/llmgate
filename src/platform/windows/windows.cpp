#include "platform.h"
#include "sockets.h"

#include <windows.h>
#include <mstcpip.h>

#include <aclapi.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sddl.h>
#include <wincrypt.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace llmgate::platform {

namespace {

constexpr unsigned private_mask = 077;
constexpr unsigned private_file_mode = 0600;
constexpr unsigned open_file_mode = 0666;
constexpr unsigned private_directory_mode = 0700;
constexpr unsigned open_directory_mode = 0777;
constexpr int wsa_major = 2;
constexpr int wsa_minor = 2;
constexpr DWORD wait_for_exit = INFINITE;
constexpr DWORD pipe_buffer_bytes = 65536;
constexpr int max_drain_rounds = 64;

std::atomic<stop_handler> installed_handler{nullptr};
DWORD saved_console_mode = 0;
bool console_mode_saved = false;
std::string home_storage;

struct runtime_setup {
    runtime_setup() {
        WSADATA data{};
        WSAStartup(MAKEWORD(wsa_major, wsa_minor), &data);
        SetConsoleOutputCP(CP_UTF8);
        for (auto const stream : {stdin, stdout, stderr}) {
            _setmode(_fileno(stream), _O_BINARY);
        }
        for (auto const handle : {GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE)}) {
            DWORD mode = 0;
            if (handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode)) {
                SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
            }
        }
    }
} const setup_once;

std::wstring widen(char const* const text) {
    if (text == nullptr || *text == '\0') {
        return {};
    }
    auto const length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring out(static_cast<size_t>(length > 0 ? length - 1 : 0), L'\0');
    if (length > 1) {
        MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), length);
    }
    return out;
}

std::string narrow(std::wstring const& text) {
    if (text.empty()) {
        return {};
    }
    auto const length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(length > 0 ? length - 1 : 0), '\0');
    if (length > 1) {
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), length, nullptr, nullptr);
    }
    return out;
}

void append_quoted(std::wstring& line, std::wstring const& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        line += argument;
        return;
    }
    line += L'"';
    for (auto at = argument.begin();; ++at) {
        size_t backslashes = 0;
        while (at != argument.end() && *at == L'\\') {
            ++at;
            ++backslashes;
        }
        if (at == argument.end()) {
            line.append(backslashes * 2, L'\\');
            break;
        }
        if (*at == L'"') {
            line.append(backslashes * 2 + 1, L'\\');
            line += L'"';
        } else {
            line.append(backslashes, L'\\');
            line += *at;
        }
    }
    line += L'"';
}

std::wstring command_line(char const* const* const argv) {
    std::wstring line;
    for (size_t index = 0; argv[index] != nullptr; ++index) {
        if (index != 0) {
            line += L' ';
        }
        append_quoted(line, widen(argv[index]));
    }
    return line;
}

struct handle_guard {
    HANDLE handle = INVALID_HANDLE_VALUE;
    handle_guard() = default;
    explicit handle_guard(HANDLE value) : handle{value} {}
    handle_guard(handle_guard const&) = delete;
    handle_guard& operator=(handle_guard const&) = delete;
    ~handle_guard() {
        if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
            CloseHandle(handle);
        }
    }
    HANDLE release() {
        auto const out = handle;
        handle = INVALID_HANDLE_VALUE;
        return out;
    }
};

struct owner_only_access {
    std::vector<unsigned char> token_buffer;
    PACL acl = nullptr;
    SECURITY_DESCRIPTOR descriptor{};
    SECURITY_ATTRIBUTES attributes{};

    owner_only_access() = default;
    owner_only_access(owner_only_access const&) = delete;
    owner_only_access& operator=(owner_only_access const&) = delete;
    ~owner_only_access() {
        if (acl != nullptr) {
            LocalFree(acl);
        }
    }

    bool build(bool const inherit) {
        HANDLE raw_token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
            return false;
        }
        handle_guard token{raw_token};
        DWORD size = 0;
        GetTokenInformation(token.handle, TokenUser, nullptr, 0, &size);
        token_buffer.resize(size);
        if (!GetTokenInformation(token.handle, TokenUser, token_buffer.data(), size, &size)) {
            return false;
        }
        auto const* const user = reinterpret_cast<TOKEN_USER const*>(token_buffer.data());
        EXPLICIT_ACCESSW entry{};
        entry.grfAccessPermissions = GENERIC_ALL;
        entry.grfAccessMode = SET_ACCESS;
        entry.grfInheritance = inherit ? (SUB_CONTAINERS_AND_OBJECTS_INHERIT) : NO_INHERITANCE;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entry.Trustee.TrusteeType = TRUSTEE_IS_USER;
        entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(user->User.Sid);
        if (SetEntriesInAclW(1, &entry, nullptr, &acl) != ERROR_SUCCESS) {
            return false;
        }
        if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) || !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) ||
            !SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
            return false;
        }
        attributes.nLength = sizeof attributes;
        attributes.lpSecurityDescriptor = &descriptor;
        attributes.bInheritHandle = FALSE;
        return true;
    }
};

int last_win_error() {
    return static_cast<int>(GetLastError());
}

int write_whole(HANDLE const file, char const* data, size_t size) {
    while (size > 0) {
        DWORD written = 0;
        auto const chunk = static_cast<DWORD>(size > 0x40000000 ? 0x40000000 : size);
        if (!WriteFile(file, data, chunk, &written, nullptr)) {
            return last_win_error();
        }
        data += written;
        size -= written;
    }
    return 0;
}

std::wstring temporary_name(std::wstring const& target) {
    auto const slash = target.find_last_of(L"\\/");
    auto const directory = slash == std::wstring::npos ? std::wstring{} : target.substr(0, slash + 1);
    auto const name = slash == std::wstring::npos ? target : target.substr(slash + 1);
    return directory + L"." + name + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
}

bool wants_private(unsigned const mode) {
    return (mode & private_mask) == 0;
}

SOCKET loopback_listener(sockaddr_in& local) {
    SOCKET listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET) {
        return listener;
    }
    local = {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int length = sizeof local;
    if (bind(listener, reinterpret_cast<sockaddr*>(&local), sizeof local) != 0 || listen(listener, 1) != 0 ||
        getsockname(listener, reinterpret_cast<sockaddr*>(&local), &length) != 0) {
        auto const failure = WSAGetLastError();
        closesocket(listener);
        WSASetLastError(failure);
        return INVALID_SOCKET;
    }
    return listener;
}

bool same_connection(SOCKET const client, SOCKET const server) {
    sockaddr_in mine{};
    sockaddr_in theirs{};
    int mine_length = sizeof mine;
    int theirs_length = sizeof theirs;
    getsockname(client, reinterpret_cast<sockaddr*>(&mine), &mine_length);
    getpeername(server, reinterpret_cast<sockaddr*>(&theirs), &theirs_length);
    return mine.sin_port == theirs.sin_port;
}

void close_if_open(SOCKET const handle) {
    if (handle != INVALID_SOCKET) {
        closesocket(handle);
    }
}

int create_pair(int* const read_end, int* const write_end) {
    sockaddr_in local{};
    SOCKET const listener = loopback_listener(local);
    if (listener == INVALID_SOCKET) {
        return WSAGetLastError();
    }
    SOCKET const client = socket(AF_INET, SOCK_STREAM, 0);
    bool const connected = client != INVALID_SOCKET && connect(client, reinterpret_cast<sockaddr*>(&local), sizeof local) == 0;
    SOCKET const server = connected ? accept(listener, nullptr, nullptr) : INVALID_SOCKET;
    int failure = WSAGetLastError();
    closesocket(listener);
    if (server == INVALID_SOCKET || !same_connection(client, server)) {
        close_if_open(client);
        close_if_open(server);
        return server == INVALID_SOCKET ? failure : WSAECONNABORTED;
    }
    *read_end = static_cast<int>(server);
    *write_end = static_cast<int>(client);
    return 0;
}

BOOL WINAPI console_event(DWORD const event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT && event != CTRL_CLOSE_EVENT) {
        return FALSE;
    }
    if (auto const handler = installed_handler.load()) {
        handler(0);
    }
    return TRUE;
}

int create_child(char const* const program, char const* const* const argv, STARTUPINFOW& startup, DWORD const flags, PROCESS_INFORMATION& info) {
    auto application = widen(program);
    auto line = command_line(argv);
    if (!CreateProcessW(application.c_str(), line.data(), nullptr, nullptr, TRUE, flags | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup, &info)) {
        return last_win_error();
    }
    return 0;
}

HANDLE open_null(DWORD const access) {
    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    return CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

bool is_regular_file(std::wstring const& path) {
    auto const attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::vector<unsigned char> current_user_token() {
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
        return {};
    }
    handle_guard token{raw_token};
    DWORD size = 0;
    GetTokenInformation(token.handle, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> buffer(size);
    if (!GetTokenInformation(token.handle, TokenUser, buffer.data(), size, &size)) {
        return {};
    }
    return buffer;
}

bool trusted_allowance(void* const ace, PSID const user) {
    auto const* const header = static_cast<ACE_HEADER const*>(ace);
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
        return true;
    }
    auto* const sid = reinterpret_cast<PSID>(&static_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart);
    return EqualSid(sid, user) != 0 || IsWellKnownSid(sid, WinLocalSystemSid) != 0 || IsWellKnownSid(sid, WinBuiltinAdministratorsSid) != 0;
}

bool acl_is_private(std::wstring const& path) {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR security = nullptr;
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &security) != ERROR_SUCCESS) {
        return false;
    }
    auto const token = current_user_token();
    bool privately = !token.empty() && dacl != nullptr;
    PSID const user = privately ? reinterpret_cast<TOKEN_USER const*>(token.data())->User.Sid : nullptr;
    for (DWORD index = 0; privately && index < dacl->AceCount; ++index) {
        void* ace = nullptr;
        privately = GetAce(dacl, index, &ace) != 0 && trusted_allowance(ace, user);
    }
    LocalFree(security);
    return privately;
}

std::vector<std::wstring> executable_attempts(std::wstring const& base) {
    auto const* extensions_text = std::getenv("PATHEXT");
    auto const extensions = widen(extensions_text == nullptr ? ".COM;.EXE;.BAT;.CMD" : extensions_text);
    std::vector<std::wstring> attempts;
    if (base.find_last_of(L'.') != std::wstring::npos) {
        attempts.push_back(base);
    }
    for (size_t begin = 0; begin <= extensions.size();) {
        auto const end = extensions.find(L';', begin);
        auto const piece = extensions.substr(begin, end == std::wstring::npos ? std::wstring::npos : end - begin);
        if (!piece.empty()) {
            attempts.push_back(base + piece);
        }
        begin = end == std::wstring::npos ? extensions.size() + 1 : end + 1;
    }
    return attempts;
}

bool copy_out(std::string const& text, char* const resolved, size_t const capacity) {
    if (text.size() >= capacity) {
        return false;
    }
    std::memcpy(resolved, text.c_str(), text.size() + 1);
    return true;
}

int write_new_file(std::wstring const& path, SECURITY_ATTRIBUTES* const attributes, char const* const data, size_t const size) {
    handle_guard file{CreateFileW(path.c_str(), GENERIC_WRITE, 0, attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.handle == INVALID_HANDLE_VALUE) {
        return last_win_error();
    }
    auto error = write_whole(file.handle, data, size);
    if (error == 0 && !FlushFileBuffers(file.handle)) {
        error = last_win_error();
    }
    CloseHandle(file.release());
    return error;
}

HANDLE open_for_append(std::wstring const& path, SECURITY_ATTRIBUTES* const attributes) {
    return CreateFileW(path.c_str(), FILE_APPEND_DATA | READ_CONTROL | WRITE_DAC, FILE_SHARE_READ | FILE_SHARE_WRITE, attributes, OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
}

int check_appendable(HANDLE const file, PACL const owner_only) {
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(file, &information)) {
        return last_win_error();
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return ERROR_CANT_ACCESS_FILE;
    }
    if (owner_only != nullptr && SetSecurityInfo(file, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, owner_only, nullptr) !=
                                     ERROR_SUCCESS) {
        return last_win_error();
    }
    return 0;
}

}  // namespace

int socket_error() noexcept {
    return WSAGetLastError();
}

bool would_block(int const error) noexcept {
    return error == WSAEWOULDBLOCK;
}

bool in_progress(int const error) noexcept {
    return error == WSAEINPROGRESS || error == WSAEWOULDBLOCK;
}

bool interrupted(int const error) noexcept {
    return error == WSAEINTR;
}

int close_socket(int const fd) noexcept {
    auto const handle = static_cast<SOCKET>(fd);
    shutdown(handle, SD_SEND);
    u_long enabled = 1;
    ioctlsocket(handle, FIONBIO, &enabled);
    char sink[4096];
    for (int round = 0; round < max_drain_rounds && recv(handle, sink, sizeof sink, 0) > 0; ++round) {
    }
    return closesocket(handle);
}

int set_nonblocking(int const fd) noexcept {
    u_long enabled = 1;
    return ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &enabled) == 0 ? 0 : WSAGetLastError();
}

int tune_tcp_socket(int const fd) noexcept {
    if (auto const failure = set_nonblocking(fd)) {
        return failure;
    }
    BOOL const enabled = TRUE;
    setsockopt(static_cast<SOCKET>(fd), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char const*>(&enabled), sizeof enabled);
    return 0;
}

void fail_fast_connect(int const fd) noexcept {
    struct initial_rto {
        USHORT rtt;
        UCHAR max_syn_retransmissions;
    };
    constexpr UCHAR no_syn_retransmissions = 0xFE;
    constexpr DWORD control_code = _WSAIOW(IOC_VENDOR, 17);
    initial_rto const parameters{.rtt = 0, .max_syn_retransmissions = no_syn_retransmissions};
    DWORD returned = 0;
    WSAIoctl(static_cast<SOCKET>(fd), control_code, const_cast<initial_rto*>(&parameters), sizeof parameters, nullptr, 0, &returned, nullptr, nullptr);
}

void allow_address_reuse(int) noexcept {}

long receive_bytes(int const fd, void* const into, size_t const size) noexcept {
    return recv(static_cast<SOCKET>(fd), static_cast<char*>(into), static_cast<int>(size), 0);
}

long send_bytes(int const fd, void const* const from, size_t const size) noexcept {
    return send(static_cast<SOCKET>(fd), static_cast<char const*>(from), static_cast<int>(size), 0);
}

long peek_byte(int const fd, bool) noexcept {
    char probe = 0;
    return recv(static_cast<SOCKET>(fd), &probe, 1, MSG_PEEK);
}

int pending_socket_error(int const fd) noexcept {
    int pending = 0;
    int length = sizeof pending;
    getsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&pending), &length);
    return pending;
}

int wait_socket(int const fd, bool const for_write, int const timeout_ms) noexcept {
    WSAPOLLFD watch{};
    watch.fd = static_cast<SOCKET>(fd);
    watch.events = for_write ? POLLWRNORM : POLLRDNORM;
    auto const ready = WSAPoll(&watch, 1, timeout_ms);
    return ready == SOCKET_ERROR ? -1 : ready;
}

int check_descriptor(int const fd) noexcept {
    int type = 0;
    int length = sizeof type;
    return getsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &length) == 0 ? 0 : WSAGetLastError();
}

wake_channel open_wake_channel() noexcept {
    int read_end = -1;
    int write_end = -1;
    if (auto const failure = create_pair(&read_end, &write_end)) {
        return {.read_end = -1, .write_end = -1, .error = failure};
    }
    for (auto const end : {read_end, write_end}) {
        tune_tcp_socket(end);
    }
    return {.read_end = read_end, .write_end = write_end, .error = 0};
}

void signal_wake(int const write_end) noexcept {
    char const byte = 1;
    send(static_cast<SOCKET>(write_end), &byte, 1, 0);
}

void clear_wake(int const read_end) noexcept {
    char sink[16];
    while (recv(static_cast<SOCKET>(read_end), sink, sizeof sink, 0) > 0) {
    }
}

void close_wake_end(int const end) noexcept {
    if (end >= 0) {
        closesocket(static_cast<SOCKET>(end));
    }
}

int install_stop_handler(stop_handler const handler) noexcept {
    installed_handler.store(handler);
    return SetConsoleCtrlHandler(console_event, TRUE) ? 0 : last_win_error();
}

void remove_stop_handler() noexcept {
    SetConsoleCtrlHandler(console_event, FALSE);
    installed_handler.store(nullptr);
}

[[noreturn]] void exit_immediately(int const status) noexcept {
    ::_exit(status);
}

int start_detached_process(char const* const program, char const* const* const argv, char const* const log_file, process_id* const started) noexcept {
    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    owner_only_access privately;
    privately.build(false);
    privately.attributes.bInheritHandle = TRUE;
    handle_guard log{CreateFileW(widen(log_file).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 privately.acl != nullptr ? &privately.attributes : &inherit, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    handle_guard input{open_null(GENERIC_READ)};
    if (log.handle == INVALID_HANDLE_VALUE || input.handle == INVALID_HANDLE_VALUE) {
        return last_win_error();
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input.handle;
    startup.hStdOutput = log.handle;
    startup.hStdError = log.handle;
    PROCESS_INFORMATION info{};
    if (auto const failure = create_child(program, argv, startup, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW, info)) {
        return failure;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    *started = static_cast<process_id>(info.dwProcessId);
    return 0;
}

int run_process(char const* const program, char const* const* const argv) noexcept {
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION info{};
    if (create_child(program, argv, startup, 0, info) != 0) {
        return -1;
    }
    CloseHandle(info.hThread);
    handle_guard process{info.hProcess};
    WaitForSingleObject(process.handle, wait_for_exit);
    DWORD code = 0;
    return GetExitCodeProcess(process.handle, &code) ? static_cast<int>(code) : -1;
}

int capture_process(char const* const program, char const* const* const argv, void* const context, output_sink const sink) noexcept {
    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    HANDLE raw_read = nullptr;
    HANDLE raw_write = nullptr;
    if (!CreatePipe(&raw_read, &raw_write, &inherit, pipe_buffer_bytes)) {
        return last_win_error();
    }
    handle_guard read_end{raw_read};
    handle_guard write_end{raw_write};
    SetHandleInformation(read_end.handle, HANDLE_FLAG_INHERIT, 0);
    handle_guard input{open_null(GENERIC_READ)};
    handle_guard errors{open_null(GENERIC_WRITE)};
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input.handle;
    startup.hStdOutput = write_end.handle;
    startup.hStdError = errors.handle;
    PROCESS_INFORMATION info{};
    if (auto const failure = create_child(program, argv, startup, CREATE_NO_WINDOW, info)) {
        return failure;
    }
    CloseHandle(info.hThread);
    handle_guard process{info.hProcess};
    CloseHandle(write_end.release());
    char chunk[4096];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(read_end.handle, chunk, sizeof chunk, &got, nullptr) || got == 0) {
            break;
        }
        sink(context, chunk, got);
    }
    WaitForSingleObject(process.handle, wait_for_exit);
    return 0;
}

bool process_alive(process_id const pid) noexcept {
    handle_guard process{OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid))};
    if (process.handle == nullptr) {
        return false;
    }
    DWORD code = 0;
    return GetExitCodeProcess(process.handle, &code) && code == STILL_ACTIVE;
}

int stop_process(process_id const pid) noexcept {
    handle_guard process{OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid))};
    if (process.handle == nullptr) {
        return last_win_error();
    }
    return TerminateProcess(process.handle, 1) ? 0 : last_win_error();
}

[[noreturn]] void replace_process(char const* const program, char const* const* const argv) noexcept {
    SetConsoleCtrlHandler(nullptr, TRUE);
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION info{};
    if (auto const failure = create_child(program, argv, startup, 0, info)) {
        std::fprintf(stderr, "llmgate: cannot run %s (error %d)\n", program, failure);
        ::_exit(127);
    }
    handle_guard job{CreateJobObjectW(nullptr, nullptr)};
    if (job.handle != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job.handle, JobObjectExtendedLimitInformation, &limits, sizeof limits);
        AssignProcessToJobObject(job.handle, info.hProcess);
    }
    CloseHandle(info.hThread);
    handle_guard process{info.hProcess};
    WaitForSingleObject(process.handle, wait_for_exit);
    DWORD code = 0;
    GetExitCodeProcess(process.handle, &code);
    ::_exit(static_cast<int>(code));
}

void set_environment_variable(char const* const name, char const* const value) noexcept {
    _putenv_s(name, value);
}

char const* home_directory() noexcept {
    if (auto const* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return home;
    }
    if (auto const* profile = std::getenv("USERPROFILE")) {
        return profile;
    }
    auto const* drive = std::getenv("HOMEDRIVE");
    auto const* path = std::getenv("HOMEPATH");
    if (drive == nullptr || path == nullptr) {
        return nullptr;
    }
    home_storage = std::string{drive} + path;
    return home_storage.c_str();
}

size_t executable_path(char* const buffer, size_t const capacity) noexcept {
    std::wstring name(capacity, L'\0');
    auto const length = GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
    if (length == 0 || length >= name.size()) {
        return 0;
    }
    name.resize(length);
    auto const text = narrow(name);
    if (text.size() >= capacity) {
        return 0;
    }
    std::memcpy(buffer, text.c_str(), text.size() + 1);
    return text.size();
}

bool resolve_executable(char const* const candidate, char* const resolved, size_t const capacity) noexcept {
    auto const base = widen(candidate);
    for (auto const& attempt : executable_attempts(base)) {
        if (is_regular_file(attempt)) {
            return copy_out(narrow(attempt), resolved, capacity);
        }
    }
    return false;
}

char path_list_separator() noexcept {
    return ';';
}

int write_file_atomic(char const* const target, char const* const data, size_t const size, unsigned const mode) noexcept {
    auto const destination = widen(target);
    auto const temporary = temporary_name(destination);
    owner_only_access privately;
    if (wants_private(mode) && !privately.build(false)) {
        return ERROR_ACCESS_DENIED;
    }
    auto const error = write_new_file(temporary, wants_private(mode) ? &privately.attributes : nullptr, data, size);
    if (error == 0 && MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return 0;
    }
    auto const failure = error != 0 ? error : last_win_error();
    DeleteFileW(temporary.c_str());
    return failure;
}

int append_to_file(char const* const target, char const* const data, size_t const size, unsigned const mode) noexcept {
    owner_only_access privately;
    bool const restrict_access = wants_private(mode);
    if (restrict_access && !privately.build(false)) {
        return ERROR_ACCESS_DENIED;
    }
    handle_guard file{open_for_append(widen(target), restrict_access ? &privately.attributes : nullptr)};
    if (file.handle == INVALID_HANDLE_VALUE) {
        return last_win_error();
    }
    if (auto const failure = check_appendable(file.handle, restrict_access ? privately.acl : nullptr)) {
        return failure;
    }
    return write_whole(file.handle, data, size);
}

int file_mode(char const* const path, unsigned* const mode) noexcept {
    auto const wide = widen(path);
    auto const attributes = GetFileAttributesW(wide.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return last_win_error();
    }
    bool const directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    bool const privately = acl_is_private(wide);
    *mode = directory ? (privately ? private_directory_mode : open_directory_mode) : (privately ? private_file_mode : open_file_mode);
    return 0;
}

bool can_write_path(char const* const path) noexcept {
    return _waccess(widen(path).c_str(), 2) == 0;
}

int make_directory_private(char const* const path) noexcept {
    owner_only_access privately;
    if (!privately.build(true)) {
        return last_win_error();
    }
    auto wide = widen(path);
    return SetNamedSecurityInfoW(wide.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, privately.acl, nullptr) ==
                   ERROR_SUCCESS
               ? 0
               : last_win_error();
}

int export_system_roots(void* const context, output_sink const sink) noexcept {
    HCERTSTORE const store = CertOpenSystemStoreW(0, L"ROOT");
    if (store == nullptr) {
        return last_win_error();
    }
    PCCERT_CONTEXT certificate = nullptr;
    while ((certificate = CertEnumCertificatesInStore(store, certificate)) != nullptr) {
        DWORD length = 0;
        constexpr DWORD format = CRYPT_STRING_BASE64HEADER | CRYPT_STRING_NOCR;
        if (!CryptBinaryToStringA(certificate->pbCertEncoded, certificate->cbCertEncoded, format, nullptr, &length) || length == 0) {
            continue;
        }
        std::string pem(length, '\0');
        if (CryptBinaryToStringA(certificate->pbCertEncoded, certificate->cbCertEncoded, format, pem.data(), &length)) {
            sink(context, pem.data(), length);
        }
    }
    CertCloseStore(store, 0);
    return 0;
}

long current_user_id() noexcept {
    return 0;
}

long current_process_id() noexcept {
    return static_cast<long>(GetCurrentProcessId());
}

bool stdin_is_terminal() noexcept {
    return _isatty(0) != 0;
}

bool disable_stdin_echo() noexcept {
    auto const input = GetStdHandle(STD_INPUT_HANDLE);
    if (!GetConsoleMode(input, &saved_console_mode)) {
        return false;
    }
    console_mode_saved = SetConsoleMode(input, saved_console_mode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT)) != 0;
    return console_mode_saved;
}

void restore_stdin_echo() noexcept {
    if (console_mode_saved) {
        SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), saved_console_mode);
        console_mode_saved = false;
    }
}

}  // namespace llmgate::platform

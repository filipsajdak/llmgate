#include <cerrno>

#include "test_io.h"

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include <boost/ut.hpp>

import llmgate.process;
import llmgate.text;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate::process;
using std::chrono::milliseconds;

constexpr milliseconds generous{3000};

#if defined(_WIN32)
std::filesystem::path shell_program() {
    return std::getenv("ComSpec");
}
std::vector<std::string> shell_script(std::string const& body) {
    return {"/c", body};
}
constexpr std::string_view shell_name = "cmd";
#else
std::filesystem::path shell_program() {
    return "/bin/sh";
}
std::vector<std::string> shell_script(std::string const& body) {
    return {"-c", body};
}
constexpr std::string_view shell_name = "sh";
#endif

std::string contents_of(std::filesystem::path const& path) {
    std::ifstream in{path};
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

long own_pid() {
    return llmgate::platform::current_process_id();
}

unsigned mode_of(std::filesystem::path const& path) {
    unsigned mode = 0;
    llmgate::platform::file_mode(path.string().c_str(), &mode);
    return mode;
}

struct canned_server {
    int listener = -1;
    std::uint16_t port = 0;
    std::jthread worker;

    explicit canned_server(std::string reply, bool answer = true) {
        listener = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(listener, reinterpret_cast<sockaddr*>(&local), sizeof local);
        ::listen(listener, 4);
        socklen_t length = sizeof local;
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&local), &length);
        port = ntohs(local.sin_port);
        worker = std::jthread{[this, reply = std::move(reply), answer] {
            int const client = static_cast<int>(::accept(listener, nullptr, nullptr));
            if (client < 0) {
                return;
            }
            std::array<char, 2048> request{};
            auto const got = test_io::read_fd(client, request.data(), request.size());
            seen_request.assign(request.data(), got > 0 ? static_cast<std::size_t>(got) : 0);
            if (answer) {
                test_io::write_fd(client, reply.data(), reply.size());
            }
            test_io::close_fd(client);
        }};
    }

    void wake_accept() const {
        int const poke = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(port);
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::connect(poke, reinterpret_cast<sockaddr*>(&local), sizeof local);
        test_io::close_fd(poke);
    }

    std::string request_seen() {
        if (worker.joinable()) {
            worker.join();
        }
        return seen_request;
    }

    ~canned_server() {
        if (worker.joinable()) {
            wake_accept();
            worker.join();
        }
        test_io::close_fd(listener);
    }

    std::string seen_request;
};

const suite process_suite = [] {
    "capability_appending_adds_lines_creates_the_file_private_and_keeps_earlier_content"_test = [] {
        auto const directory = std::filesystem::temp_directory_path() / std::format("llmgate-append-{}", own_pid());
        auto const target = directory / "nested" / "usage.jsonl";
        std::filesystem::remove_all(directory);

        expect(append_file(target, "one\n", 0600, true).has_value());
        expect(append_file(target, "two\n", 0600, true).has_value());

        expect(contents_of(target) == "one\ntwo\n");
        expect(mode_of(target) == 0600);
        expect(mode_of(target.parent_path()) == 0700);
        std::filesystem::remove_all(directory);
    };

    "limit_appending_refuses_to_follow_a_symlink"_test = [] {
        auto const directory = std::filesystem::temp_directory_path() / std::format("llmgate-link-{}", own_pid());
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
        std::error_code no_link;
        std::filesystem::create_symlink(directory / "elsewhere", directory / "usage.jsonl", no_link);
        if (no_link) {
            std::filesystem::remove_all(directory);
            return;
        }

        expect(!append_file(directory / "usage.jsonl", "x\n", 0600, false).has_value());
        expect(!std::filesystem::exists(directory / "elsewhere"));
        std::filesystem::remove_all(directory);
    };

    "capability_an_http_1_0_answer_is_understood"_test = [] {
        canned_server server{"HTTP/1.0 200 OK\r\n\r\nold"};

        auto const answer = http_get("127.0.0.1", server.port, "/", {}, generous);

        expect(answer.has_value() && answer->status == 200 && answer->body == "old");
    };

    "capability_a_tilde_path_expands_to_the_home_directory"_test = [] {
        expect(expand_home("~/x/y") == std::string{llmgate::platform::home_directory()} + "/x/y");
        expect(expand_home("/abs/path") == "/abs/path");
        expect(expand_home("rel~/p") == "rel~/p");
    };

    "capability_the_current_executable_is_this_test_binary"_test = [] {
        auto const path = current_executable();

        expect(path.has_value());
        expect(path->stem() == "process_test");
    };

    "capability_a_program_is_found_on_the_path_and_a_missing_one_is_not"_test = [] {
        expect(find_in_path(shell_name).has_value());
        expect(!find_in_path("definitely-not-a-program-llmgate").has_value());
        expect(find_in_path(shell_program().generic_string()).has_value());
    };

    "capability_http_get_returns_the_status_and_body_and_sends_host_and_headers"_test = [] {
        canned_server server{"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok"};

        auto const answer = http_get("127.0.0.1", server.port, "/healthz", {{"x-api-key", "k"}}, generous);
        server.worker.join();

        expect(answer.has_value());
        expect(answer->status == 200);
        expect(answer->body == "ok");
        auto const seen = server.request_seen();
        expect(seen.starts_with("GET /healthz HTTP/1.1\r\nHost: 127.0.0.1:"));
        expect(seen.contains("x-api-key: k"));
    };

    "limit_a_server_that_never_answers_times_out_instead_of_hanging"_test = [] {
        canned_server server{"", false};
        auto const started = std::chrono::steady_clock::now();

        auto const answer = http_get("127.0.0.1", server.port, "/", {}, milliseconds{150});

        expect(!answer.has_value() || answer->status == 0);
        expect(std::chrono::steady_clock::now() - started < milliseconds{2000});
    };

    "limit_nothing_listening_means_no_answer_and_the_gateway_is_not_alive"_test = [] {
        std::uint16_t closed_port = 0;
        {
            canned_server server{"x", false};
            closed_port = server.port;
        }

        expect(!http_get("127.0.0.1", closed_port, "/", {}, milliseconds{300}).has_value());
        expect(!gateway_alive("127.0.0.1", closed_port));
    };

    "capability_a_listening_port_is_connectable_and_a_closed_one_is_not"_test = [] {
        std::uint16_t closed_port = 0;
        {
            canned_server gone{"x", false};
            closed_port = gone.port;
        }
        canned_server open{"x", false};

        expect(tcp_connectable("127.0.0.1", open.port, generous));
        expect(!tcp_connectable("127.0.0.1", closed_port, generous));
        expect(!tcp_connectable("not an address", open.port, generous));
    };

    "capability_a_running_process_is_seen_and_an_unused_pid_is_not"_test = [] {
        constexpr long unused_pid = 0x3ffffff0;

        expect(process_running(own_pid()));
        expect(!process_running(unused_pid));
    };

    "limit_a_garbage_reply_is_not_an_answer"_test = [] {
        canned_server server{"not http at all"};

        expect(!http_get("127.0.0.1", server.port, "/", {}, generous).has_value());
    };

    "capability_a_detached_process_runs_and_logs_to_the_file"_test = [] {
        auto const directory = std::filesystem::temp_directory_path() / std::format("llmgate-process-{}", own_pid());
        auto const log = directory / "logs" / "child.log";

        auto const started = start_detached(shell_program(), shell_script("echo detached-hello"), log);
        auto const written = wait_until([&log] {
            std::ifstream in{log};
            std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
            return text.contains("detached-hello");
        }, generous, milliseconds{20});
        std::filesystem::remove_all(directory);

        expect(started.has_value());
        expect(written);
    };

    "limit_starting_a_program_that_does_not_exist_reports_an_error"_test = [] {
        auto const directory = std::filesystem::temp_directory_path() / std::format("llmgate-process-missing-{}", own_pid());

        auto const started = start_detached("/nonexistent/binary", {}, directory / "x.log");
        std::filesystem::remove_all(directory);

        expect(!started.has_value());
    };

    "capability_wait_until_returns_as_soon_as_the_condition_holds_and_false_on_timeout"_test = [] {
        int calls = 0;

        expect(wait_until([&calls] { return ++calls >= 3; }, generous, milliseconds{1}));
        expect(!wait_until([] { return false; }, milliseconds{30}, milliseconds{5}));
    };

    "capability_run_and_wait_returns_the_exit_status_of_the_program"_test = [] {
        expect(run_and_wait(shell_program(), shell_script("exit 0")) == 0_i);
        expect(run_and_wait(shell_program(), shell_script("exit 1")) == 1_i);
        expect(run_and_wait(shell_program(), shell_script("exit 7")) == 7_i);
    };

    "limit_run_and_wait_reports_minus_one_for_a_missing_program_and_a_killed_one"_test = [] {
        expect(run_and_wait("/nonexistent/program", {}) == -1_i);
#if !defined(_WIN32)
        expect(run_and_wait("/bin/sh", {"-c", "kill -9 $$"}) == -1_i);
#endif
    };

    "capability_make_argv_puts_the_program_first_and_ends_with_a_null"_test = [] {
        std::vector<std::string> arguments;
        arguments.emplace_back("--flag");
        arguments.emplace_back("value");

        auto const built = make_argv("/bin/tool", arguments);

        expect(built.pointers.size() == 4_u);
        expect(std::string_view{built.pointers[0]} == "/bin/tool");
        expect(std::string_view{built.pointers[2]} == "value");
        expect(built.pointers.back() == nullptr);
    };

    "capability_the_environment_is_applied_before_an_exec"_test = [] {
        std::vector<llmgate::text::name_value> environment;
        environment.emplace_back("LLMGATE_PROCESS_TEST_A", "one");
        environment.emplace_back("LLMGATE_PROCESS_TEST_B", "two");

        apply_environment(environment);

        expect(std::string_view{std::getenv("LLMGATE_PROCESS_TEST_A")} == "one");
        expect(std::string_view{std::getenv("LLMGATE_PROCESS_TEST_B")} == "two");
    };

    "limit_writing_to_a_closed_descriptor_is_an_error"_test = [] {
        expect(write_all(-1, "x").value() == EBADF);
        expect(!write_all(-1, "").value());
    };

    "capability_an_atomic_write_replaces_the_file_and_leaves_no_temporary_behind"_test = [] {
        auto const directory = std::filesystem::temp_directory_path() / std::format("llmgate-atomic-{}", own_pid());
        auto const target = directory / "nested" / "file.txt";
        std::filesystem::remove_all(directory);

        expect(write_file_atomic(target, "first", 0600, true).has_value());
        expect(write_file_atomic(target, "second", 0600, true).has_value());

        expect(contents_of(target) == "second");
        expect(std::ranges::distance(std::filesystem::directory_iterator{target.parent_path()}) == 1_i);
        std::filesystem::remove_all(directory);
    };

    "limit_an_atomic_write_that_cannot_rename_cleans_up_its_temporary"_test = [] {
        auto const directory = std::filesystem::temp_directory_path() / std::format("llmgate-atomic-dir-{}", own_pid());
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory / "occupied");

        expect(!write_file_atomic(directory / "occupied", "x", 0600, false).has_value());
        expect(std::ranges::distance(std::filesystem::directory_iterator{directory}) == 1_i);
        std::filesystem::remove_all(directory);
    };

    "limit_atomic_write_and_append_fail_when_the_parent_is_a_file_or_the_target_a_directory"_test = [] {
        auto const directory = std::filesystem::temp_directory_path() / std::format("llmgate-blocked-{}", own_pid());
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory / "dir");
        std::ofstream{directory / "file"} << "x";

        expect(!write_file_atomic(directory / "file" / "child", "x", 0600, false).has_value());
        expect(!append_file(directory / "dir", "x", 0600, false).has_value());
        std::filesystem::remove_all(directory);
    };

    "limit_replies_without_a_proper_status_line_or_header_end_are_not_answers"_test = [] {
        for (std::string_view const reply : {"HTTP/1.1 abc OK\r\n\r\nbody", "HTTP/1.1 200 OK\r\nno end of headers", "HTTP/2 200 OK\r\n\r\n",
                                              "HTTP/1.1200 OK\r\n\r\n", "HTTP/1.1 20"}) {
            canned_server server{std::string{reply}};

            expect(!http_get("127.0.0.1", server.port, "/", {}, generous).has_value()) << reply;
        }
    };

    "limit_an_address_that_is_not_ipv4_is_no_answer"_test = [] {
        expect(!http_get("not an address", 80, "/", {}, generous).has_value());
    };

    "capability_read_all_collects_a_pipe_across_chunks_until_it_closes"_test = [] {
        int ends[2];
#if defined(_WIN32)
        expect(::_pipe(ends, 65536, 0x8000) == 0);
#else
        expect(::pipe(ends) == 0);
#endif
        std::string const sent(read_chunk * 2 + 1, 'x');
        std::jthread writer{[&] {
            write_all(ends[1], sent);
            ::close(ends[1]);
        }};

        auto const got = read_all(ends[0]);
        ::close(ends[0]);

        expect(got.has_value() && *got == sent);
    };

    "limit_read_all_reports_a_descriptor_that_cannot_be_read"_test = [] {
        auto const got = read_all(-1);

        expect(!got.has_value() && got.error().value() == EBADF);
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}

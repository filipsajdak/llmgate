#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <boost/ut.hpp>

import llmgate.process;
import std;

namespace {

using namespace boost::ut;
using namespace llmgate::process;
using std::chrono::milliseconds;

constexpr milliseconds generous{3000};

struct canned_server {
    int listener = -1;
    std::uint16_t port = 0;
    std::jthread worker;

    explicit canned_server(std::string reply, bool answer = true) {
        listener = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(listener, reinterpret_cast<sockaddr*>(&local), sizeof local);
        ::listen(listener, 4);
        socklen_t length = sizeof local;
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&local), &length);
        port = ntohs(local.sin_port);
        worker = std::jthread{[this, reply = std::move(reply), answer] {
            int const client = ::accept(listener, nullptr, nullptr);
            if (client < 0) {
                return;
            }
            std::array<char, 2048> request{};
            auto const got = ::recv(client, request.data(), request.size(), 0);
            seen_request.assign(request.data(), got > 0 ? static_cast<std::size_t>(got) : 0);
            if (answer) {
                ::send(client, reply.data(), reply.size(), 0);
            }
            ::close(client);
        }};
    }

    void wake_accept() const {
        int const poke = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(port);
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::connect(poke, reinterpret_cast<sockaddr*>(&local), sizeof local);
        ::close(poke);
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
        ::close(listener);
    }

    std::string seen_request;
};

const suite process_suite = [] {
    "capability_appending_adds_lines_creates_the_file_private_and_keeps_earlier_content"_test = [] {
        auto const directory = std::filesystem::temp_directory_path() / std::format("llmgate-append-{}", ::getpid());
        auto const target = directory / "nested" / "usage.jsonl";
        std::filesystem::remove_all(directory);

        expect(append_file(target, "one\n", 0600, true).has_value());
        expect(append_file(target, "two\n", 0600, true).has_value());

        std::ifstream in{target};
        std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        expect(text == "one\ntwo\n");
        expect((std::filesystem::status(target).permissions() & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) == std::filesystem::perms::none);
        expect((std::filesystem::status(target.parent_path()).permissions() & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) == std::filesystem::perms::none);
        std::filesystem::remove_all(directory);
    };

    "limit_appending_refuses_to_follow_a_symlink"_test = [] {
        auto const directory = std::filesystem::temp_directory_path() / std::format("llmgate-link-{}", ::getpid());
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
        std::filesystem::create_symlink(directory / "elsewhere", directory / "usage.jsonl");

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
        expect(expand_home("~/x/y") == std::string{std::getenv("HOME")} + "/x/y");
        expect(expand_home("/abs/path") == "/abs/path");
        expect(expand_home("rel~/p") == "rel~/p");
    };

    "capability_the_current_executable_is_this_test_binary"_test = [] {
        auto const path = current_executable();

        expect(path.has_value());
        expect(path->filename() == "process_test");
    };

    "capability_a_program_is_found_on_the_path_and_a_missing_one_is_not"_test = [] {
        expect(find_in_path("sh").has_value());
        expect(!find_in_path("definitely-not-a-program-llmgate").has_value());
        expect(find_in_path("/bin/sh").has_value());
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

    "limit_a_garbage_reply_is_not_an_answer"_test = [] {
        canned_server server{"not http at all"};

        expect(!http_get("127.0.0.1", server.port, "/", {}, generous).has_value());
    };

    "capability_a_detached_process_runs_and_logs_to_the_file"_test = [] {
        auto const directory = std::filesystem::temp_directory_path() / std::format("llmgate-process-{}", ::getpid());
        auto const log = directory / "logs" / "child.log";

        auto const started = start_detached("/bin/sh", {"-c", "echo detached-hello"}, log);
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
        auto const directory = std::filesystem::temp_directory_path() / std::format("llmgate-process-missing-{}", ::getpid());

        auto const started = start_detached("/nonexistent/binary", {}, directory / "x.log");
        std::filesystem::remove_all(directory);

        expect(!started.has_value());
    };

    "capability_wait_until_returns_as_soon_as_the_condition_holds_and_false_on_timeout"_test = [] {
        int calls = 0;

        expect(wait_until([&calls] { return ++calls >= 3; }, generous, milliseconds{1}));
        expect(!wait_until([] { return false; }, milliseconds{30}, milliseconds{5}));
    };
};

}  // namespace

int main() {
    return boost::ut::cfg<>.run() ? 1 : 0;
}

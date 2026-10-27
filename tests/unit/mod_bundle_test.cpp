#include <boost/ut.hpp>

import llmgate.mod_bundle;
import std;

namespace {

using namespace boost::ut;
namespace cl = llmgate::claude;

std::filesystem::path const mod_source = std::filesystem::path{LLMGATE_SOURCE_DIR} / "mods" / "llmgate";

std::string read_all(std::filesystem::path const& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

std::set<std::string> bundled_paths() {
    return cl::mod_files | std::views::transform([](cl::bundled_file const& file) { return std::string{file.path}; }) | std::ranges::to<std::set>();
}

suite<"mod_bundle"> tests = [] {
    "capability_every_runtime_file_of_the_mod_is_in_the_bundle"_test = [] {
        auto const bundled = bundled_paths();
        std::set<std::string> on_disk{".claude-plugin/plugin.json"};
        for (auto const& entry : std::filesystem::directory_iterator{mod_source / "hooks"}) {
            on_disk.insert("hooks/" + entry.path().filename().string());
        }

        expect(bundled == on_disk) << "bundle and mods/llmgate differ; update mod_bundle.cppm and LLMGATE_MOD_RUNTIME_FILES";
    };

    "capability_the_bundled_bytes_equal_the_files_in_the_repository"_test = [] {
        for (auto const& file : cl::mod_files) {
            std::string const bundled(file.bytes.begin(), file.bytes.end());
            expect(!bundled.empty() && bundled == read_all(mod_source / file.path)) << file.path;
        }
    };

    "capability_the_digest_is_sixteen_hex_digits_and_does_not_change_between_calls"_test = [] {
        auto const digest = cl::mod_digest();

        expect(digest.size() == 16_u && std::ranges::all_of(digest, [](char const c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; })) << digest;
        expect(digest == cl::mod_digest());
    };

    "limit_the_bundle_holds_no_tests_and_no_type_files"_test = [] {
        for (auto const& file : cl::mod_files) {
            expect(!file.path.contains("tests/") && !file.path.contains("types/") && !file.path.ends_with(".d.ts")) << file.path;
        }
    };
};

}  // namespace

int main() {}

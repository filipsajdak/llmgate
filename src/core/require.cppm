export module llmgate.require;

import std;

export namespace llmgate {

constexpr void require(bool const condition) noexcept {
    if (!condition) {
        std::abort();
    }
}

}  // namespace llmgate

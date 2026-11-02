module;

#include "sockets.h"
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

export module llmgate.proof;

import std;

export namespace llmgate::proof {

constexpr std::size_t sha256_bytes = 32;
constexpr std::string_view hex_digits = "0123456789abcdef";

inline std::string to_hex(std::span<unsigned char const> const bytes) {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (auto const byte : bytes) {
        out.push_back(hex_digits[byte >> 4]);
        out.push_back(hex_digits[byte & 0xF]);
    }
    return out;
}

constexpr std::string_view identity_tag = "llmgate-identity-v1";

constexpr std::string identity_message(std::string_view const scope, std::string_view const challenge) {
    std::string message{identity_tag};
    message += '|';
    message += scope;
    message += '|';
    message += challenge;
    return message;
}

constexpr std::int64_t control_window_seconds = 60;
constexpr std::size_t longest_nonce = 64;
constexpr std::size_t most_remembered_nonces = 4096;

constexpr std::string control_challenge(std::int64_t const sent, std::string_view const nonce, std::string_view const body) {
    std::array<char, 24> digits{};
    auto const end = std::to_chars(digits.data(), digits.data() + digits.size(), sent).ptr;
    std::string message{digits.data(), end};
    message += '|';
    message += nonce;
    message += '|';
    message += body;
    return message;
}

class replay_guard {
public:
    bool admit(std::string_view const nonce, std::int64_t const sent, std::int64_t const now) {
        if (nonce.empty() || nonce.size() > longest_nonce || sent < now - control_window_seconds || sent > now + control_window_seconds) {
            return false;
        }
        std::erase_if(seen_, [now](auto const& entry) { return entry.second < now - control_window_seconds; });
        if (seen_.size() >= most_remembered_nonces) {
            return false;
        }
        return seen_.emplace(std::string{nonce}, sent).second;
    }

private:
    std::map<std::string, std::int64_t, std::less<>> seen_;
};

inline std::array<unsigned char, sha256_bytes> sha256(std::string_view const data) {
    std::array<unsigned char, sha256_bytes> digest{};
    unsigned int length = 0;
    EVP_Digest(data.data(), data.size(), digest.data(), &length, EVP_sha256(), nullptr);
    return digest;
}

inline std::string hmac_sha256_hex(std::string_view const key, std::string_view const message) {
    std::array<unsigned char, sha256_bytes> digest{};
    unsigned int length = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), reinterpret_cast<unsigned char const*>(message.data()), message.size(),
         digest.data(), &length);
    return to_hex(std::span{digest}.first(length));
}

inline std::expected<std::string, std::error_code> random_hex(std::size_t const bytes) {
    std::vector<unsigned char> buffer(bytes);
    if (RAND_bytes(buffer.data(), static_cast<int>(buffer.size())) != 1) {
        return std::unexpected{std::make_error_code(std::errc::resource_unavailable_try_again)};
    }
    return to_hex(buffer);
}

}  // namespace llmgate::proof

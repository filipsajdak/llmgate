export module llmgate.sigv4;

import std;

export namespace llmgate::sigv4 {

constexpr std::size_t digest_bytes = 32;
constexpr std::size_t block_bytes = 64;
constexpr std::size_t length_bytes = 8;
constexpr std::string_view algorithm = "AWS4-HMAC-SHA256";
constexpr std::string_view terminator = "aws4_request";
constexpr std::string_view hex_digits = "0123456789abcdef";
constexpr std::string_view upper_hex_digits = "0123456789ABCDEF";

using digest = std::array<std::uint8_t, digest_bytes>;

namespace detail {

constexpr std::array<std::uint32_t, 64> round_constants{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::array<std::uint32_t, 8> initial_state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

constexpr std::uint32_t rotate_right(std::uint32_t const value, int const bits) {
    return std::rotr(value, bits);
}

constexpr std::array<std::uint32_t, 64> schedule_of(std::string_view const block) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
        for (std::size_t byte = 0; byte < 4; ++byte) {
            words[index] = words[index] << 8 | static_cast<unsigned char>(block[index * 4 + byte]);
        }
    }
    for (std::size_t index = 16; index < 64; ++index) {
        auto const low = rotate_right(words[index - 15], 7) ^ rotate_right(words[index - 15], 18) ^ (words[index - 15] >> 3);
        auto const high = rotate_right(words[index - 2], 17) ^ rotate_right(words[index - 2], 19) ^ (words[index - 2] >> 10);
        words[index] = words[index - 16] + low + words[index - 7] + high;
    }
    return words;
}

constexpr void compress(std::array<std::uint32_t, 8>& state, std::string_view const block) {
    auto const words = schedule_of(block);
    auto working = state;
    for (std::size_t round = 0; round < 64; ++round) {
        auto const sum1 = rotate_right(working[4], 6) ^ rotate_right(working[4], 11) ^ rotate_right(working[4], 25);
        auto const choose = (working[4] & working[5]) ^ (~working[4] & working[6]);
        auto const first = working[7] + sum1 + choose + round_constants[round] + words[round];
        auto const sum0 = rotate_right(working[0], 2) ^ rotate_right(working[0], 13) ^ rotate_right(working[0], 22);
        auto const majority = (working[0] & working[1]) ^ (working[0] & working[2]) ^ (working[1] & working[2]);
        std::ranges::rotate(working, working.begin() + 7);
        working[4] += first;
        working[0] = first + sum0 + majority;
    }
    for (std::size_t index = 0; index < state.size(); ++index) {
        state[index] += working[index];
    }
}

constexpr std::string padded(std::string_view const data) {
    std::string out{data};
    out.push_back(static_cast<char>(0x80));
    while (out.size() % block_bytes != block_bytes - length_bytes) {
        out.push_back('\0');
    }
    auto const bits = static_cast<std::uint64_t>(data.size()) * 8;
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((bits >> shift) & 0xFF));
    }
    return out;
}

}  // namespace detail

constexpr digest sha256(std::string_view const data) {
    auto state = detail::initial_state;
    auto const message = detail::padded(data);
    for (std::size_t at = 0; at < message.size(); at += block_bytes) {
        detail::compress(state, std::string_view{message}.substr(at, block_bytes));
    }
    digest out{};
    for (std::size_t index = 0; index < state.size(); ++index) {
        for (std::size_t byte = 0; byte < 4; ++byte) {
            out[index * 4 + byte] = static_cast<std::uint8_t>(state[index] >> (24 - 8 * byte));
        }
    }
    return out;
}

constexpr std::string view_of(digest const& value) {
    std::string out;
    for (auto const byte : value) {
        out.push_back(static_cast<char>(byte));
    }
    return out;
}

constexpr digest hmac_sha256(std::string_view const key, std::string_view const message) {
    std::string block(block_bytes, '\0');
    if (key.size() > block_bytes) {
        auto const hashed = sha256(key);
        std::ranges::copy(view_of(hashed), block.begin());
    } else {
        std::ranges::copy(key, block.begin());
    }
    std::string inner = block;
    std::string outer = block;
    for (std::size_t index = 0; index < block_bytes; ++index) {
        inner[index] = static_cast<char>(inner[index] ^ 0x36);
        outer[index] = static_cast<char>(outer[index] ^ 0x5c);
    }
    inner.append(message);
    auto const inner_hash = sha256(inner);
    outer.append(view_of(inner_hash));
    return sha256(outer);
}

constexpr std::string hex(digest const& value) {
    std::string out;
    for (auto const byte : value) {
        out.push_back(hex_digits[byte >> 4]);
        out.push_back(hex_digits[byte & 0xF]);
    }
    return out;
}

struct credentials {
    std::string access_key{};
    std::string secret_key{};
    std::string session_token{};
};

struct scope {
    std::string region{};
    std::string service{};
};

struct request {
    std::string_view method{};
    std::string_view target{};
    std::string_view host{};
    std::string_view body{};
};

struct signature {
    std::string authorization{};
    std::string security_token{};
};

constexpr digest signing_key(std::string_view const secret, std::string_view const date, scope const& where) {
    auto const seed = hmac_sha256(std::string{"AWS4"} + std::string{secret}, date);
    auto const regional = hmac_sha256(view_of(seed), where.region);
    auto const serviced = hmac_sha256(view_of(regional), where.service);
    return hmac_sha256(view_of(serviced), terminator);
}

namespace detail {

constexpr bool is_unreserved(char const c) {
    constexpr std::string_view marks = "-._~";
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || marks.contains(c);
}

constexpr std::string encode_path(std::string_view const path) {
    std::string out;
    for (char const c : path) {
        if (is_unreserved(c) || c == '/') {
            out.push_back(c);
        } else {
            auto const code = static_cast<unsigned char>(c);
            out.push_back('%');
            out.push_back(upper_hex_digits[code >> 4]);
            out.push_back(upper_hex_digits[code & 0xF]);
        }
    }
    return out.empty() ? std::string{"/"} : out;
}

constexpr std::string sorted_query(std::string_view const query) {
    std::vector<std::string> pairs;
    for (auto const part : std::views::split(query, '&')) {
        if (!std::ranges::empty(part)) {
            pairs.emplace_back(std::string_view{part.begin(), part.end()});
        }
    }
    std::ranges::sort(pairs);
    std::string out;
    for (auto const& pair : pairs) {
        out += out.empty() ? "" : "&";
        out += pair.contains('=') ? pair : pair + "=";
    }
    return out;
}

struct target_parts {
    std::string_view path{};
    std::string_view query{};
};

constexpr target_parts split_target(std::string_view const target) {
    auto const mark = target.find('?');
    if (mark == std::string_view::npos) {
        return {.path = target};
    }
    return {.path = target.substr(0, mark), .query = target.substr(mark + 1)};
}

constexpr std::string signed_header_names(credentials const& keys) {
    return keys.session_token.empty() ? "host;x-amz-date" : "host;x-amz-date;x-amz-security-token";
}

constexpr std::string canonical_headers(request const& item, credentials const& keys, std::string_view const stamp) {
    std::string out;
    out.append("host:").append(item.host).append("\nx-amz-date:").append(stamp).append("\n");
    if (!keys.session_token.empty()) {
        out.append("x-amz-security-token:").append(keys.session_token).append("\n");
    }
    return out;
}

constexpr std::string canonical_request(request const& item, credentials const& keys, std::string_view const stamp) {
    auto const parts = split_target(item.target);
    std::string out;
    out.append(item.method).append("\n").append(encode_path(parts.path)).append("\n").append(sorted_query(parts.query)).append("\n");
    out.append(canonical_headers(item, keys, stamp)).append("\n").append(signed_header_names(keys)).append("\n");
    out.append(hex(sha256(item.body)));
    return out;
}

constexpr std::string credential_scope(std::string_view const date, scope const& where) {
    std::string out;
    out.append(date).append("/").append(where.region).append("/").append(where.service).append("/").append(terminator);
    return out;
}

}  // namespace detail

constexpr signature sign(request const& item, credentials const& keys, scope const& where, std::string_view const stamp) {
    auto const date = stamp.substr(0, 8);
    auto const covered = detail::credential_scope(date, where);
    std::string to_sign;
    to_sign.append(algorithm).append("\n").append(stamp).append("\n").append(covered).append("\n");
    to_sign.append(hex(sha256(detail::canonical_request(item, keys, stamp))));
    auto const proof = hex(hmac_sha256(view_of(signing_key(keys.secret_key, date, where)), to_sign));
    signature out{.security_token = keys.session_token};
    out.authorization.append(algorithm).append(" Credential=").append(keys.access_key).append("/").append(covered);
    out.authorization.append(", SignedHeaders=").append(detail::signed_header_names(keys)).append(", Signature=").append(proof);
    return out;
}

inline std::string stamp_of(std::chrono::sys_seconds const when) {
    return std::format("{:%Y%m%dT%H%M%SZ}", when);
}

static_assert(hex(sha256("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
static_assert(hex(sha256("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
static_assert(hex(hmac_sha256("key", "The quick brown fox jumps over the lazy dog")) == "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");
static_assert(hex(signing_key("wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY", "20150830", {.region = "us-east-1", .service = "iam"})) ==
              "c4afb1cc5771d871763a393e44b703571b55cc28424d1a5e86da6ed3c154a4b9");
static_assert(sign({.method = "GET", .target = "/", .host = "example.amazonaws.com"}, {.access_key = "AKIDEXAMPLE", .secret_key = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY"},
                   {.region = "us-east-1", .service = "service"}, "20150830T123600Z")
                  .authorization == "AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20150830/us-east-1/service/aws4_request, SignedHeaders=host;x-amz-date, "
                                    "Signature=5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31");

}  // namespace llmgate::sigv4

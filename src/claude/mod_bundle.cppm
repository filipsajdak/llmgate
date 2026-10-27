export module llmgate.mod_bundle;

import std;

namespace llmgate::claude::detail {

constexpr unsigned char plugin_json[] = {
#embed LLMGATE_EMBED_PLUGIN_JSON
};

constexpr unsigned char hooks_json[] = {
#embed LLMGATE_EMBED_HOOKS_JSON
};

constexpr unsigned char model_ts[] = {
#embed LLMGATE_EMBED_MODEL_TS
};

constexpr unsigned char recipes_ts[] = {
#embed LLMGATE_EMBED_RECIPES_TS
};

constexpr unsigned char register_tsx[] = {
#embed LLMGATE_EMBED_REGISTER_TSX
};

constexpr unsigned char scorecard_ts[] = {
#embed LLMGATE_EMBED_SCORECARD_TS
};

constexpr unsigned char setup_ts[] = {
#embed LLMGATE_EMBED_SETUP_TS
};

constexpr std::uint64_t fnv_offset = 14695981039346656037ULL;
constexpr std::uint64_t fnv_prime = 1099511628211ULL;
constexpr unsigned hex_digits = 16;
constexpr unsigned bits_per_digit = 4;

}  // namespace llmgate::claude::detail

export namespace llmgate::claude {

struct bundled_file {
    std::string_view path{};
    std::span<unsigned char const> bytes{};
};

constexpr std::array<bundled_file, 7> mod_files{{
    {".claude-plugin/plugin.json", detail::plugin_json},
    {"hooks/hooks.json", detail::hooks_json},
    {"hooks/model.ts", detail::model_ts},
    {"hooks/recipes.ts", detail::recipes_ts},
    {"hooks/register.tsx", detail::register_tsx},
    {"hooks/scorecard.ts", detail::scorecard_ts},
    {"hooks/setup.ts", detail::setup_ts},
}};

constexpr std::uint64_t mod_digest_value() {
    auto hash = detail::fnv_offset;
    auto const mix = [&hash](unsigned char const byte) { hash = (hash ^ byte) * detail::fnv_prime; };
    for (auto const& file : mod_files) {
        std::ranges::for_each(file.path, [&](char const c) { mix(static_cast<unsigned char>(c)); });
        mix(0);
        std::ranges::for_each(file.bytes, mix);
        mix(0);
    }
    return hash;
}

constexpr std::string mod_digest() {
    constexpr std::string_view digits = "0123456789abcdef";
    auto const value = mod_digest_value();
    std::string out(detail::hex_digits, '0');
    for (unsigned index = 0; index < detail::hex_digits; ++index) {
        out[detail::hex_digits - 1 - index] = digits[(value >> (index * detail::bits_per_digit)) & 0xF];
    }
    return out;
}

}  // namespace llmgate::claude

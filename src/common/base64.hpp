#pragma once

// Minimal Base64 decoder for subscription bodies. Subscription providers
// often wrap Clash YAML in Base64; we only need decode (no encode).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace clew {

[[nodiscard]] inline std::optional<std::string> base64_decode(std::string_view in) {
    static constexpr int8_t kTable[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };

    // Strip whitespace / newlines common in pasted subscriptions.
    std::string cleaned;
    cleaned.reserve(in.size());
    for (unsigned char c : in) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        cleaned.push_back(static_cast<char>(c));
    }
    if (cleaned.empty() || cleaned.size() % 4 != 0) return std::nullopt;

    std::string out;
    out.reserve(cleaned.size() / 4 * 3);

    for (size_t i = 0; i < cleaned.size(); i += 4) {
        const int a = kTable[static_cast<unsigned char>(cleaned[i])];
        const int b = kTable[static_cast<unsigned char>(cleaned[i + 1])];
        const int c = cleaned[i + 2] == '=' ? 0
                      : kTable[static_cast<unsigned char>(cleaned[i + 2])];
        const int d = cleaned[i + 3] == '=' ? 0
                      : kTable[static_cast<unsigned char>(cleaned[i + 3])];
        if (a < 0 || b < 0 || (cleaned[i + 2] != '=' && c < 0) ||
            (cleaned[i + 3] != '=' && d < 0)) {
            return std::nullopt;
        }
        const uint32_t triple = (static_cast<uint32_t>(a) << 18) |
                                (static_cast<uint32_t>(b) << 12) |
                                (static_cast<uint32_t>(c) << 6) |
                                 static_cast<uint32_t>(d);
        out.push_back(static_cast<char>((triple >> 16) & 0xFF));
        if (cleaned[i + 2] != '=') out.push_back(static_cast<char>((triple >> 8) & 0xFF));
        if (cleaned[i + 3] != '=') out.push_back(static_cast<char>(triple & 0xFF));
    }
    return out;
}

} // namespace clew

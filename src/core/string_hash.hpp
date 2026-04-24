#pragma once

// Transparent heterogeneous hash + comparator for string-keyed containers.
//
// C++20 enables heterogeneous lookup on `std::unordered_map`/`_set` when the
// hasher exposes `is_transparent` AND the key-equal type is a transparent
// comparator (the empty-specialization `std::equal_to<>` suffices).
//
// Effect: `map.find(sv)`, `.count(sv)`, `.contains(sv)`, `.equal_range(sv)`
// no longer implicitly construct a `std::string` from the lookup key.
// (Note: `operator[]` and `insert`/`emplace` still require a key that can
//  actually be stored; C++26 P2363 will address that. For C++23 we accept
//  the `std::string` construction on insert.)

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace clew {

struct string_hash {
    using is_transparent = void;                     // enable heterogeneous lookup
    using hasher = std::hash<std::string_view>;

    [[nodiscard]] std::size_t operator()(std::string_view sv) const noexcept {
        return hasher{}(sv);
    }
    [[nodiscard]] std::size_t operator()(const std::string& s) const noexcept {
        return hasher{}(s);
    }
    [[nodiscard]] std::size_t operator()(const char* s) const noexcept {
        return hasher{}(s);
    }
};

template<class V>
using string_map = std::unordered_map<std::string, V, string_hash, std::equal_to<>>;

using string_set = std::unordered_set<std::string, string_hash, std::equal_to<>>;

} // namespace clew

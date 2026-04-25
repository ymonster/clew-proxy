#pragma once

// Generic PATCH-style partial-update helper for nlohmann::json.
//
// Given a target object and a JSON patch document, copies just the
// allow-listed fields whose keys are present in the patch. The allow-list
// doubles as a security boundary — anything not bound is unchangeable
// regardless of what the client sends.
//
// Usage:
//   apply_patch(g, patch,
//       field_binding{"name",     &ProxyGroup::name},
//       field_binding{"host",     &ProxyGroup::host},
//       field_binding{"port",     &ProxyGroup::port});
//
// Type safety: pointer-to-member binding fails to compile if the field
// doesn't exist or has a type incompatible with nlohmann::json::get.
//
// Performance: one hash lookup per bound field (find + deref iterator),
// half the cost of the manual `contains(k) + [k]` pattern. The path is
// human-driven (UI config edits), not on any packet/event hot path.

#include <string_view>
#include <type_traits>

#include <nlohmann/json.hpp>

namespace clew {

template <class T, class V>
struct field_binding {
    std::string_view key;
    V T::*           member;
};

template <class T, class... Vs>
void apply_patch(T& obj, const nlohmann::json& patch,
                 const field_binding<T, Vs>&... fields) {
    auto one = [&](auto& f) {
        using M = std::remove_reference_t<decltype(obj.*(f.member))>;
        if (auto it = patch.find(f.key); it != patch.end()) {
            obj.*(f.member) = it->template get<M>();
        }
    };
    (one(fields), ...);
}

} // namespace clew

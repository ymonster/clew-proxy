#pragma once

// process_tree_service — process tree queries + hijack/unhijack CRUD.
//
// The full-tree snapshot path (formerly served via /api/processes) was
// removed when the backend->frontend push channel switched from SSE to
// WebView2 PostMessage; the snapshot is now delivered straight inside
// each push payload by process_projection. Per-PID query and hijack
// CRUD remain HTTP.

#include <cstdint>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "domain/strand_bound.hpp"

namespace clew {

class process_tree_service {
public:
    explicit process_tree_service(strand_bound_manager& exec);

    process_tree_service(const process_tree_service&)            = delete;
    process_tree_service& operator=(const process_tree_service&) = delete;

    // /api/processes/:pid
    [[nodiscard]] nlohmann::json find_process(std::uint32_t pid) const;
    // /api/processes/:pid/detail — adds cmdline + image_path (queried off-strand)
    [[nodiscard]] nlohmann::json find_process_detail(std::uint32_t pid) const;

    // /api/hijack
    [[nodiscard]] nlohmann::json list_hijacked() const;

    // /api/hijack/:pid — POST and DELETE variants
    void hijack(std::uint32_t pid, bool tree_mode, std::uint32_t group_id);
    void unhijack(std::uint32_t pid, bool tree_mode);

    // /api/hijack/batch — action = "hijack" | "unhijack"
    nlohmann::json batch_hijack(const std::vector<std::uint32_t>& pids,
                                std::string_view action,
                                std::uint32_t group_id);

private:
    strand_bound_manager& exec_;
};

} // namespace clew

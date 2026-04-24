#pragma once

// process_tree_service — process tree queries + hijack/unhijack CRUD.
//
// tree_snapshot() returns an immutable shared_ptr<const string> of the
// tree JSON, maintained by process_projection (Stage 3). Stage 2 wires
// the getter as null; handlers then fall back to building via strand
// (see query_tree_snapshot).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "domain/strand_bound.hpp"

namespace clew {

class process_tree_service {
public:
    using snapshot_fn = std::function<std::shared_ptr<const std::string>()>;

    explicit process_tree_service(strand_bound_manager& exec);

    process_tree_service(const process_tree_service&)            = delete;
    process_tree_service& operator=(const process_tree_service&) = delete;

    // Injected by main.cpp after process_projection is constructed. Before
    // injection, tree_snapshot() falls back to computing via strand.
    void set_snapshot_getter(snapshot_fn fn);

    // /api/processes — full tree JSON snapshot
    [[nodiscard]] std::shared_ptr<const std::string> tree_snapshot() const;

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
    snapshot_fn           snapshot_getter_;
};

} // namespace clew

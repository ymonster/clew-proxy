#pragma once

// Cross-strand dispatch wrapper for strand-owned domain objects.
// See refactor_docs/DESIGN.md H1.
//
// strand_bound<Owner> exposes query(Fn) / command(Fn), concept-constrained
// to accept (const Owner&) / (Owner&) respectively. Both post the lambda to
// the owner's strand via std::packaged_task, block on the future, and
// normalize exceptions into api_exception so handler/service code only
// has to care about a single exception type.
//
// Forward-declares clew::domain::process_tree_manager at the bottom so
// this header can be included without pulling the full manager header;
// the typed alias `strand_bound_manager` is then usable wherever the
// alias is referenced. Any TU that actually calls query/command still
// needs the full process_tree_manager definition at instantiation time.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif

#include <asio.hpp>

#include <future>
#include <type_traits>
#include <utility>

#include "common/api_exception.hpp"

namespace clew {

template <typename Fn, typename Owner>
concept query_fn = requires(Fn fn, const Owner& owner) {
    { fn(owner) };
};

template <typename Fn, typename Owner>
concept command_fn = requires(Fn fn, Owner& owner) {
    { fn(owner) };
};

template <typename Owner>
class strand_bound {
public:
    using strand_type = asio::strand<asio::io_context::executor_type>;

    strand_bound(Owner& owner, strand_type& strand) noexcept
        : owner_(owner), strand_(strand) {}

    strand_bound(const strand_bound&)            = delete;
    strand_bound& operator=(const strand_bound&) = delete;

    // Read-only access. Fn must be callable as (const Owner&) -> R.
    // Posts the lambda to the strand and blocks until it completes.
    template <typename Fn>
        requires query_fn<Fn, Owner>
    auto query(Fn&& fn) const -> std::invoke_result_t<Fn, const Owner&> {
        using R = std::invoke_result_t<Fn, const Owner&>;
        std::packaged_task<R()> task(
            [fn = std::forward<Fn>(fn), this]() mutable -> R {
                try {
                    return fn(std::as_const(owner_));
                } catch (const api_exception&) {
                    throw;
                } catch (const std::exception& e) {
                    throw api_exception{api_error::internal, e.what()};
                } catch (...) {
                    throw api_exception{api_error::internal, "unknown error"};
                }
            });
        auto fut = task.get_future();
        // Wrap the packaged_task inside a lambda before posting. Posting the
        // task object directly triggers "future already retrieved" with the
        // asio version we ship (packaged_task's shared-state transfer through
        // asio's handler storage is not well-defined). This lambda form is
        // what the legacy strand_sync() used.
        asio::post(strand_, [t = std::move(task)]() mutable { t(); });
        return fut.get();
    }

    // Mutation. Fn must be callable as (Owner&) -> R.
    // Posts the lambda to the strand and blocks until it completes.
    template <typename Fn>
        requires command_fn<Fn, Owner>
    auto command(Fn&& fn) -> std::invoke_result_t<Fn, Owner&> {
        using R = std::invoke_result_t<Fn, Owner&>;
        std::packaged_task<R()> task(
            [fn = std::forward<Fn>(fn), this]() mutable -> R {
                try {
                    return fn(owner_);
                } catch (const api_exception&) {
                    throw;
                } catch (const std::exception& e) {
                    throw api_exception{api_error::internal, e.what()};
                } catch (...) {
                    throw api_exception{api_error::internal, "unknown error"};
                }
            });
        auto fut = task.get_future();
        // Wrap the packaged_task inside a lambda before posting. Posting the
        // task object directly triggers "future already retrieved" with the
        // asio version we ship (packaged_task's shared-state transfer through
        // asio's handler storage is not well-defined). This lambda form is
        // what the legacy strand_sync() used.
        asio::post(strand_, [t = std::move(task)]() mutable { t(); });
        return fut.get();
    }

private:
    Owner&       owner_;
    strand_type& strand_;
};

// Forward declaration so the typed alias below is spellable without
// pulling domain/process_tree_manager.hpp. Full definition lives in
// domain/process_tree_manager.hpp.
namespace domain { class process_tree_manager; }

using strand_bound_manager = strand_bound<domain::process_tree_manager>;

} // namespace clew

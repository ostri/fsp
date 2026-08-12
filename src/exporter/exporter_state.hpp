#pragma once

/**
 * @file exporter_state.hpp
 * @brief Shared, mutex-protected run state for fsp::exporter<T,Q>: the static drain list, the
 * lazily-loaded per-drain run statistics, the still-available drains, per-document bookkeeping,
 * and the run-wide controlled-stop signal.
 *
 * A plain (non-template) class -- it only stores drain/document bookkeeping, never a
 * transaction, so it does not need to know T or Q. Shared by reference between exporter<T,Q> and
 * every exporter_worker<T,Q>, mirroring how src/importer/pipeline.hpp shares its own state
 * (results_/errors_/... each behind their own std::mutex) with pipeline_worker.
 *
 * Deliberate deviation from doc/opis_exporterja.txt's note that "when we say queue, we mean
 * lock_query<int>": available_drains_ is a plain std::vector<int> guarded by its own std::mutex, not
 * fsp::lock_queue<int> (src/common/lock_queue/lock_queue.hpp). lock_queue is a FIFO
 * push/pop/try_pop wrapper with no random-access erase and no membership check -- but the actual
 * operations the spec requires on available_drains_ are "keep my drain-id if I already have one"
 * (membership check), "random selection among more than one" (indexed access), and "remove this
 * specific drain-id" (erase-by-value) -- none of which fit lock_queue's API without bolting on an
 * out-of-band drain/filter/refill under a second lock, which would be strictly worse than a small
 * hand-rolled vector + mutex.
 */

#include "exporter_error.hpp"
#include "exporter_types.hpp"
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <unordered_map>
#include <vector>

namespace fsp
{
  /**
   * @brief Owns the run-wide state shared by every exporter_worker<T,Q> thread. Every public
   * method takes and releases its own lock internally -- callers never lock exporter_state
   * themselves.
   */
  class exporter_state
  {
  public:
    explicit exporter_state(std::vector<drain_dscr_t> drains);

    // --- available_drains_: which drains still have unclaimed work ---

    /**
     * @brief Returns the drain-id a worker should keep working on.
     * @param current the worker's currently-held drain-id, if any -- returned unchanged without
     * touching available_drains_ at all ("keep it" per doc/opis_exporterja.txt), so a drain
     * removed from available_drains_ by another thread never interrupts a thread already
     * committed to it.
     * @return current if set; otherwise one drain-id picked from available_drains_ (uniformly at
     * random when more than one remains); nullopt if available_drains_ was found empty (a race
     * with another thread that just exhausted the last drain -- not an error, caller should
     * re-check available_drains_empty() and loop).
     */
    [[nodiscard]] std::optional<int> pick_or_keep_drain(std::optional<int> current);
    /// @brief Removes drain_id from available_drains_, if present. Idempotent.
    void remove_available_drain(int drain_id);
    /// @brief True once every drain has been removed from available_drains_.
    [[nodiscard]] bool available_drains_empty() const;

    // --- drain_statistic_: lazily-loaded per-drain run statistics ---

    /// @brief True once drain_id's initial statistics have been loaded (see load_drain_stat_if_needed()).
    [[nodiscard]] bool is_drain_loaded(int drain_id) const;
    /**
     * @brief Loads drain_id's initial statistics exactly once, the first time any worker calls
     * this for that drain.
     * @details Double-checked locking: is_drain_loaded() (lock-free-ish fast path via the caller)
     * lets a worker skip this call entirely once loaded; here, under stats_mutex_, the loaded
     * flag is checked again so a second caller that lost the race to a first one does not
     * re-invoke fetch (idempotent). max_doc_txn is needed to compute initial_future_doc_count.
     * @param fetch invokes the caller's cb_exporter::fetch_run_stat() for drain_id; only called
     * at all if this is genuinely the first (winning) caller for drain_id.
     */
    void load_drain_stat_if_needed(int drain_id, std::size_t max_doc_txn, const std::function<exp_result<run_stat_pair_t>()>& fetch);
    /// @brief Atomically allocates and returns the next document id for drain_id (numbered from 1).
    [[nodiscard]] std::uint64_t next_doc_id(int drain_id);

    // --- doc_statistics_: per-document bookkeeping ---

    /// @brief Registers a new in-progress document, returning its index for a later finalize_doc() call.
    std::size_t register_doc_start(doc_statistics_t entry);
    /// @brief Updates a previously-registered document's entry once it has been fully written.
    void finalize_doc(std::size_t doc_stat_ndx, doc_statistics_t updated_fields);
    /// @brief Increments the running "documents produced" count for drain_id (the run's global stat).
    void increment_drain_doc_count(int drain_id);

    /// @brief Looks up drain_id's static description, or nullptr if unknown.
    [[nodiscard]] const drain_dscr_t*              find_drain(int drain_id) const noexcept;
    [[nodiscard]] const std::vector<drain_dscr_t>& drains() const noexcept { return drain_static_; }

    // --- controlled-stop signal shared by every worker ---

    /// @brief One stop_token, shared by every worker thread, so one request_stop() call reaches all of them.
    [[nodiscard]] std::stop_token stop_token() const noexcept { return stop_source_.get_token(); }
    /// @brief Requests every worker thread to stop at its next loop check. Idempotent.
    void request_stop() { stop_source_.request_stop(); }
  private:
    std::vector<drain_dscr_t> drain_static_; // immutable after construction, no lock needed

    mutable std::mutex available_mutex_; // protects available_drains_ -- the hottest lock, touched every loop iteration
    std::vector<int>   available_drains_;

    mutable std::mutex             stats_mutex_;     // protects only the one-time 'loaded' transition per drain
    std::vector<drain_statistic_t> drain_statistic_; // sized once in the ctor, never resized (see drain_statistic_t)

    mutable std::mutex            doc_stats_mutex_; // protects doc_statistics_, touched once per document
    std::vector<doc_statistics_t> doc_statistics_;

    mutable std::mutex                   doc_count_mutex_; // protects drain_doc_counts_
    std::unordered_map<int, std::size_t> drain_doc_counts_;

    std::stop_source stop_source_; // one shared source; every worker reads its token
  };
} // namespace fsp

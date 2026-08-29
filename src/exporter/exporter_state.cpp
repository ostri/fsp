#include "exporter_state.hpp"
#include <algorithm>
#include <random>

namespace fsp
{
  namespace
  {
    // Thread-local RNG for pick_or_keep_drain()'s random selection -- avoids contending on a
    // single shared generator's own state across worker threads, and avoids reseeding std::rand.
    std::mt19937& rng() // NOLINT(cert-msc32-c,cert-msc51-cpp) -- not used for anything security-sensitive
    {
      thread_local std::mt19937 gen{std::random_device{}()};
      return gen;
    }
  } // namespace

  exporter_state::exporter_state(std::vector<drain_dscr_t> drains, std::size_t phase1_worker_count)
  : drain_static_(std::move(drains))
  , drain_statistic_(drain_static_.size()) // sized once here, never resized afterward
  , phase1_active_workers_(phase1_worker_count)
  {
    available_drains_.reserve(drain_static_.size());
    for (const auto& d : drain_static_) { available_drains_.push_back(d.id); }
  }

  std::optional<drain_t> exporter_state::pick_or_keep_drain(std::optional<drain_t> current)
  {
    if (current.has_value()) { return current; }

    const std::scoped_lock lock(available_mutex_);
    if (available_drains_.empty()) { return std::nullopt; }
    if (available_drains_.size() == 1) { return available_drains_.front(); }

    std::uniform_int_distribution<std::size_t> dist(0, available_drains_.size() - 1);
    return available_drains_.at(dist(rng()));
  }

  void exporter_state::remove_available_drain(drain_t drain_id)
  {
    const std::scoped_lock lock(available_mutex_);
    std::erase(available_drains_, drain_id);
  }

  bool exporter_state::available_drains_empty() const
  {
    const std::scoped_lock lock(available_mutex_);
    return available_drains_.empty();
  }

  bool exporter_state::is_drain_loaded(drain_t drain_id) const
  {
    const auto* d = find_drain(drain_id);
    if (d == nullptr) { return false; }
    const auto ndx = static_cast<std::size_t>(std::distance(static_cast<const drain_dscr_t*>(drain_static_.data()), d));

    const std::scoped_lock lock(stats_mutex_);
    return drain_statistic_.at(ndx).loaded;
  }

  void exporter_state::load_drain_stat_if_needed(drain_t                                             drain_id,
                                                 std::size_t                                         max_doc_txn,
                                                 const std::function<exp_result<run_stat_pair_t>()>& fetch)
  {
    const auto* d = find_drain(drain_id);
    if (d == nullptr) { return; } // unknown drain id -- nothing to load, caller will fail on the next step
    const auto ndx = static_cast<std::size_t>(std::distance(static_cast<const drain_dscr_t*>(drain_static_.data()), d));

    {
      const std::scoped_lock lock(stats_mutex_);
      if (drain_statistic_.at(ndx).loaded) { return; } // lost the race to another thread -- idempotent no-op
    }

    // fetch() itself (the caller's own cb_exporter::fetch_run_stat()) runs OUTSIDE stats_mutex_ --
    // it is a network round trip to the caller's own database, not bounded work, and stats_mutex_
    // is shared by every drain, not one lock per drain (drain_statistic_ is a flat vector, sized
    // once in the ctor, indexed by ndx). Holding the lock across fetch() would serialize every
    // OTHER drain's own first fetch_run_stat() call behind whichever one happens to run first -
    // confirmed directly (multi-worker export run, gdb thread sampling): every worker thread but
    // one parked here in pthread_mutex_lock, the remaining one still inside its own fetch()'s own
    // PQgetResult() wait, for as long as that single call took. Real double-checked locking
    // instead: check under lock (above), fetch() unlocked, re-lock only to check+write the result
    // (below) - a second caller that raced this one to the SAME drain_id and lost still does one
    // redundant fetch() (wasted work, not a correctness problem: run_stat_pair_t is plain,
    // idempotent data), but every OTHER drain's own first caller is never blocked on it.
    const auto stat = fetch();
    if (! stat.has_value()) { return; } // caller (exporter_worker) re-observes !loaded and surfaces the fetch error itself

    const std::scoped_lock lock(stats_mutex_);
    auto&                  stat_entry = drain_statistic_.at(ndx);
    if (stat_entry.loaded) { return; } // another thread's own fetch() already won this drain_id - discard ours, not an error

    stat_entry.initial_txn_count        = stat->remaining_txn_count;
    stat_entry.initial_doc_count        = stat->existing_doc_count;
    stat_entry.initial_future_doc_count = max_doc_txn > 0 ? (stat->remaining_txn_count / max_doc_txn) + 1 : 0;
    stat_entry.last_doc_id.store(stat->existing_doc_count, std::memory_order_relaxed);
    stat_entry.loaded = true;
  }

  doc_id_t exporter_state::next_doc_id(drain_t drain_id)
  {
    const auto* d = find_drain(drain_id);
    if (d == nullptr) { return 0; }
    const auto ndx = static_cast<std::size_t>(std::distance(static_cast<const drain_dscr_t*>(drain_static_.data()), d));
    // last_doc_id is itself atomic -- no mutex needed here once the drain is loaded.
    return drain_statistic_.at(ndx).last_doc_id.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  std::size_t exporter_state::register_doc_start(doc_statistics_t entry)
  {
    const std::scoped_lock lock(doc_stats_mutex_);
    doc_statistics_.push_back(std::move(entry));
    return doc_statistics_.size() - 1;
  }

  void exporter_state::finalize_doc(std::size_t doc_stat_ndx, doc_statistics_t updated_fields)
  {
    const std::scoped_lock lock(doc_stats_mutex_);
    if (doc_stat_ndx < doc_statistics_.size()) { doc_statistics_.at(doc_stat_ndx) = std::move(updated_fields); }
  }

  void exporter_state::increment_drain_doc_count(drain_t drain_id)
  {
    const std::scoped_lock lock(doc_count_mutex_);
    ++drain_doc_counts_[drain_id];
  }

  const drain_dscr_t* exporter_state::find_drain(drain_t drain_id) const noexcept
  {
    const auto it = std::ranges::find(drain_static_, drain_id, &drain_dscr_t::id);
    return it == drain_static_.end() ? nullptr : &*it;
  }

  void exporter_state::publish_blocks(drain_t drain_id, const std::vector<doc_id_t>& doc_ids)
  {
    const std::scoped_lock lock(work_queue_mutex_);
    for (const auto id : doc_ids) { work_queue_.push_back(drain_doc_slot_t{.drain_id = drain_id, .doc_id = id}); }
  }

  std::optional<drain_doc_slot_t> exporter_state::pop_work_block()
  {
    const std::scoped_lock lock(work_queue_mutex_);
    if (work_queue_.empty()) { return std::nullopt; }
    const auto slot = work_queue_.front();
    work_queue_.pop_front();
    return slot;
  }

  bool exporter_state::mark_phase1_worker_done()
  {
    // fetch_sub returns the value BEFORE the decrement -- phase 1 as a whole is done once that
    // value was 1 (this call is the last one), i.e. the counter is now 0.
    return phase1_active_workers_.fetch_sub(1, std::memory_order_acq_rel) == 1;
  }
} // namespace fsp

#pragma once

#include <logger/logger.hpp>
#include "xml_segment.hpp"
#include "segment_result.hpp"
#include "lock_queue.hpp"
#include <mutex>
#include <atomic>
#include <vector>

namespace fsp
{
  using ndx_queue = lock_queue<std::size_t>;
  class segment_pool
  {
  public:
    // segment_pool();
    explicit segment_pool(const logger::Logger& log, std::size_t no_of_slots, std::size_t num_shards = 1);
    void                      init(std::size_t capacity = 1024UL);  // NOLINT(readability-magic-numbers)
    std::size_t               acquire_slot(std::size_t segment_id); // get free slot for segment_id's shard (blocks if none)
    void                      push_ready(std::size_t idx);
    auto                      try_pop_ready(std::size_t shard);
    [[nodiscard]] std::size_t size() const noexcept;
    void                      ready_queue_close();
    void                      abort();
    queue_status              pop_segment_ndx(std::size_t shard, std::size_t& ndx);
    void                      set_segment(std::size_t ndx, xml_segment seg);
    void                      set_result(std::size_t ndx, const segment_result& seg_r);
    xml_segment               retrieve_segment(std::size_t ndx);
    [[nodiscard]] std::size_t ready_queue_size() const
    {
      std::size_t total = 0;
      for (const auto& q : ready_queues_) total += q.size();
      return total;
    }
    [[nodiscard]] std::ptrdiff_t ready_queue_size_approx(std::size_t shard) const noexcept;
    [[nodiscard]] std::size_t    num_shards() const noexcept { return num_shards_; }
    // Highest number of distinct slots ever handed out from the unallocated pool (across all
    // shards) -- a shard only reaches for a never-before-used slot when its own free_queues_
    // entry is empty, so this is the peak footprint the pool actually needed this run, as
    // opposed to capacity_ (the fixed size it was allocated with up front).
    [[nodiscard]] std::size_t high_water_mark() const noexcept { return next_unallocated_slot_.load(std::memory_order_relaxed); }
  private:
    const logger::Logger&    log_;
    std::size_t              capacity_{0};
    std::size_t              num_shards_{1};
    std::atomic<std::size_t> next_unallocated_slot_{0};
    // Segment id (Handler::counter_) modulo num_shards_ picks the shard -- since a document's
    // segments get consecutive ids as its cutter thread produces them, this round-robins one
    // document's segments evenly across shards. Splitting the single ready/free lock_queue pair
    // into num_shards_ independent pairs directly cuts down how many concurrent C/P threads
    // contend on any one queue's mutex+condition_variable (see the 21x-more-CPU-seconds/327x-more
    // voluntary-context-switches blowup measured going from 1 to 10 concurrent documents).
    std::vector<lock_queue<std::size_t>> ready_queues_; // C -> P : indices ready for processing, sharded
    std::vector<lock_queue<std::size_t>> free_queues_;  // P -> C : reusable slot indices, sharded
    mutable std::mutex                   resize_mtx_;   // whenever we access segments_ or results_ as whole
    std::unique_ptr<xml_segment[]>       segments_;     // NOLINT(hicpp-avoid-c-arrays)
    std::unique_ptr<segment_result[]>    results_;      // NOLINT(hicpp-avoid-c-arrays)
    // Which shard owns each slot -- set once in acquire_slot(), read (never concurrently with
    // the write) in push_ready()/retrieve_segment() via the happens-before edge each queue's
    // mutex already provides, so a plain array is enough, no atomics needed.
    std::unique_ptr<std::size_t[]> shard_of_slot_; // NOLINT(hicpp-avoid-c-arrays)
    const bool                     log_trace_ = log_.active(logger::level::trace);
    const bool                     log_debug_ = log_.active(logger::level::debug);
    const bool                     log_info_  = log_.active(logger::level::info);
    const bool                     log_warn_  = log_.active(logger::level::warn);
    const bool                     log_error_ = log_.active(logger::level::error);
    const bool                     log_crit_  = log_.active(logger::level::critical);
  };
  // inline segment_pool::segment_pool() { init(); }
  inline segment_pool::segment_pool(const logger::Logger& log, std::size_t no_of_slots, std::size_t num_shards)
  : log_(log)
  , num_shards_(std::max<std::size_t>(1, num_shards))
  , ready_queues_(num_shards_)
  , free_queues_(num_shards_)
  { init(no_of_slots); }
  /////////////////////////////////////////////////////////////////////////////////////////////////
  inline void segment_pool::init(std::size_t capacity)
  {
    std::lock_guard lock(resize_mtx_);
    capacity_ = capacity;
    next_unallocated_slot_.store(0, std::memory_order_relaxed);
    // resize structures
    // segments_.resize(capacity, xml_segment{});
    // results_.resize(capacity, segment_result{0, -1});
    segments_      = std::make_unique_for_overwrite<xml_segment[]>(capacity);    // NOLINT(hicpp-avoid-c-arrays)
    results_       = std::make_unique_for_overwrite<segment_result[]>(capacity); // NOLINT(hicpp-avoid-c-arrays)
    shard_of_slot_ = std::make_unique_for_overwrite<std::size_t[]>(capacity);    // NOLINT(hicpp-avoid-c-arrays)
    //    free_queue_.push_range(std::views::iota(0UL, capacity));
    if (log_info_) log_.info(fmt::format("Pool size: {} ({} shard(s))", capacity_, num_shards_));
  }

  inline std::size_t segment_pool::acquire_slot(std::size_t segment_id)
  {
    const std::size_t shard = segment_id % num_shards_;
    // grab free segment from already allocated ones (this shard's own reuse pool)
    auto        opt = free_queues_[shard].try_pop();
    std::size_t slot;
    if (opt) slot = *opt;
    else
    {
      // grab free segment from the list of unallocated ones (shared across all shards)
      slot = next_unallocated_slot_.fetch_add(1, std::memory_order_relaxed);
      if (slot < capacity_)
      {
        segments_[slot] = xml_segment{};
        results_[slot]  = segment_result{};
      }
      else
      {
        // no free slots - let's wait on this shard's own free queue
        if (log_info_) log_.info(fmt::format("All slots occupied: {}. Waiting...", size()));
        if (free_queues_[shard].pop(slot) != queue_status::active)
          throw std::runtime_error("Internal error: wanted to have free slot, but was interrupted.");
      }
    }
    shard_of_slot_[slot] = shard;
    return slot;
  }

  inline void        segment_pool::push_ready(std::size_t idx) { ready_queues_[shard_of_slot_[idx]].push(idx); }
  inline auto        segment_pool::try_pop_ready(std::size_t shard) { return ready_queues_[shard].try_pop(); }
  inline std::size_t segment_pool::size() const noexcept
  {
    std::lock_guard lock(resize_mtx_);
    return capacity_;
  }
  inline void segment_pool::ready_queue_close()
  {
    for (auto& q : ready_queues_) q.set_finished();
  }
  inline void segment_pool::abort()
  {
    for (auto& q : ready_queues_) q.set_abort();
    for (auto& q : free_queues_) q.set_abort();
  }
  inline queue_status segment_pool::pop_segment_ndx(std::size_t shard, std::size_t& ndx) { return ready_queues_[shard].pop(ndx); }
  /**
   * @brief extract the segment from the pool and free's the pool segment slot
   * It is an error if idx is out of range of current vector
   * @param idx index of the segment in the segment pool
   * @return xml_segment&
   */
  inline xml_segment segment_pool::retrieve_segment(std::size_t ndx)
  {
    results_[ndx] = segment_result{0, -1, -1};  // FIXME ostri check whether we need to have segmetns and results in parallel
                                                //    log_.debug(fmt::format("retrieve before: idx: {} {}", ndx, segments_[ndx].dump()));
    xml_segment seg(std::move(segments_[ndx])); // std::move(segments_.at(idx));
    if (seg.subtree_type() < 0 || seg.length() == 0)
      log_.critical(fmt::format("Retrieved invalid segment from slot {} {})", ndx, seg.dump()));
    //    log_.debug(fmt::format("retrieve after:  idx: {} {}", ndx, seg.dump()));
    //    segments_.at(ndx) = xml_segment{}; // default empty
    free_queues_[shard_of_slot_[ndx]].push(ndx);
    return seg; // with move the segment slot is also reinitiaized
  }
  inline void segment_pool::set_segment(std::size_t ndx, xml_segment seg)
  {
    //    log_.debug(fmt::format("before set segment: ndx: {} {}", ndx, seg.dump()));
    segments_[ndx] = std::move(seg);
    //    log_.debug(fmt::format("after set segment:  ndx: {} {}", ndx, segments_[ndx].dump()));
  }
  inline void           segment_pool::set_result(std::size_t ndx, const segment_result& seg_r) { results_[ndx] = seg_r; }
  inline std::ptrdiff_t segment_pool::ready_queue_size_approx(std::size_t shard) const noexcept
  { return ready_queues_[shard].size_approx(); }
} // namespace fsp
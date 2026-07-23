#pragma once

#include "logger.hpp"
#include "xml_segment.hpp"
#include "segment_result.hpp"
#include "lock_queue.hpp"
#include <vector>
#include <mutex>
#include <atomic>

namespace fsp
{
  using ndx_queue = lock_queue<std::size_t>;
  class segment_pool
  {
  public:
    // segment_pool();
    explicit segment_pool(const fsp_logger& log, std::size_t no_of_slots);
    void                      init(std::size_t capacity = 1024UL * 1024UL * 8UL); // NOLINT(readability-magic-numbers)
    std::size_t               acquire_slot();                                     // get free slot (blocks if none)
    void                      push_ready(std::size_t idx);
    auto                      try_pop_ready();
    [[nodiscard]] std::size_t size() const noexcept;
    void                      ready_queue_close() { ready_queue_.set_finished(); }
    void                      abort()
    {
      ready_queue_.set_abort();
      free_queue_.set_abort();
    }
    queue_status pop_segment_ndx(std::size_t& ndx) { return ready_queue_.pop(ndx); }
    void         set_segment(std::size_t ndx, const xml_segment& seg);
    void         set_result(std::size_t ndx, const segment_result& seg_r);
    xml_segment  retrieve_segment(std::size_t idx);
    std::size_t  ready_queue_size() const { return ready_queue_.size(); }
  private:
    const fsp_logger&           log_;
    std::vector<xml_segment>    segments_;
    std::vector<segment_result> results_;     // parallel to segments
    lock_queue<std::size_t>     ready_queue_; // C -> P : indices ready for processing
    lock_queue<std::size_t>     free_queue_;  // P -> C : reusable slot indices
    std::atomic<std::size_t>    next_id_{0};  // unique segmetn id index
    mutable std::mutex          resize_mtx_;  // whenever we access segments_ or results_ as whole
    const bool                  log_trace_ = log_.active(fsp::lvl_enum::trace);
    const bool                  log_debug_ = log_.active(fsp::lvl_enum::debug);
    const bool                  log_info_  = log_.active(fsp::lvl_enum::info);
    const bool                  log_warn_  = log_.active(fsp::lvl_enum::warn);
    const bool                  log_error_ = log_.active(fsp::lvl_enum::err);
    const bool                  log_crit_  = log_.active(fsp::lvl_enum::crit);
  };
  // inline segment_pool::segment_pool() { init(); }
  inline segment_pool::segment_pool(const fsp_logger& log, std::size_t no_of_slots)
  : log_(log)
  { init(no_of_slots); }
  /////////////////////////////////////////////////////////////////////////////////////////////////
  inline void segment_pool::init(std::size_t capacity)
  {
    std::lock_guard lock(resize_mtx_);
    segments_.reserve(capacity);
    results_.reserve(capacity);

    for (std::size_t i = 0; i < capacity; ++i)
    {
      free_queue_.push(i);
      segments_.emplace_back();
      results_.emplace_back(0, -1);
    }
    if (log_info_) log_.info(fmt::format("Pool size: {}", segments_.size()));
  }

  inline std::size_t segment_pool::acquire_slot()
  {
    auto opt = free_queue_.try_pop();
    if (opt) return *opt;
    // no free slots - let's wait
    if (log_info_) log_.info(fmt::format("All slots occupied: {}. Waiting...", size()));
    std::size_t ndx;
    if (free_queue_.pop(ndx) == queue_status::active) return ndx;
    throw std::runtime_error("Internal error: wanted to have free slot, but was interrupted.");
  }

  inline void        segment_pool::push_ready(std::size_t idx) { ready_queue_.push(idx); }
  inline auto        segment_pool::try_pop_ready() { return ready_queue_.try_pop(); }
  inline std::size_t segment_pool::size() const noexcept
  {
    std::lock_guard lock(resize_mtx_);
    return segments_.size();
  }
  /**
   * @brief extract the segment from the pool and free's the pool segment slot
   * It is an error if idx is out of range of current vector
   * @param idx index of the segment in the segment pool
   * @return xml_segment&
   */
  inline xml_segment segment_pool::retrieve_segment(std::size_t idx)
  {
    results_[idx]   = segment_result{0, -1}; // FIXME ostri check whether we need to have segmetns and results in parallel
    xml_segment seg = std::move(segments_.at(idx));
    free_queue_.push(idx);
    return seg; // with move the segment slot is also reinitiaized
  }
  inline void segment_pool::set_segment(std::size_t ndx, const xml_segment& seg) { segments_.at(ndx) = seg; }
  inline void segment_pool::set_result(std::size_t ndx, const segment_result& seg_r) { results_.at(ndx) = seg_r; }
} // namespace fsp
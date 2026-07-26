// doc_counters.hpp
#pragma once
#include <atomic>
#include <chrono>
#include <string>
#include <fmt/format.h>

namespace fsp
{
  // Per-document runtime facts collected while processing one document: how many of its segments
  // turned out semantically correct vs. in error (via record_segment(), driven by the
  // on_segment_processed hook's verdict), plus end-to-end timing for the cutting phase
  // (open/close) and the segment-processing phase (first/last segment). Replaces the old
  // doc_timing_t, which only tracked timing -- this consolidates timing and outcome counts into
  // one per-document structure. No session-wide totals are kept here or anywhere alongside it:
  // whenever a whole-run total is needed, sum across doc_set_counter once at the end instead of
  // maintaining a separate, contended, shared running counter.
  class doc_counters
  {
  public:
    // Called once by the cutter thread when it starts cutting this document.
    void record_doc_open() noexcept;
    // Called once by the cutter thread when it finishes cutting this document, reporting how
    // many segments it found in total. Returns true if this call is the one that completes the
    // document (i.e. every one of its segments had already been processed by the time cutting
    // finished) -- see record_segment() for the symmetric, more common case.
    bool record_doc_close(std::size_t total_segments) noexcept;

    // Called by whichever P-role thread just finished processing one segment of this document.
    // semantically_ok is the on_segment_processed hook's verdict (true = semantically correct,
    // false = semantic error) -- not to be confused with technical extraction success. Returns
    // true if this call is the one that completes the document (cutting already finished AND
    // this was the last of its segments to be processed).
    bool record_segment(bool semantically_ok) noexcept;

    [[nodiscard]] std::size_t ok() const noexcept;
    [[nodiscard]] std::size_t error() const noexcept;
    [[nodiscard]] std::size_t total() const noexcept; // ok() + error(), never stored separately

    [[nodiscard]] std::chrono::milliseconds processing_doc() const noexcept;  // doc open -> doc close
    [[nodiscard]] std::chrono::milliseconds processing_segs() const noexcept; // first segment -> last segment
    // True end-to-end latency for this document: from the moment cutting started to the moment
    // its last segment was processed. Not the same as processing_doc() (cutting only) or
    // processing_segs() (first-to-last segment span only, which excludes the initial cutting
    // work needed before any segment can even be ready) -- with P threads shared across many
    // concurrently-cutting documents, this can be much longer than either of those on their own.
    [[nodiscard]] std::chrono::milliseconds total_latency() const noexcept; // doc open -> last segment

    // Prints every field as "name: value", one pair per line, each line indented by offs spaces.
    [[nodiscard]] std::string dump(int offs = 0) const;
  private:
    using clock = std::chrono::steady_clock;
    // Shared completion check used by both record_doc_close() and record_segment(), since either
    // one can be the call that satisfies the last remaining condition -- whichever happens later
    // wins the CAS on last_seg_logged_ and is the one that reports completion.
    bool maybe_complete() noexcept;

    std::atomic<std::size_t> ok_{0};
    std::atomic<std::size_t> error_{0};
    std::atomic<std::size_t> expected_total_{0};       // set once, by record_doc_close()
    std::atomic<bool>        cut_finished_{false};     // set once, by record_doc_close()
    std::atomic<bool>        first_seg_logged_{false}; // guards first_seg_ against a double write
    std::atomic<bool>        last_seg_logged_{false};  // guards last_seg_ / completion against firing twice
    clock::time_point        doc_open_;
    clock::time_point        doc_close_;
    clock::time_point        first_seg_;
    clock::time_point        last_seg_;
  };

  inline void doc_counters::record_doc_open() noexcept { doc_open_ = clock::now(); }

  inline bool doc_counters::maybe_complete() noexcept
  {
    if (! cut_finished_.load(std::memory_order_acquire)) return false;
    if (total() < expected_total_.load(std::memory_order_acquire)) return false;
    bool expected = false;
    return last_seg_logged_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
  }

  inline bool doc_counters::record_doc_close(std::size_t total_segments) noexcept
  {
    doc_close_ = clock::now();
    expected_total_.store(total_segments, std::memory_order_relaxed);
    cut_finished_.store(true, std::memory_order_release);
    bool completed = maybe_complete();
    if (completed) last_seg_ = clock::now();
    return completed;
  }

  inline bool doc_counters::record_segment(bool semantically_ok) noexcept
  {
    if (semantically_ok) ok_.fetch_add(1, std::memory_order_relaxed);
    else error_.fetch_add(1, std::memory_order_relaxed);

    bool expected_first = false;
    if (first_seg_logged_.compare_exchange_strong(expected_first, true, std::memory_order_acq_rel)) first_seg_ = clock::now();

    bool completed = maybe_complete();
    if (completed) last_seg_ = clock::now();
    return completed;
  }

  inline std::size_t doc_counters::ok() const noexcept { return ok_.load(std::memory_order_relaxed); }
  inline std::size_t doc_counters::error() const noexcept { return error_.load(std::memory_order_relaxed); }
  inline std::size_t doc_counters::total() const noexcept { return ok() + error(); }

  inline std::chrono::milliseconds doc_counters::processing_doc() const noexcept
  { return std::chrono::duration_cast<std::chrono::milliseconds>(doc_close_ - doc_open_); }

  inline std::chrono::milliseconds doc_counters::processing_segs() const noexcept
  { return std::chrono::duration_cast<std::chrono::milliseconds>(last_seg_ - first_seg_); }

  inline std::chrono::milliseconds doc_counters::total_latency() const noexcept
  { return std::chrono::duration_cast<std::chrono::milliseconds>(last_seg_ - doc_open_); }

  inline std::string doc_counters::dump(int offs) const
  {
    auto leading = std::string(offs, ' ');
    return fmt::format(R"({0} seg.(ok: {1} err: {2} total: {3}) C doc: {4} ms P segs: {5} ms total latency: {6} ms)",
                       leading,
                       ok(),
                       error(),
                       total(),
                       processing_doc().count(),
                       processing_segs().count(),
                       total_latency().count());
  }
} // namespace fsp
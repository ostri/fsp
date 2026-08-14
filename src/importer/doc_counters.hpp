// doc_counters.hpp
#pragma once
#include <atomic>
#include <chrono>
#include <string>
#include <fmt/format.h>

namespace fsp
{
  using str_t = std::string;
  // Per-document runtime facts collected while processing one document: how many of its segments
  // turned out semantically correct vs. in error (via begin_segment()/end_segment(), driven by
  // the on_semantic_check hook's verdict), plus end-to-end timing for the cutting phase
  // (open/close) and the segment-processing phase (first/last segment). Replaces the old
  // doc_timing_t, which only tracked timing -- this consolidates timing and outcome counts into
  // one per-document structure. No session-wide totals are kept here or anywhere alongside it:
  // whenever a whole-run total is needed, sum across doc_set_counter once at the end instead of
  // maintaining a separate, contended, shared running counter.
  class doc_counters
  {
  public:
    // Reported to the caller by begin_segment(), see below.
    struct segment_position
    {
      bool is_first; // NOLINT(misc-non-private-member-variables-in-classes)
      bool is_last;  // NOLINT(misc-non-private-member-variables-in-classes)
    };

    // Called once by the cutter thread when it starts cutting this document.
    void record_doc_open() noexcept;
    // Called once by the cutter thread when it finishes cutting this document, reporting how
    // many segments it found in total. Returns true if this call is the one that completes the
    // document (i.e. every one of its segments had already been processed by the time cutting
    // finished) -- see end_segment() for the symmetric, more common case. This is the ORIGINAL,
    // single-stage "all segments processed" completion condition (cut_finished && total >=
    // expected_total) -- independent of syntax/validation/semantic verdicts, which now live
    // entirely in doc_dscr's own doc_status_t (see doc_dscr.hpp). Whichever call wins is
    // responsible for invoking hooks.on_doc_sem_check() and feeding its bool result into
    // doc_status_t::set_semantic() -- doc_counters itself has no opinion on syntax/validation/
    // semantics, only on "have all segments been accounted for".
    bool record_doc_close(std::size_t total_segments) noexcept;

    // Called by whichever P-role thread is about to process one segment of this document,
    // BEFORE its outcome is known -- this is what lets on_semantic_check's is_first/is_last
    // parameters be ready before the hook call itself. seg_id is the segment's own position in
    // the document (see xml_segment/segment_result), assigned sequentially by the single cutter
    // thread that cut this document -- is_first/is_last are therefore document-order facts
    // (seg_id == 0 / seg_id == last), NOT which P-thread happened to reach this call first/last
    // (P-threads drain a shared queue, so processing order across threads does not follow seg_id
    // order). is_last here is still best-effort in one respect: for a document small/fast enough
    // that every one of its segments gets processed before cutting itself reports done,
    // cut_finished() is still false for every one of them, so none can correctly claim is_last
    // (see end_segment()/maybe_complete() for the authoritative, dual-path completion check used
    // for internal bookkeeping instead).
    [[nodiscard]] segment_position begin_segment(std::size_t seg_id) noexcept;
    // Called once the segment's outcome is known (semantically_ok is the on_semantic_check
    // hook's verdict, or false for a segment that failed technically and never reached the
    // hook at all -- not to be confused with technical extraction success in the ok() case).
    // Returns true if this call is the one that completes the document (cutting already
    // finished AND this was the last of its segments to be processed) -- checked independently
    // of begin_segment()'s is_last, and correct even in the race described above.
    bool end_segment(bool semantically_ok) noexcept;

    [[nodiscard]] std::size_t ok() const noexcept;
    [[nodiscard]] std::size_t error() const noexcept;
    [[nodiscard]] std::size_t total() const noexcept; // ok() + error(), never stored separately
    [[nodiscard]] bool        cut_finished() const noexcept;

    // Segment-processing completion: the ORIGINAL, single, lock-free CAS latch (cut_finished &&
    // total>=expected_total) -- whichever of record_doc_close()/end_segment() satisfies this
    // condition wins the CAS on last_seg_logged_ and is the one that must go on to call
    // hooks.on_doc_sem_check() and feed its bool result into doc_dscr's own
    // doc_status_t::set_semantic() (see doc_dscr.hpp) -- deliberately independent of
    // syntax/validation, which now live entirely in doc_status_t and are reported via
    // doc_dscr::set_syntax_result()/set_validation_result() instead. This is the ONLY completion
    // condition doc_counters itself is responsible for; the "who calls hooks.on_doc_close()"
    // decision is doc_status_t::try_start_closing()'s job alone (see doc_dscr.hpp's own class doc
    // comment on why that single mutex-protected latch replaced this class's former second stage).
    [[nodiscard]] bool maybe_seg_processing_complete() noexcept;

    [[nodiscard]] std::chrono::milliseconds processing_doc() const noexcept;  // doc open -> doc close
    [[nodiscard]] std::chrono::milliseconds processing_segs() const noexcept; // first segment -> last segment
    // True end-to-end latency for this document: from the moment cutting started to the moment
    // its last segment was processed. Not the same as processing_doc() (cutting only) or
    // processing_segs() (first-to-last segment span only, which excludes the initial cutting
    // work needed before any segment can even be ready) -- with P threads shared across many
    // concurrently-cutting documents, this can be much longer than either of those on their own.
    [[nodiscard]] std::chrono::milliseconds total_latency() const noexcept; // doc open -> last segment

    // Prints every field as "name: value", one pair per line, each line indented by offs spaces.
    [[nodiscard]] str_t dump(int offs = 0) const;
  private:
    using clock = std::chrono::steady_clock;

    std::atomic<std::size_t> ok_{0};
    std::atomic<std::size_t> error_{0};
    std::atomic<std::size_t> expected_total_{0};       // set once, by record_doc_close()
    std::atomic<bool>        cut_finished_{false};     // set once, by record_doc_close()
    std::atomic<bool>        first_seg_logged_{false}; // guards first_seg_ against a double write
    std::atomic<bool> last_seg_logged_{false}; // guards last_seg_ / completion against firing twice -- see maybe_seg_processing_complete()
    clock::time_point doc_open_;
    clock::time_point doc_close_;
    clock::time_point first_seg_;
    clock::time_point last_seg_;
  };

  inline void doc_counters::record_doc_open() noexcept { doc_open_ = clock::now(); }

  inline bool doc_counters::maybe_seg_processing_complete() noexcept
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
    const bool completed = maybe_seg_processing_complete();
    if (completed) last_seg_ = clock::now();
    return completed;
  }

  inline doc_counters::segment_position doc_counters::begin_segment(std::size_t seg_id) noexcept
  {
    // first_seg_ timing is chronological (earliest processing activity across all P-threads for
    // this doc), independent of which seg_id that turns out to be -- kept separate from the
    // document-order is_first returned below, which the two used to conflate.
    bool expected_first = false;
    if (first_seg_logged_.compare_exchange_strong(expected_first, true, std::memory_order_acq_rel)) first_seg_ = clock::now();

    const bool is_last = cut_finished_.load(std::memory_order_acquire) && seg_id + 1 == expected_total_.load(std::memory_order_acquire);
    return {.is_first = seg_id == 0, .is_last = is_last};
  }

  inline bool doc_counters::end_segment(bool semantically_ok) noexcept
  {
    if (semantically_ok) ok_.fetch_add(1, std::memory_order_relaxed);
    else error_.fetch_add(1, std::memory_order_relaxed);

    const bool completed = maybe_seg_processing_complete();
    if (completed) last_seg_ = clock::now();
    return completed;
  }

  inline std::size_t doc_counters::ok() const noexcept { return ok_.load(std::memory_order_relaxed); }
  inline std::size_t doc_counters::error() const noexcept { return error_.load(std::memory_order_relaxed); }
  inline std::size_t doc_counters::total() const noexcept { return ok() + error(); }
  inline bool        doc_counters::cut_finished() const noexcept { return cut_finished_.load(std::memory_order_acquire); }

  // doc_close_/last_seg_ are only ever written inside record_doc_close()/the completion branch
  // of end_segment() -- for a document that never completes (e.g. cutting fails partway
  // through, after some segments were already processed), they stay at their default epoch
  // value, which would make these subtractions wildly negative. Guard on the flag that actually
  // gates the write instead of assuming it happened.
  inline std::chrono::milliseconds doc_counters::processing_doc() const noexcept
  {
    if (! cut_finished()) return std::chrono::milliseconds{0};
    return std::chrono::duration_cast<std::chrono::milliseconds>(doc_close_ - doc_open_);
  }

  inline std::chrono::milliseconds doc_counters::processing_segs() const noexcept
  {
    if (! last_seg_logged_.load(std::memory_order_acquire)) return std::chrono::milliseconds{0};
    return std::chrono::duration_cast<std::chrono::milliseconds>(last_seg_ - first_seg_);
  }

  inline std::chrono::milliseconds doc_counters::total_latency() const noexcept
  {
    if (! last_seg_logged_.load(std::memory_order_acquire)) return std::chrono::milliseconds{0};
    return std::chrono::duration_cast<std::chrono::milliseconds>(last_seg_ - doc_open_);
  }

  inline str_t doc_counters::dump(int offs) const
  {
    auto       leading = str_t(offs, ' ');
    const auto kilo    = 1000.0;
    return fmt::format(R"({0} segments [ok: {1:4} err: {2:4} Σ: {3:4}] threads [C: {4:.3f} sec P: {5:.3f} sec Σ: {6:.3f} sec])",
                       leading,
                       ok(),
                       error(),
                       total(),
                       static_cast<double>(processing_doc().count()) / kilo,
                       static_cast<double>(processing_segs().count()) / kilo,
                       static_cast<double>(total_latency().count()) / kilo);
  }
} // namespace fsp
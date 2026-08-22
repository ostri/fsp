// doc_dscr.hpp
#pragma once
#include "error_info.hpp"
#include "mmap_file.hpp"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>

namespace fsp
{
  /**
   * @brief Three-way verdict for one of doc_status_t's four tracked facts (syntax/validation/
   * semantic/stored) -- distinct from a plain bool so "nobody has reported on this yet" (unknown)
   * can never be confused with "reported, and it's bad" (invalid). unknown is the ONLY valid
   * initial state: every doc_status_t field starts here and is written at most once, by
   * set_syntax()/set_valid()/set_semantic()/set_stored() respectively (see their own doc comments)
   * -- there is no path back from valid/invalid to unknown.
   */
  enum class three_state : std::uint8_t
  {
    unknown, //< nobody has reported a verdict for this fact yet (not the same as "known bad")
    valid,   //< reported, and the verdict was positive
    invalid  //< reported, and the verdict was negative
  };

  /**
   * @brief Which specific error class rejected a document -- a finer-grained, orthogonal
   * complement to doc_status_t's own syntax_/valid_/semantic_ three_state facts, not a
   * replacement for them. status()/ok() and the individual _status() getters are computed purely
   * from the three three_state facts, exactly as before this enum existed; error_mask() (see
   * doc_status_t below) is an independent, additional record of WHICH class of failure a caller
   * (typically a hook wanting to log/route differently, or a future on_remove_stored_data() call
   * site) can ask about, without needing three_state's own valid/invalid/unknown distinction --
   * a bit here is either set (this class of error definitely happened) or not (it didn't, or
   * hasn't been determined not to have -- same ambiguity status()==three_state::unknown already
   * carries for the aggregate, not something this mask needs to re-encode itself). See
   * docs/importer_usage.md's own "Document errors" section for the full UA/SE/VE/HE/TE taxonomy
   * this mirrors.
   */
  enum class error_class : std::uint8_t
  {
    ua = 0x01, //< unknown agent -- pipeline_hooks::get_doc_agent_id() resolved to 0
    se = 0x02, //< syntax error -- the document is not XML-well-formed
    ve = 0x04, //< validation error -- the document failed XSD validation
    he = 0x08, //< header semantic error -- on_type() returned false for the header segment
    te = 0x10, //< transaction semantic error -- on_type() returned false for a non-header segment
  };

  /**
   * @brief Self-contained, mutex-protected final syntax/validation/semantic/stored verdict for one
   * document -- owns BOTH the four individual three_state facts (syntax_/valid_/semantic_/
   * stored_) AND the "have all four been reported yet, and who gets to act on that" completion
   * logic (done_/closing_/is_finished()/try_start_closing()), replacing the old doc_status_t (a
   * bare 3-bool struct) plus doc_counters' own second-stage CAS latch that used to gate
   * on_doc_close(). stored_ is a fourth, later addition (see set_stored()'s own doc comment) --
   * unlike the original three, it has no failure state of its own (there is no "storage failed"
   * verdict fsp-core reports here; on_doc_stored() firing at all IS the fact) and is therefore not
   * surfaced in status()/ok()'s aggregate the way syntax_/valid_/semantic_ are -- it exists purely
   * to gate WHEN on_doc_close() may fire, not to influence WHETHER the document's own verdict is
   * positive.
   *
   * Why one mutex instead of the earlier design's separate atomics (one per field) plus
   * hand-rolled acquire/release ordering between a completion counter and those fields: the
   * earlier, two-stage-CAS design needed careful memory_order reasoning to guarantee that a
   * thread which observed "all three fields are known" also observed the actual VALUES of those
   * three fields (an acquire/release pair per field, plus a separate CAS latch to decide who acts
   * on the completion) -- easy to get subtly wrong, and exactly the kind of manual ordering this
   * class exists to make unnecessary. Guarding all five members (syntax_/valid_/semantic_/done_/
   * closing_) with the SAME mutex_ means every method that touches more than one of them (in
   * particular set_*()'s "update my field, then maybe become the closer" combo, and
   * status()'s "read all three together") executes as one atomic critical section -- no
   * cross-field ordering to reason about, at the cost of a real lock instead of a lock-free CAS.
   * Acceptable here: these calls happen at most 3 times per document (once each for syntax/
   * valid/semantic), not per-segment, so contention/latency is negligible compared to the
   * per-segment hot path (on_seg_sem_check()) that stays lock-free elsewhere in fsp.
   */
  class doc_status_t
  {
  public:
    doc_status_t() = default;
    // Move-only, and only ever safe/used while the source is exclusively owned by a single
    // thread and not yet shared (e.g. doc_set_dscr's std::vector<doc_dscr> growing/relocating
    // during pipeline::add_documents(), strictly before any worker thread starts -- same
    // happens-before argument as doc_dscr::out_doc_id_'s own doc comment). Delegates to the
    // private snapshot ctor below, which locks o's mutex while reading its fields into a plain
    // aggregate FIRST, then move-constructs *this from that snapshot in the member-initializer
    // list -- well-defined even under generic (not fsp-specific) container machinery that might,
    // in principle, call this concurrently with a read of o; fsp itself never actually does that.
    doc_status_t(doc_status_t&& o) noexcept
    : doc_status_t(snapshot(o))
    {
    }
    doc_status_t& operator=(doc_status_t&& o) noexcept
    {
      if (this == &o) return *this;
      const std::scoped_lock lock(mtx_, o.mtx_);
      syntax_   = o.syntax_;
      valid_    = o.valid_;
      semantic_ = o.semantic_;
      stored_   = o.stored_;
      done_     = o.done_;
      closing_  = o.closing_;
      // error_mask_/rejected_flag_ are their own atomics, outside mtx_'s own critical section
      // (see their own doc comments) -- copied here too, same "source exclusively owned, not yet
      // shared" precondition as the rest of this move assignment, so a relaxed load/store is fine.
      error_mask_.store(o.error_mask_.load(std::memory_order_relaxed), std::memory_order_relaxed);
      rejected_flag_.store(o.rejected_flag_.load(std::memory_order_relaxed), std::memory_order_relaxed);
      return *this;
    }
    doc_status_t(const doc_status_t&)            = delete;
    doc_status_t& operator=(const doc_status_t&) = delete;
    ~doc_status_t()                              = default;

    /**
     * @brief Reported by C (the cutter), or by C alone in folded cut_with_validation=true mode
     * (see doc_dscr::set_syntax_result()'s old doc comment for the exact folded-vs-separate-V
     * rule, now applied by whichever worker calls this) -- exactly once per document. ok=true
     * moves syntax_ from unknown to valid; ok=false moves it to invalid. Either way, immediately
     * (same critical section) attempts try_start_closing() -- see that method's own doc comment
     * for why this is the ONLY place a caller needs to check its own result to know whether it
     * must go on to call hooks.on_doc_close().
     * @return true if THIS call is the one that must call hooks.on_doc_close() next (see
     * try_start_closing()) -- false otherwise (someone else already claimed that job, or won't
     * be able to until the remaining fact(s) are also known).
     */
    [[nodiscard]] bool set_syntax(bool ok) noexcept { return set_field(syntax_, ok); }
    /**
     * @brief Reported by V (a separate validation pass) once doc_validator::validate() returns,
     * exactly once per document, only ever called when this run's cut_with_validation is false
     * (folded mode instead routes its single verdict through set_syntax() for both facts -- see
     * pipeline_worker::do_cut()). Same true/false -> valid/invalid -> try_start_closing() pattern
     * as set_syntax() above; same return-value meaning.
     */
    [[nodiscard]] bool set_valid(bool ok) noexcept { return set_field(valid_, ok); }
    /**
     * @brief Reported by the fsp-core worker that wins doc_counters' (unchanged, original,
     * single-stage) "all segments processed" completion check, immediately after it calls
     * hooks.on_doc_sem_check(doc_ndx, doc_data) and gets that hook's bool verdict back -- a cb
     * itself never calls this (on_doc_sem_check() only ever returns a bool, see
     * pipeline_hooks.hpp). Exactly once per document. Same true/false -> valid/invalid ->
     * try_start_closing() pattern as set_syntax()/set_valid() above; same return-value meaning.
     */
    [[nodiscard]] bool set_semantic(bool ok) noexcept { return set_field(semantic_, ok); }
    /**
     * @brief Reported by whichever worker's flush_ok_block()/flush_nak_block() call is the one
     * that pushes this document's own "segments stored" count (see doc_counters' stored_ member)
     * up to its known total (see pipeline::record_segments_stored()) -- exactly once per document.
     * Unlike set_syntax()/set_valid()/set_semantic() above, there is no ok parameter: storage
     * completeness has no failure verdict of its own to report (a segment that genuinely failed to
     * be written would surface as an on_block_store()/on_failed_block_store() error propagated
     * from xml_worker::flush_ok_block()/flush_nak_block() itself, well before this point -- see
     * pipeline_hooks.hpp's on_block_safe_store()'s own doc comment), so this call always marks
     * stored_ three_state::valid. Same try_start_closing()-attempt-under-the-same-lock pattern as
     * set_syntax()/set_valid()/set_semantic(); same return-value meaning.
     */
    [[nodiscard]] bool set_stored() noexcept { return set_field(stored_, true); }

    /**
     * @brief True once every one of the four facts this object tracks is either known (all
     * four reported, whatever their individual verdicts) or the outcome is already decided
     * beyond doubt (ANY one of them came back invalid -- see set_field()'s own doc comment on why
     * a single invalid report short-circuits waiting on the others). Monotonic: once true,
     * stays true for this object's lifetime.
     */
    [[nodiscard]] bool is_finished() const noexcept
    {
      const std::scoped_lock lock(mtx_);
      return done_ >= k_done_threshold;
    }

    /**
     * @brief The ONE-SHOT "who gets to call hooks.on_doc_close()" decision for this document --
     * the single, unified replacement for the old design's two separate CAS latches (segment-
     * processing completion stage + syntax/validation/semantic completion stage). Every one of
     * set_syntax()/set_valid()/set_semantic() calls this internally, under the SAME mtx_ that
     * guards done_/closing_, immediately after updating its own field -- so there is no window
     * between "the last fact became known" and "we decided who announces it" for another thread
     * to observe. Returns true to EXACTLY ONE caller, ever, for a given doc_status_t: the first
     * call that finds is_finished()'s condition already true AND closing_ still false. That
     * caller, and only that caller, is thereafter responsible for invoking
     * hooks.on_doc_close(doc_ndx, *this, err, dscr) -- every other caller (including ones that
     * arrive after the winner, or that themselves complete the LAST fact but lose the race to
     * another thread already inside this critical section) must NOT call on_doc_close() itself.
     * Safe to call again after the first true (e.g. defensively) -- it simply keeps returning
     * false, since closing_ is sticky.
     */
    [[nodiscard]] bool try_start_closing() noexcept
    {
      const std::scoped_lock lock(mtx_);
      if (done_ < k_done_threshold || closing_) return false;
      closing_ = true;
      return true;
    }

    /**
     * @brief Aggregate verdict across the three VERDICT-bearing facts (syntax_/valid_/semantic_):
     * valid iff all three are three_state::valid; invalid iff AT LEAST ONE of them is
     * three_state::invalid; three_state::unknown otherwise (i.e. at least one fact still
     * unreported and none of the reported ones failed yet). Deliberately does NOT consider stored_
     * -- storage-completeness is a GATE on when on_doc_close() may fire (see try_start_closing()),
     * not a fourth pass/fail dimension of the document's own verdict (see stored_'s own doc
     * comment on the class above). In practice, callers only ever observe this from inside
     * hooks.on_doc_close(), which fsp-core only calls once is_finished() is already true -- so
     * three_state::unknown should never actually be seen there, but this method doesn't special-
     * case that away (it's a well-defined answer for a not-yet-finished object too, e.g. for
     * diagnostics/logging mid-run).
     */
    [[nodiscard]] three_state status() const noexcept
    {
      const std::scoped_lock lock(mtx_);
      if (syntax_ == three_state::invalid || valid_ == three_state::invalid || semantic_ == three_state::invalid)
        return three_state::invalid;
      if (syntax_ == three_state::valid && valid_ == three_state::valid && semantic_ == three_state::valid) return three_state::valid;
      return three_state::unknown;
    }

    /**
     * @brief Convenience over status(): true iff status() == three_state::valid, i.e. syntax AND
     * validation AND doc-level semantics all explicitly passed. Mirrors the old (pre-round-5)
     * doc_status_t::ok()'s meaning for callers that only care about the single pass/fail bit.
     */
    [[nodiscard]] bool ok() const noexcept { return status() == three_state::valid; }

    /**
     * @brief Partial status getters -- for a cb's on_doc_close() override that wants to know
     * WHICH of the tracked facts specifically failed, not just that the aggregate status() is
     * three_state::invalid (e.g. to log/route differently for a syntax problem vs. a doc-level
     * semantic one). Each simply returns that one field's current three_state under the lock; no
     * aggregation applied. stored_status() is included for symmetry/diagnostics even though
     * status()/ok() above never consult it (see stored_'s own doc comment on the class above) --
     * it can only ever read three_state::unknown or three_state::valid, never invalid.
     */
    [[nodiscard]] three_state syntax_status() const noexcept
    {
      const std::scoped_lock lock(mtx_);
      return syntax_;
    }
    [[nodiscard]] three_state valid_status() const noexcept
    {
      const std::scoped_lock lock(mtx_);
      return valid_;
    }
    [[nodiscard]] three_state semantic_status() const noexcept
    {
      const std::scoped_lock lock(mtx_);
      return semantic_;
    }
    [[nodiscard]] three_state stored_status() const noexcept
    {
      const std::scoped_lock lock(mtx_);
      return stored_;
    }

    /**
     * @brief Records that this document was rejected for reason cls -- OR's cls's own bit into
     * error_mask_ (see error_class's own doc comment for why this is additional information, not
     * a replacement for the three_state facts above). Callers may call this more than once for
     * the same document (e.g. a document that is both syntactically invalid AND, independently,
     * resolves to an unknown agent) -- unlike set_syntax()/set_valid()/set_semantic(), which are
     * each exactly-once-per-document, error_mask_ can accumulate more than one bit over a
     * document's lifetime. Lock-free: error_mask_ is its own atomic, independent of mtx_ (nothing
     * reads error_mask_ together with syntax_/valid_/semantic_/stored_ as one consistent
     * snapshot, so there is no cross-field ordering to protect here the way set_field() has to for
     * those four). Deliberately does NOT touch rejected_flag_ itself -- HE/TE (a single segment's
     * own semantic failure) must NOT make rejected() true, only UA/SE/VE (the whole document is
     * unusable) may -- see mark_rejected() below, which a caller opts into separately for exactly
     * those three classes.
     * @return true iff THIS call is the one that moved error_mask_ from empty (0) to non-empty --
     * i.e. the first time this document has EVER been marked with any error class at all. Same
     * "exactly one winner" shape as try_start_closing(), but simpler: fetch_or()'s own return
     * value already tells the caller everything needed (was error_mask_ zero right before this
     * call's own bit landed), no separate lock or latch required. A caller (pipeline.cpp) uses
     * this to fire on_remove_stored_data_safe() exactly once per document -- see its own doc
     * comment in pipeline_hooks.hpp.
     */
    [[nodiscard]] bool mark_error(error_class cls) noexcept
    { return error_mask_.fetch_or(static_cast<std::uint8_t>(cls), std::memory_order_relaxed) == 0; }

    /**
     * @brief Stamps rejected_flag_ true early, for the whole-document error classes (UA/SE/VE) --
     * same one-way store set_field() itself already performs the moment syntax_/valid_/semantic_
     * first turns invalid, but needed HERE too for those three classes specifically: every one of
     * their own call sites in pipeline_worker.cpp calls report_error_class() (which calls
     * mark_error() above, then dispatches on_remove_stored_data_safe()) BEFORE the corresponding
     * report_syntax_result()/report_validation_result() -- the call that would otherwise be what
     * first sets rejected_flag_ via set_field(). Without this, there is a real window, between
     * mark_error() returning (error_mask_ already non-empty, on_remove_stored_data_safe() already
     * dispatched and possibly already returned) and that later call actually landing, during which
     * rejected() still reads false -- a P-role thread's own xml_worker::process_one() rejected()
     * check would not yet skip this document's segments, so one could still be written to storage
     * in that window and never get cleaned up afterward (confirmed directly: a small-buffer
     * end-to-end scenario flushing one segment at a time reproduced this non-deterministically,
     * roughly 1 run in 5, before this fix). NOT called for HE/TE (see mark_error()'s own doc
     * comment on why those must leave rejected() alone) -- pipeline::report_error_class()'s own
     * no_headers parameter is exactly "is this UA/SE/VE (false) or HE (true)", so it is the one
     * caller in a position to know which of the two applies here.
     */
    void mark_rejected() noexcept { rejected_flag_.store(true, std::memory_order_relaxed); }
    /// @brief The raw accumulated bitmask -- see error_class's own doc comment for the bit layout.
    [[nodiscard]] std::uint8_t error_mask() const noexcept { return error_mask_.load(std::memory_order_relaxed); }
    /// @brief Convenience over error_mask(): true iff cls's own bit has been recorded via mark_error().
    [[nodiscard]] bool has_error(error_class cls) const noexcept
    { return (error_mask() & static_cast<std::uint8_t>(cls)) != 0; }

    /**
     * @brief Cheap, lock-free equivalent of status() == three_state::invalid -- see
     * doc_dscr::rejected()'s own doc comment for why this exists and where it's used (a
     * per-segment hot-path check, at 10M-segment scale, that would otherwise pay status()'s own
     * mtx_ lock on every single call just to answer a question that is "no" the overwhelming
     * majority of the time). Set (relaxed store, one-way: never reset) from inside set_field()
     * itself, in the SAME call that first drives one of syntax_/valid_/semantic_ to invalid -- so
     * this is never observably stale relative to status() for a caller that only needs the
     * one-bit answer, just cheaper to read. status() itself is untouched and remains the
     * authority for callers that need the full three-way three_state (e.g. on_doc_close()).
     */
    [[nodiscard]] bool rejected() const noexcept { return rejected_flag_.load(std::memory_order_relaxed); }
  private:
    // Plain, lock-free aggregate holding a snapshot of the six fields below -- exists solely so
    // the move ctor above can lock o's mutex, read its fields into one of these (see snapshot()),
    // and THEN move-construct *this from it in the member-initializer list, satisfying
    // cppcoreguidelines-prefer-member-initializer without ever reading o's fields unlocked.
    struct fields
    {
      three_state   syntax_;
      three_state   valid_;
      three_state   semantic_;
      three_state   stored_;
      int           done_;
      bool          closing_;
      std::uint8_t  error_mask_;   //< snapshot of the atomic error_mask_ (see its own doc comment)
      bool          rejected_flag_; //< snapshot of the atomic rejected_flag_ (see its own doc comment)
    };
    // Builds a fields snapshot of o under o's own lock -- see the move ctor's own doc comment.
    // error_mask_/rejected_flag_ are their own atomics, not guarded by mtx_ (see their own doc
    // comments) -- read here with a relaxed load, same "source exclusively owned, not yet shared"
    // precondition the move ctor itself already documents.
    [[nodiscard]] static fields snapshot(doc_status_t& o) noexcept
    {
      const std::scoped_lock lock(o.mtx_);
      return {.syntax_         = o.syntax_,
              .valid_          = o.valid_,
              .semantic_       = o.semantic_,
              .stored_         = o.stored_,
              .done_           = o.done_,
              .closing_        = o.closing_,
              .error_mask_     = o.error_mask_.load(std::memory_order_relaxed),
              .rejected_flag_  = o.rejected_flag_.load(std::memory_order_relaxed)};
    }
    // Private, snapshot-based delegate target for the move ctor -- mtx_ itself is deliberately
    // NOT part of fields (std::mutex isn't copyable/movable), so it default-constructs here as a
    // fresh, unlocked mutex for the newly-constructed object, exactly as doc_status_t's own
    // default member initializer would.
    explicit doc_status_t(const fields& f) noexcept
    : syntax_(f.syntax_)
    , valid_(f.valid_)
    , semantic_(f.semantic_)
    , stored_(f.stored_)
    , done_(f.done_)
    , closing_(f.closing_)
    , error_mask_(f.error_mask_)
    , rejected_flag_(f.rejected_flag_)
    {
    }

    // done_ reaches this value exactly when the object is finished: either all four facts were
    // individually reported (each set_*() call increments done_ by 1 on a valid verdict, so four
    // valid reports sum to 4), or a single invalid report jumps done_ straight to 4 (see
    // set_field()'s own doc comment) -- both cases compare equal against this same threshold. Note
    // stored_ can only ever contribute a valid report (see set_stored()'s own doc comment), so in
    // practice only syntax_/valid_/semantic_ can ever trigger the invalid short-circuit below.
    static constexpr int k_done_threshold = 4;

    /**
     * @brief Shared body for set_syntax()/set_valid()/set_semantic()/set_stored() -- updates field
     * (one of syntax_/valid_/semantic_/stored_) from three_state::unknown to three_state::valid or
     * three_state::invalid depending on ok, adjusts done_, and (still under the same lock)
     * attempts try_start_closing()'s decision. On ok=true: field becomes valid, done_ +=1 (one
     * more of the four facts is now known-good). On ok=false: field becomes invalid, done_ is
     * set to k_done_threshold (4) OUTRIGHT rather than incremented -- this is the "short-circuit"
     * that lets is_finished()/try_start_closing() fire as soon as ANY single fact is known bad,
     * without waiting for the others to be reported at all (mirrors the design discussion's
     * round-1 point 7 / "discard already-doomed documents ASAP": once one fact is invalid, the
     * document's overall verdict is already decided -- status() will report invalid regardless of
     * what the still-unreported fact(s) would have said, so there is nothing to gain by waiting
     * for them and every reason not to hold up on_doc_close() until a fact that can no longer
     * change the outcome eventually reports in). set_stored() never passes ok=false (see its own
     * doc comment), so this short-circuit is, in practice, only ever reachable via syntax_/valid_/
     * semantic_.
     * @return whatever try_start_closing()'s own critical section decided for this call -- see
     * its doc comment.
     */
    [[nodiscard]] bool set_field(three_state& field, bool ok) noexcept
    {
      {
        const std::scoped_lock lock(mtx_);
        field = ok ? three_state::valid : three_state::invalid;
        done_ = ok ? done_ + 1 : k_done_threshold;
        // rejected()'s own fast path -- see its doc comment. Set here, under the same lock, the
        // FIRST time any of the three verdict-bearing facts turns invalid -- one-way (never reset
        // back to false), so a later valid report on a DIFFERENT field cannot un-set it.
        if (! ok) rejected_flag_.store(true, std::memory_order_relaxed);
        if (done_ >= k_done_threshold && ! closing_)
        {
          closing_ = true;
          return true;
        }
        return false;
      }
    }

    // --- the four tracked facts -- each written exactly once, by set_syntax()/set_valid()/
    // set_semantic()/set_stored() respectively (see their own doc comments for which fsp-core role
    // writes which: C for syntax_, V (or C in folded mode) for valid_, the worker orchestrating
    // on_doc_sem_check() for semantic_, whichever worker's flush_ok_block()/flush_nak_block() call
    // crosses this document's segment-stored total for stored_ -- see
    // pipeline::record_segments_stored()). All four start unknown and only ever move to valid or
    // invalid, never back (stored_ in practice only ever reaches valid, never invalid -- see
    // set_stored()'s own doc comment). ---
    three_state syntax_{three_state::unknown};   //< set by set_syntax(), reported by C
    three_state valid_{three_state::unknown};    //< set by set_valid(), reported by V (or C, folded mode)
    three_state semantic_{three_state::unknown}; //< set by set_semantic(), reported by the on_doc_sem_check() orchestrator
    three_state stored_{three_state::unknown};   //< set by set_stored(), reported by pipeline::record_segments_stored()
    // Count of "this fact's outcome can no longer change the final verdict" -- reaches
    // k_done_threshold (4) either via four individual valid reports (one increment each) or a
    // single invalid report (jumps straight to 4, see set_field()). Guarded by mtx_ together with
    // closing_ so is_finished()/try_start_closing() never observe one without the other.
    int done_ = 0;
    // One-shot latch: true once try_start_closing() has handed out its single "true" answer.
    // Guarded by mtx_ -- see try_start_closing()'s own doc comment for why this must be decided
    // in the SAME critical section as the done_ >= k_done_threshold check, not as a separate,
    // later CAS the way the old two-stage design did it.
    bool closing_ = false;
    // Deliberately its own atomic, NOT one of the six fields guarded by mtx_ above -- see
    // mark_error()'s own doc comment for why this needs no cross-field ordering with
    // syntax_/valid_/semantic_/stored_/done_/closing_.
    std::atomic<std::uint8_t> error_mask_{0}; //< see error_class's own doc comment for the bit layout
    // Deliberately its own atomic too -- see rejected()'s own doc comment for why this is a
    // separate, lock-free fast path rather than a seventh field folded into mtx_'s own snapshot.
    std::atomic<bool> rejected_flag_{false};
    mutable std::mutex
      mtx_; //< guards every one of the six members above; see class's own doc comment on why one mutex, not per-field atomics
  };

  class doc_dscr
  {
  public:
    doc_dscr() = default;
    explicit doc_dscr(cstr_t path);
    explicit doc_dscr(mmap_file&& file);
    ~doc_dscr();
    // Copy/move semantics
    doc_dscr(const doc_dscr&)            = delete;
    doc_dscr& operator=(const doc_dscr&) = delete;
    doc_dscr(doc_dscr&& o) noexcept;
    doc_dscr&                                operator=(doc_dscr&& o) noexcept;
    void                                     close() noexcept;
    [[nodiscard]] bool                       is_open() const noexcept;
    [[nodiscard]] bool                       empty() const noexcept;
    [[nodiscard]] size_t                     size() const noexcept;
    [[nodiscard]] const std::byte*           data() const noexcept;
    [[nodiscard]] cstr_t                     path() const;
    [[nodiscard]] cstr_t                     string_view() const;
    [[nodiscard]] std::byte                  operator[](size_t pos) const;
    [[nodiscard]] std::byte                  at(size_t pos) const;
    [[nodiscard]] auto                       begin() const noexcept;
    [[nodiscard]] auto                       end() const noexcept;
    [[nodiscard]] auto                       cbegin() const noexcept;
    [[nodiscard]] auto                       cend() const noexcept;
    [[nodiscard]] std::span<const std::byte> span() const noexcept;
    [[nodiscard]] std::span<const std::byte> subspan(size_t offset, size_t count) const;
    void                                     prefetch(size_t offset, size_t count = mmap_file::prefetch_size) const noexcept;
    [[nodiscard]] explicit                   operator bool() const noexcept;
    [[nodiscard]] const mmap_file&           mmf() const noexcept;
    mmap_file&                               mmf() noexcept;
    // The live doc_status_t itself -- doc_status_t is deliberately non-copyable (it owns a
    // std::mutex), so unlike the old bare-3-bool doc_status_t this returns a reference to the
    // ONE instance living inside this doc_dscr, not a snapshot copy. A caller that only wants a
    // point-in-time aggregate reads status().status()/status().ok(); a caller orchestrating the
    // C/V/semantic completion race (pipeline.cpp) calls status().set_syntax()/set_valid()/
    // set_semantic() (via set_syntax_result()/set_validation_result()/set_semantic_result() below,
    // which apply this class's own folded/failure-propagation rules first) and inspects the
    // returned bool to learn whether IT is the one that must call hooks.on_doc_close().
    [[nodiscard]] doc_status_t&       status() noexcept { return status_; }
    [[nodiscard]] const doc_status_t& status() const noexcept { return status_; }
    // True once status().status() == three_state::invalid, i.e. at least one of syntax/
    // validation is already known bad -- distinct from a field still being three_state::unknown
    // (not yet reported). Used by pipeline_worker::do_cut()'s and xml_worker::process_one()'s
    // "already known invalid, skip the work" precondition (point 6 of the design discussion this
    // implements) -- semantic_ deliberately does NOT influence this (a document isn't "known bad"
    // for C/P's purposes just because on_doc_sem_check() hasn't run yet, and semantic_ing alone
    // failing doesn't retroactively make earlier-cut segments worth discarding).
    [[nodiscard]] bool failed() const noexcept
    { return status_.syntax_status() == three_state::invalid || status_.valid_status() == three_state::invalid; }
    // True once status().status() == three_state::invalid, i.e. ANY of syntax/validation/semantic
    // is already known bad -- unlike failed() above, semantic_ DOES count here. Not usable for
    // failed()'s own "skip the work" precondition (a not-yet-processed segment's document can't
    // possibly have a semantic verdict yet -- on_doc_sem_check() only runs once every segment is
    // already accounted for, see doc_counters::maybe_seg_processing_complete()), but exactly the
    // predicate a P-role thread's flush_ok_block()/flush_nak_block() needs for the OPPOSITE
    // direction: a segment that already finished processing (is sitting in ok_block_indices_/
    // nak_block_indices_, not yet flushed) whose own document has SINCE been rejected -- on
    // whichever fact, including a semantic one discovered only after this segment's own
    // processing completed -- is worth dropping before it it ever reaches storage, since
    // hooks.on_doc_close() will reject the whole document anyway and a cb's own docs/msgs/orders
    // schema (see e.g. ach's own docs/ach-operation/negative-tests.md) may not want segments of a
    // rejected document persisted at all. See xml_worker::flush_ok_block()'s own doc comment for
    // where this is actually used.
    //
    // Reads doc_status_t::rejected() (a lock-free atomic flag), NOT status().status() -- the two
    // are always equivalent (see doc_status_t::rejected()'s own doc comment on when the flag is
    // set), but this call site is xml_worker::process_one()'s own per-segment hot path, at
    // 10M-segment scale, where status()'s std::scoped_lock would otherwise be paid on every single
    // segment just to answer a question that is "no" the overwhelming majority of the time.
    [[nodiscard]] bool rejected() const noexcept { return status_.rejected(); }
    // Thin forwarders to status_'s own mark_error()/error_mask()/has_error() -- see their doc
    // comments on doc_status_t. Kept here too, same "convenience wrapper over status_" pattern as
    // set_semantic_result()/set_stored_result() above, so call sites don't need to spell out
    // .status().mark_error(...) themselves.
    [[nodiscard]] bool          mark_error(error_class cls) noexcept { return status_.mark_error(cls); }
    void                        mark_rejected() noexcept { status_.mark_rejected(); }
    [[nodiscard]] std::uint8_t  error_mask() const noexcept { return status_.error_mask(); }
    [[nodiscard]] bool          has_error(error_class cls) const noexcept { return status_.has_error(cls); }
    // Reported by C (the cutter) once cutting finishes. folded_validation is
    // importer_config::cut_with_validation's effective value for this run (see pipeline_worker.cpp) --
    // when true, C is the SOLE authority for both syntax and validation (success sets both valid,
    // failure sets both invalid, via two calls into status_ -- see doc_status_t::set_syntax()/
    // set_valid()); when false (separate V pass), C only ever sets syntax on success (validation
    // is V's job, see set_validation_result() below) but still drags BOTH invalid on failure -- an
    // ill-formed document can't be "schema-valid" either.
    // @return true if THIS call is the one that must call hooks.on_doc_close() next (see
    // doc_status_t::try_start_closing()) -- i.e. either of the (at most two) underlying set_*()
    // calls this makes won that one-shot race.
    [[nodiscard]] bool set_syntax_result(bool ok, bool folded_validation, error_info err = {}) noexcept;
    // Reported by V (a SEPARATE validation pass, only ever called when folded_validation above is
    // false). Success sets validation valid only (syntax, already set by C, is untouched). Failure
    // drags BOTH invalid -- a document V rejects can't be called well-formed either. Same
    // return-value meaning as set_syntax_result() above.
    [[nodiscard]] bool set_validation_result(bool ok, error_info err = {}) noexcept;
    // Reported by the fsp-core worker that wins doc_counters' (unchanged, original, single-stage)
    // "all segments processed" completion check, immediately after calling hooks.on_doc_sem_check()
    // and getting its bool verdict back -- a cb never calls this directly, see
    // doc_status_t::set_semantic()'s own doc comment. Same return-value meaning as
    // set_syntax_result()/set_validation_result() above.
    [[nodiscard]] bool set_semantic_result(bool ok) noexcept { return status_.set_semantic(ok); }
    // Reported by pipeline::record_segments_stored() once this document's own "segments stored"
    // count (see doc_counters' stored_ member) reaches its known total -- a cb never calls this
    // directly, see doc_status_t::set_stored()'s own doc comment on why it takes no ok parameter.
    // Same return-value meaning as set_syntax_result()/set_validation_result()/
    // set_semantic_result() above.
    [[nodiscard]] bool              set_stored_result() noexcept { return status_.set_stored(); }
    [[nodiscard]] const error_info& error() const noexcept;
    /**
     * @brief The caller-assigned output document id (see pipeline_hooks::get_doc_id()) --
     * plain, non-atomic: written exactly once, from the main thread, in
     * doc_set_dscr::add_document(), before this doc_dscr is shared with any worker thread (see
     * pipeline::add_documents(), called before any std::jthread is started). Every later read
     * (from any worker thread, once running) is therefore safe without synchronization -- the
     * std::jthread launch itself is the happens-before edge.
     */
    [[nodiscard]] std::uint64_t out_doc_id() const noexcept { return out_doc_id_; }
    void                        set_out_doc_id(std::uint64_t id) noexcept { out_doc_id_ = id; }
    /**
     * @brief Caller-assigned agent id for this document, resolved from whatever a hook's own
     * get_doc_agent_id() implementation derives it from (e.g. a BIC4 prefix in the document's own
     * filename today; a public key or some other document property later) -- fsp itself never
     * inspects or interprets this value, only carries it (same "opaque payload, domain-neutral
     * pipeline" contract as out_doc_id() above), except for one convention it DOES act on: 0 means
     * "unresolved agent" (pipeline_worker::do_cut()/do_validate() reject such a document before any
     * cut/validate work -- see their own doc comments). An unoverridden get_doc_agent_id() returns 0
     * (see pipeline_hooks.hpp's own doc comment on why that, not std::nullopt, is the safe default),
     * so agent_id() is 0 -- not unset -- unless a hook override resolves a real, non-zero id.
     * std::nullopt itself is reserved for a hook that overrides this and, on its own terms, cannot
     * decide the agent for a particular path at all -- distinct from "decided: unresolved" (0); the
     * agent_id()==0 rejection above does not fire for an unset (nullopt) agent_id().
     *
     * Same happens-before/thread-safety argument as out_doc_id(): plain, non-atomic, written
     * exactly once, from the main thread, in pipeline::add_documents(), before this doc_dscr is
     * shared with any worker thread -- every later read is therefore safe without synchronization.
     */
    [[nodiscard]] std::optional<std::int16_t> agent_id() const noexcept { return agent_id_; }
    void                                      set_agent_id(std::optional<std::int16_t> id) noexcept { agent_id_ = id; }

    /**
     * @brief True once pipeline_worker::do_cut() has called hooks.on_doc_safe_open() (and that
     * call has RETURNED) for this document - false until then. C and V are seeded into their own
     * queues independently (pipeline::seed_queues()) and can run on different threads with no
     * ordering between them, so a fast-failing V pass can otherwise call report_validation_result()
     * (and, if it wins doc_status_t::try_start_closing(), hooks.on_doc_safe_close()) BEFORE
     * on_doc_safe_open() has even started - a hook whose own on_doc_open() does work (e.g. an
     * ach_hook-derived class writing a database row) then sees on_doc_close() for a document it
     * never opened. pipeline_worker::do_validate() checks this before validating (see its own doc
     * comment) and, if not yet opened, re-queues the document instead of proceeding - this flag is
     * that check's own signal. mark_opened()/is_opened() are the only writer/reader; no separate
     * mutex needed (bool is naturally atomic-safe to publish this way, and the two are never used
     * for anything requiring a stronger ordering than "eventually visible").
     */
    void               mark_opened() const noexcept { open_reported_.store(true, std::memory_order_release); }
    [[nodiscard]] bool is_opened() const noexcept { return open_reported_.load(std::memory_order_acquire); }
  private: //< methods
    void open(cstr_t path);
    // Records err_ the first time EITHER set_syntax_result()/set_validation_result() reports a
    // failure -- a later, redundant failure report (e.g. C fails AND V independently also fails
    // the same document, see the design discussion's point 4 on why V can race ahead of C) is
    // dropped, since the document is already known invalid either way and err_ already holds a
    // reason. err_mutex_ guards this first-writer-wins race (err_ itself is plain, non-atomic
    // data -- doc_status_t's own mtx_ only protects doc_status_t's five members, not this
    // sibling field, so it needs its own tiny guard).
    void note_error_once(error_info err) noexcept;
  private:
    mmap_file                   doc_;             // core document functionality
    doc_status_t                status_;          // syntax/validation/semantic verdict + completion logic, see its own class doc comment
    std::mutex                  err_mutex_;       // guards err_/err_set_, see note_error_once()
    bool                        err_set_ = false; // true once note_error_once() has recorded the first failure reason
    error_info                  err_;             // if there is an error, here it is the error description (first reporter wins)
    std::uint64_t               out_doc_id_ = 0;  // caller-assigned output document id, see out_doc_id() above
    std::optional<std::int16_t> agent_id_;        // caller-assigned agent id, see agent_id() above -- nullopt until/unless set
    // mutable: mark_opened() is called through pipeline::ds_dscr()'s own const-only accessor (see
    // pipeline.hpp) - same "logical, not representational, state" reasoning doc_status_t's own
    // mtx_ already carries elsewhere in this file.
    mutable std::atomic<bool> open_reported_{false}; // see mark_opened()/is_opened() above
  };
  ///////////////////////////////////////////////////////////////////////////////////////////
  inline doc_dscr::doc_dscr(cstr_t path)
  : doc_(path)
  {
  }
  // Allow construction from existing mmap_file
  inline doc_dscr::doc_dscr(mmap_file&& file)
  : doc_(std::move(file))
  {
  }
  inline doc_dscr::~doc_dscr()
  {
    if (doc_.is_open()) doc_.close();
  }
  inline doc_dscr::doc_dscr(doc_dscr&& o) noexcept
  : doc_(std::move(o.doc_))
  , status_(std::move(o.status_))
  , err_set_(o.err_set_)
  , err_(std::move(o.err_))
  , out_doc_id_(o.out_doc_id_)
  , agent_id_(o.agent_id_)
  , open_reported_(o.open_reported_.load(std::memory_order_relaxed))
  {
  }
  inline doc_dscr& doc_dscr::operator=(doc_dscr&& o) noexcept
  {
    if (this != &o)
    {
      doc_        = std::move(o.doc_);
      status_     = std::move(o.status_);
      err_set_    = o.err_set_;
      err_        = std::move(o.err_);
      out_doc_id_ = o.out_doc_id_;
      agent_id_   = o.agent_id_;
      open_reported_.store(o.open_reported_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    return *this;
  }
  // Opening/closing
  inline void doc_dscr::open(cstr_t path) { doc_.open(path); }
  inline void doc_dscr::close() noexcept { doc_.close(); }
  // Accessors
  inline bool             doc_dscr::is_open() const noexcept { return doc_.is_open(); }
  inline bool             doc_dscr::empty() const noexcept { return doc_.empty(); }
  inline size_t           doc_dscr::size() const noexcept { return doc_.size(); }
  inline const std::byte* doc_dscr::data() const noexcept { return doc_.data(); }
  inline cstr_t           doc_dscr::path() const { return doc_.path(); }
  // String view access (same as mmap_file::string_view())
  inline cstr_t doc_dscr::string_view() const { return doc_.string_view(); }
  // Element access
  inline std::byte doc_dscr::operator[](size_t pos) const { return doc_[pos]; }
  inline std::byte doc_dscr::at(size_t pos) const { return doc_.at(pos); }
  // Iterator support
  inline auto doc_dscr::begin() const noexcept { return doc_.begin(); }
  inline auto doc_dscr::end() const noexcept { return doc_.end(); }
  inline auto doc_dscr::cbegin() const noexcept { return doc_.cbegin(); }
  inline auto doc_dscr::cend() const noexcept { return doc_.cend(); }
  // Span access
  inline std::span<const std::byte> doc_dscr::span() const noexcept { return doc_.span(); }
  inline std::span<const std::byte> doc_dscr::subspan(size_t offset, size_t count) const { return doc_.subspan(offset, count); }
  // Prefetch support
  inline void doc_dscr::prefetch(size_t offset, size_t count) const noexcept { doc_.prefetch(offset, count); }
  // Access to underlying mmap_file (for advanced use)
  inline const mmap_file& doc_dscr::mmf() const noexcept { return doc_; }
  inline mmap_file&       doc_dscr::mmf() noexcept { return doc_; }
  inline doc_dscr::       operator bool() const noexcept { return doc_.is_open(); }

  /**
   * @brief Records err_ the first time either set_syntax_result()/set_validation_result() reports
   * a failure -- see this method's own doc comment in the class body above. A concurrent, later
   * call is a safe, silent no-op (the earlier failure reason is kept; the document is already
   * known invalid either way via doc_status_t, which is what failed()/status() actually consult).
   */
  inline void doc_dscr::note_error_once(error_info err) noexcept
  {
    const std::scoped_lock lock(err_mutex_);
    if (err_set_) return;
    err_set_ = true;
    err_     = std::move(err);
  }

  inline bool doc_dscr::set_syntax_result(bool ok, bool folded_validation, error_info err) noexcept
  {
    if (! ok)
    {
      // An ill-formed document can't be schema-valid either -- both facts drag invalid together
      // (point 14 of the design discussion). Two underlying set_*() calls are made; at most one
      // of them can ever win try_start_closing()'s one-shot race, so both results are combined
      // with || rather than assuming which one it will be.
      note_error_once(std::move(err));
      const bool won_via_syntax = status_.set_syntax(false);
      const bool won_via_valid  = status_.set_valid(false);
      return won_via_syntax || won_via_valid;
    }
    const bool won = status_.set_syntax(true);
    if (folded_validation)
    {
      // C is the sole authority for both in folded mode -- success sets validation valid too, no
      // separate V pass will ever run for this document (see pipeline::plan_run()).
      return status_.set_valid(true) || won;
    }
    return won;
  }

  inline bool doc_dscr::set_validation_result(bool ok, error_info err) noexcept
  {
    if (! ok)
    {
      // A document V rejects can't be called well-formed either -- same both-facts-invalid rule
      // as set_syntax_result()'s failure path above.
      note_error_once(std::move(err));
      const bool won_via_valid  = status_.set_valid(false);
      const bool won_via_syntax = status_.set_syntax(false);
      return won_via_valid || won_via_syntax;
    }
    return status_.set_valid(true);
  }

  inline const error_info& doc_dscr::error() const noexcept { return err_; }
}; // namespace fsp
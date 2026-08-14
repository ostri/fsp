// doc_dscr.hpp
#pragma once
#include "error_info.hpp"
#include "mmap_file.hpp"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string_view>

namespace fsp
{
  /**
   * @brief Three-way verdict for one of doc_status_t's three tracked facts (syntax/validation/
   * semantic) -- distinct from a plain bool so "nobody has reported on this yet" (unknown) can
   * never be confused with "reported, and it's bad" (invalid). unknown is the ONLY valid initial
   * state: every doc_status_t field starts here and is written at most once, by set_syntax()/
   * set_valid()/set_semantic() respectively (see their own doc comments) -- there is no path back
   * from valid/invalid to unknown.
   */
  enum class three_state : std::uint8_t
  {
    unknown, //< nobody has reported a verdict for this fact yet (not the same as "known bad")
    valid,   //< reported, and the verdict was positive
    invalid  //< reported, and the verdict was negative
  };

  /**
   * @brief Self-contained, mutex-protected final syntax/validation/semantic verdict for one
   * document -- owns BOTH the three individual three_state facts (syntax_/valid_/semantic_) AND
   * the "have all three been reported yet, and who gets to act on that" completion logic
   * (done_/closing_/is_finished()/try_start_closing()), replacing the old doc_status_t (a bare
   * 3-bool struct) plus doc_counters' own second-stage CAS latch that used to gate on_doc_close().
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
      done_     = o.done_;
      closing_  = o.closing_;
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
     * @brief True once every one of the three facts this object tracks is either known (all
     * three reported, whatever their individual verdicts) or the outcome is already decided
     * beyond doubt (ANY one of them came back invalid -- see set_field()'s own doc comment on why
     * a single invalid report short-circuits waiting on the other two). Monotonic: once true,
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
     * @brief Aggregate verdict across all three tracked facts: valid iff syntax_/valid_/
     * semantic_ are ALL three::valid; invalid iff AT LEAST ONE of them is three_state::invalid;
     * three_state::unknown otherwise (i.e. at least one fact still unreported and none of the
     * reported ones failed yet). In practice, callers only ever observe this from inside
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
     * WHICH of the three facts specifically failed, not just that the aggregate status() is
     * three_state::invalid (e.g. to log/route differently for a syntax problem vs. a doc-level
     * semantic one). Each simply returns that one field's current three_state under the lock; no
     * aggregation applied.
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
  private:
    // Plain, lock-free aggregate holding a snapshot of the five fields below -- exists solely so
    // the move ctor above can lock o's mutex, read its fields into one of these (see snapshot()),
    // and THEN move-construct *this from it in the member-initializer list, satisfying
    // cppcoreguidelines-prefer-member-initializer without ever reading o's fields unlocked.
    struct fields
    {
      three_state syntax_;
      three_state valid_;
      three_state semantic_;
      int         done_;
      bool        closing_;
    };
    // Builds a fields snapshot of o under o's own lock -- see the move ctor's own doc comment.
    [[nodiscard]] static fields snapshot(doc_status_t& o) noexcept
    {
      const std::scoped_lock lock(o.mtx_);
      return {.syntax_ = o.syntax_, .valid_ = o.valid_, .semantic_ = o.semantic_, .done_ = o.done_, .closing_ = o.closing_};
    }
    // Private, snapshot-based delegate target for the move ctor -- mtx_ itself is deliberately
    // NOT part of fields (std::mutex isn't copyable/movable), so it default-constructs here as a
    // fresh, unlocked mutex for the newly-constructed object, exactly as doc_status_t's own
    // default member initializer would.
    explicit doc_status_t(const fields& f) noexcept
    : syntax_(f.syntax_)
    , valid_(f.valid_)
    , semantic_(f.semantic_)
    , done_(f.done_)
    , closing_(f.closing_)
    {
    }

    // done_ reaches this value exactly when the object is finished: either all three facts were
    // individually reported (each set_*() call increments done_ by 1 on a valid verdict, so three
    // valid reports sum to 3), or a single invalid report jumps done_ straight to 3 (see
    // set_field()'s own doc comment) -- both cases compare equal against this same threshold.
    static constexpr int k_done_threshold = 3;

    /**
     * @brief Shared body for set_syntax()/set_valid()/set_semantic() -- updates field (one of
     * syntax_/valid_/semantic_) from three_state::unknown to three_state::valid or
     * three_state::invalid depending on ok, adjusts done_, and (still under the same lock)
     * attempts try_start_closing()'s decision. On ok=true: field becomes valid, done_ +=1 (one
     * more of the three facts is now known-good). On ok=false: field becomes invalid, done_ is
     * set to k_done_threshold (3) OUTRIGHT rather than incremented -- this is the "short-circuit"
     * that lets is_finished()/try_start_closing() fire as soon as ANY single fact is known bad,
     * without waiting for the other two to be reported at all (mirrors the design discussion's
     * round-1 point 7 / "discard already-doomed documents ASAP": once one fact is invalid, the
     * document's overall verdict is already decided -- status() will report invalid regardless of
     * what the still-unreported fact(s) would have said, so there is nothing to gain by waiting
     * for them and every reason not to hold up on_doc_close() until a fact that can no longer
     * change the outcome eventually reports in).
     * @return whatever try_start_closing()'s own critical section decided for this call -- see
     * its doc comment.
     */
    [[nodiscard]] bool set_field(three_state& field, bool ok) noexcept
    {
      {
        const std::scoped_lock lock(mtx_);
        field = ok ? three_state::valid : three_state::invalid;
        done_ = ok ? done_ + 1 : k_done_threshold;
        if (done_ >= k_done_threshold && ! closing_)
        {
          closing_ = true;
          return true;
        }
        return false;
      }
    }

    // --- the three tracked facts -- each written exactly once, by set_syntax()/set_valid()/
    // set_semantic() respectively (see their own doc comments for which fsp-core role writes
    // which: C for syntax_, V (or C in folded mode) for valid_, the worker orchestrating
    // on_doc_sem_check() for semantic_). All three start unknown and only ever move to valid or
    // invalid, never back. ---
    three_state syntax_{three_state::unknown};   //< set by set_syntax(), reported by C
    three_state valid_{three_state::unknown};    //< set by set_valid(), reported by V (or C, folded mode)
    three_state semantic_{three_state::unknown}; //< set by set_semantic(), reported by the on_doc_sem_check() orchestrator
    // Count of "this fact's outcome can no longer change the final verdict" -- reaches
    // k_done_threshold (3) either via three individual valid reports (one increment each) or a
    // single invalid report (jumps straight to 3, see set_field()). Guarded by mtx_ together with
    // closing_ so is_finished()/try_start_closing() never observe one without the other.
    int done_ = 0;
    // One-shot latch: true once try_start_closing() has handed out its single "true" answer.
    // Guarded by mtx_ -- see try_start_closing()'s own doc comment for why this must be decided
    // in the SAME critical section as the done_ >= k_done_threshold check, not as a separate,
    // later CAS the way the old two-stage design did it.
    bool closing_ = false;
    mutable std::mutex
      mtx_; //< guards every one of the five members above; see class's own doc comment on why one mutex, not per-field atomics
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
    [[nodiscard]] bool              set_semantic_result(bool ok) noexcept { return status_.set_semantic(ok); }
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
    mmap_file     doc_;             // core document functionality
    doc_status_t  status_;          // syntax/validation/semantic verdict + completion logic, see its own class doc comment
    std::mutex    err_mutex_;       // guards err_/err_set_, see note_error_once()
    bool          err_set_ = false; // true once note_error_once() has recorded the first failure reason
    error_info    err_;             // if there is an error, here it is the error description (first reporter wins)
    std::uint64_t out_doc_id_ = 0;  // caller-assigned output document id, see out_doc_id() above
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
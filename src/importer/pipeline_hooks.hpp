// pipeline_hooks.hpp
#pragma once
#include "doc_dscr.hpp"
#include "doc_set_counter.hpp"
#include "doc_set_dscr.hpp"
#include "error_info.hpp"
#include <logger/logger.hpp>
#include "result_values.hpp"
#include "run_doc_data.hpp"
#include "segment_pool.hpp"
#include "segment_result.hpp"
#include "xml_segment.hpp"
#include <cassert>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <memory>
#include <span>

namespace fsp
{
  class pipeline; // forward declaration is enough -- only used via pointer here, see pipeline_'s own doc comment below

  /**
   * @brief Lifecycle/observability hooks a caller of process_files() can plug into, without
   * touching pipeline/pipeline_worker/xml_worker themselves.
   *
   * One clone (see clone()) is made per worker thread -- each clone is exclusively owned and
   * called by that one thread for the thread's whole lifetime, so on_doc_open/close and
   * on_seg_sem_check never need any locking around a developer's own state. on_run_safe_start()/
   * on_run_safe_end() are the only two hooks called from the main thread, on the original instance
   * the caller passed in; on_run_safe_end() receives every worker clone so the developer can
   * aggregate their own per-thread state however they like (sum, max, top-N, ...) in one place.
   *
   * All hook bodies default to a no-op (or, for on_seg_sem_check/on_doc_sem_check/on_doc_close, a
   * sensible default verdict) so a derived class only needs to override what it actually cares
   * about.
   *
   * Every hook that concerns a specific document, plus on_run_start()/on_run_end(), can reach the
   * cb's own run-level/doc-level shared data (see run_doc_data.hpp) via the protected run_data()/
   * doc_data(doc_ndx) accessors declared on pipeline_hooks_crtp below -- not as a parameter, see
   * pipeline_hooks_crtp's own doc comment.
   *
   * EXPERIMENTAL (see pipeline_hooks.hpp's own git history/PR discussion before relying on this
   * shape long-term): every hook pipeline/pipeline_worker/xml_worker actually call is now the
   * "_safe" one, declared public below (on_run_safe_start(), on_wrk_safe_start(), ...), and is
   * final: it does the administrative work (stamping run_start_/worker_start_/log_, ...)
   * unconditionally, then dispatches to the protected override point a caller of process_files()
   * is meant to override instead -- the plain, non-"_safe" name (on_run_start(), on_wrk_start(),
   * ...), which takes no logger::Logger parameter at all (call the protected log() accessor
   * instead). Because the "_safe" wrapper is final, a derived class can no longer override it
   * directly, so it can no longer forget to chain to the base implementation the way the OLD
   * on_run_start()/on_wrk_start() required (see this class's git history) to get
   * run_start_/worker_start_/log_ populated -- log() is guaranteed valid inside every override
   * without the caller having to do anything for it.
   */
  class pipeline_hooks // NOLINT(hicpp-special-member-functions)
  {
  public:
    // Explicit noexcept default ctor: run_start_/worker_start_ (added below) make the
    // implicitly-generated ctor's noexcept-ness non-obvious to static analysis, which otherwise
    // flags default_pipeline_hooks' static-storage-duration initialization as a potential
    // uncaught-throw hazard (bugprone-throwing-static-initialization / cert-err58-cpp).
    pipeline_hooks() noexcept = default;
    virtual ~pipeline_hooks() = default;

    /**
     * @brief Makes a fresh, independent instance of the caller's concrete hook type. See
     * pipeline_hooks_crtp for a way to get this for free.
     */
    [[nodiscard]] virtual std::unique_ptr<pipeline_hooks> clone() const = 0;

    /**
     * @brief Main thread, once, before any document is cut/processed. Final -- stamps run_start_/
     * log_ and binds pipeline_ (see its own doc comment) unconditionally, then dispatches to
     * on_run_start() (see the class's own doc comment on why a derived class overrides that one
     * instead of this one).
     */
    virtual void on_run_safe_start(pipeline& pl, const doc_set_dscr& ds_dscr, const logger::Logger& log) final
    {
      pipeline_  = &pl;
      run_start_ = std::chrono::steady_clock::now();
      log_       = &log;
      on_run_start(ds_dscr);
    }
    /**
     * @brief Main thread, once, after every worker thread has finished. worker_clones holds one
     * entry per worker thread (see clone()); the original instance's own on_run_safe_end() is the
     * only place all of them are visible at once. Final -- see on_run_safe_start()'s own doc
     * comment.
     */
    virtual void on_run_safe_end(const doc_set_counter&           counters,
                                 const doc_set_dscr&              ds_dscr,
                                 std::span<const pipeline_hooks*> worker_clones) final
    { on_run_end(counters, ds_dscr, worker_clones); }

    /**
     * @brief Main thread, once per document, right after its doc_dscr is constructed but before
     * it is added to doc_set_dscr -- the returned id is stored as that document's
     * doc_dscr::out_doc_id() (see pipeline::add_documents()), for a block-writer hook
     * (on_block_store()/on_failed_block_store()) to attach to whatever it writes downstream. Called
     * strictly before any worker thread starts, so the default body below (a plain, non-atomic
     * counter) needs no locking.
     * @param node_hint doc_ndx modulo some caller-meaningful block size (e.g. a Snowflake
     * implementation's node-id range) -- deterministic per document, not tied to which thread
     * later processes it (pipeline_hooks intentionally never exposes worker/thread identity to a
     * hook, see the class's own doc comment). The default body ignores it.
     * @return an opaque, caller-chosen 64-bit id; the default is a simple 1, 2, 3, ... counter.
     */
    [[nodiscard]] virtual std::uint64_t get_doc_id([[maybe_unused]] std::size_t node_hint) { return next_doc_id_++; }

    /**
     * @brief Makes this hooks instance's own concrete run-level shared-data instance (see
     * run_doc_data.hpp's run_data_root and pipeline_hooks_crtp's RunData template parameter) --
     * called once, on the ORIGINAL hooks instance only, by pipeline::process_files() immediately
     * before on_run_safe_start(). Default body returns a plain run_data_root (timing only) -- a
     * derived class never overrides this directly, pipeline_hooks_crtp<Derived, RunData, DocData>
     * does it automatically from the RunData template argument.
     */
    [[nodiscard]] virtual std::unique_ptr<run_data_root> make_run_data_struct() const { return std::make_unique<run_data_root>(); }

    /**
     * @brief Makes one fresh (or, after the pool has grown once, recycled -- see doc_data_root::
     * reset()) instance of this hooks instance's own concrete doc-level shared-data type (see
     * run_doc_data.hpp's doc_data_root and pipeline_hooks_crtp's DocData template parameter) --
     * called by pipeline's doc-data pool, on the ORIGINAL hooks instance only, only when the pool
     * needs to grow (see pipeline_worker::do_cut()). Default body returns a plain doc_data_root
     * (timing only) -- a derived class never overrides this directly, pipeline_hooks_crtp<Derived,
     * RunData, DocData> does it automatically from the DocData template argument.
     */
    [[nodiscard]] virtual std::unique_ptr<doc_data_root> make_doc_data_struct() const { return std::make_unique<doc_data_root>(); }

    /**
     * @brief The pipeline_worker thread itself, once at start and once at end of its lifetime.
     * Final -- stamps worker_start_/log_ and binds pipeline_ (see its own doc comment)
     * unconditionally, then dispatches to on_wrk_start() (see on_run_safe_start()'s own doc
     * comment).
     */
    virtual void on_wrk_safe_start(pipeline& pl, int worker_id, cstr_t thread_name, const logger::Logger& log) final
    {
      pipeline_     = &pl;
      worker_start_ = std::chrono::steady_clock::now();
      log_          = &log;
      on_wrk_start(worker_id, thread_name);
    }
    virtual void on_wrk_safe_end(int worker_id, cstr_t thread_name) final { on_wrk_end(worker_id, thread_name); }

    /** @brief The cutter thread for this specific document (cutting just started / cutting just finished, NOT the whole document's final
     * verdict -- see on_doc_safe_close() below for that). Final -- see on_run_safe_start()'s own doc comment. */
    virtual void on_doc_safe_open(std::size_t doc_ndx, const doc_dscr& dscr) final { on_doc_open(doc_ndx, dscr); }
    /**
     * @brief Fires right after doc_cutter::cut() returns for this document -- BEFORE P has
     * necessarily processed any of its segments and before a separate V pass has necessarily run.
     * Renamed from the old on_doc_close() (which this exact firing point used to be called,
     * misleadingly): see on_doc_safe_sem_check()/on_doc_safe_close() below for the hooks that fire
     * once more is actually known. Final -- see on_run_safe_start()'s own doc comment.
     */
    virtual void on_doc_safe_cutting_finished(std::size_t doc_ndx, const doc_dscr& dscr) final { on_doc_cutting_finished(doc_ndx, dscr); }

    /**
     * @brief Fires once every segment of doc_ndx has been processed (fsp-core's existing
     * cut_finished && total>=expected_total condition, see doc_counters::maybe_seg_processing_complete()) --
     * deliberately independent of whether syntax/validation are known yet (see doc_status_t's own
     * class doc comment in doc_dscr.hpp). A cb's override reaches its own doc-level shared data
     * via the protected doc_data(doc_ndx) accessor (see pipeline_hooks_crtp), not a parameter --
     * whatever per-segment hooks (on_seg_sem_safe_check()) already accumulated into it. Returns
     * the document's own doc-level semantic verdict (true = ok); the fsp-core worker that calls
     * this immediately feeds the returned bool into doc_status_t::set_semantic() (see
     * doc_dscr.hpp), which may also make that SAME call the one responsible for on_doc_safe_close()
     * below (see doc_status_t::try_start_closing()). Called from whichever thread's
     * record_doc_close()/end_segment() call happens to be the one that satisfies the completion
     * condition -- exactly once per document. Final -- see on_run_safe_start()'s own doc comment.
     */
    virtual bool on_doc_safe_sem_check(std::size_t doc_ndx) final { return on_doc_sem_check(doc_ndx); }

    /**
     * @brief Fires once syntax+validation+doc-level-semantics are ALL known for doc_ndx (the
     * renamed old on_doc_close(), now firing at the point its name always implied instead of
     * right after cutting -- see on_doc_safe_cutting_finished() above for that). verdict is the
     * document's own live doc_status_t (see doc_dscr.hpp's own class doc comment) -- by the time
     * this fires, verdict.is_finished() is guaranteed true, so verdict.status()/verdict.ok() give
     * the final aggregate, and verdict.syntax_status()/valid_status()/semantic_status() give the
     * individual partial verdicts if a cb needs to know WHICH fact specifically failed. err is
     * fsp-core's own error_info for a syntax/validation failure (its default-constructed
     * processor_error::success state means "no error" -- see error_info.hpp). Returns the FINAL
     * verdict for this document (default: verdict.ok()) -- e.g. an importer deciding whether to
     * move a document to a done-path or an err-path. Final -- see on_run_safe_start()'s own doc
     * comment. Whichever of C/V/the on_doc_safe_sem_check() orchestrator won
     * doc_status_t::try_start_closing() is the one, and only one, thread that ever calls this for
     * a given document. NOTE: doc_data(doc_ndx) is still valid inside this call and inside
     * on_doc_safe_finish() below -- the doc-level slot is only recycled AFTER on_doc_finish()
     * returns, see pipeline::finish_doc_close().
     */
    virtual bool on_doc_safe_close(std::size_t doc_ndx, const doc_status_t& verdict, const error_info& err, const doc_dscr& dscr) final
    { return on_doc_close(doc_ndx, verdict, err, dscr); }

    /**
     * @brief Fires immediately after on_doc_safe_close() returns -- same call site, same "exactly
     * once, from whichever thread won doc_status_t::try_start_closing()" guarantee. Final --
     * unconditionally stops this document's doc_data(doc_ndx).timing() (so duration() is already
     * frozen and readable inside on_doc_finish() below) BEFORE dispatching to it; AFTER
     * on_doc_finish() returns, pipeline::finish_doc_close() hands the doc-level slot back to the
     * recycling pool (see pipeline_hooks_crtp's own doc comment on doc_data()) -- this is the one
     * and only place a cb is guaranteed to see this document's final, frozen doc-level state
     * before it may be reset() and reused by a different document.
     */
    virtual void on_doc_safe_finish(std::size_t doc_ndx) final
    {
      doc_data_timing_stop(doc_ndx);
      on_doc_finish(doc_ndx);
    }

    /**
     * @brief A P-role thread just extracted values from one segment. is_first/is_last mark the
     * first/last segment of doc_ndx (mutually exclusive with a single-segment document, where
     * both are true at once). result bundles seg_id()/seg_type()/doc_ndx()/values() -- pass
     * result.seg_type() and result (by reference) to fsp::materialize_variant<Namespace>() to get
     * the segment back as the developer's own schema type instead of the generic, name-indexed
     * result_values -- result is non-const because a validated_t<X> field's failure gets appended
     * to result.errors() during materialization (see reflection.hpp). segment is the raw cut this
     * result came from (offset/length/ns/attrs of the top-level tag), for callers that want more
     * than the extracted values. Returns the segment's semantic verdict (true = semantically
     * correct). Final -- see on_run_safe_start()'s own doc comment.
     * @note Called potentially millions of times per run -- always a genuine virtual call (see
     * pipeline_hooks_crtp's doc comment for why the earlier "detect override, skip the call"
     * idea was dropped).
     */
    virtual bool on_seg_sem_safe_check(const xml_segment& segment, segment_result& result, bool is_first, bool is_last) final
    { return on_seg_sem_check(segment, result, is_first, is_last); }

    /**
     * @brief A P-role thread hands off a batch of semantically OK segments (on_seg_sem_check()
     * returned true) for external storage (file/db/message queue). Called from the one worker
     * thread that accumulated indices -- either once importer_config::ok_block_flush_size worth
     * of segments have piled up, or once, with whatever remains, when that thread's loop ends
     * (see xml_worker/pipeline_worker). indices are still-live slots in pool -- neither
     * pool.segment_at(idx) nor pool.result_at(idx) is reused by anyone else until THIS call
     * returns and the caller (not on_block_store() itself) releases them via
     * segment_pool::release_slots(). ds_dscr resolves each segment's mmap_base
     * (ds_dscr[seg.doc_ndx()].mmf().data(), for xml_segment::view()) and out_doc_id() -- a batch
     * can freely mix segments from different documents, so ds_dscr is looked up per index, not
     * once for the whole batch. Final -- see on_run_safe_start()'s own doc comment.
     * @param indices pool slot indices belonging to this batch; empty is possible only for the
     * final end-of-loop flush and is a valid, harmless no-op call.
     */
    virtual void on_block_safe_store( //
      std::span<const std::size_t> indices,
      segment_pool&                pool,
      const doc_set_dscr&          ds_dscr) final
    { on_block_store(indices, pool, ds_dscr); }

    /**
     * @brief Same as on_block_safe_store(), for the batch of semantically FAILED segments
     * (on_seg_sem_check() returned false) -- see importer_config::nak_block_flush_size. errors holds
     * one entry per index, same order, same length as indices (errors[i] describes why
     * indices[i] failed). Final -- see on_run_safe_start()'s own doc comment.
     */
    virtual void on_failed_block_safe_store(std::span<const std::size_t> indices,
                                            std::span<const error_info>  errors,
                                            segment_pool&                pool,
                                            const doc_set_dscr&          ds_dscr) final
    { on_failed_block_store(indices, errors, pool, ds_dscr); }
  protected:
    /**
     * @brief This run's logger -- valid inside every override point below (on_run_start()/
     * on_wrk_start() -- the two hooks guaranteed to run first on the original instance and on
     * each clone respectively, see the class's own doc comment -- set it unconditionally before
     * dispatching to any other override point). Asserts rather than dereferencing null if somehow
     * called before either of those, e.g. from a derived class's own constructor.
     */
    [[nodiscard]] const logger::Logger& log() const
    {
      assert(log_ != nullptr &&
             "pipeline_hooks::log() called before on_run_safe_start()/on_wrk_safe_start() -- see their own doc comments");
      return *log_;
    }

    /// @brief Override point for on_run_safe_start() -- see the class's own doc comment. log() is valid inside this call.
    virtual void on_run_start([[maybe_unused]] const doc_set_dscr& ds_dscr) { }
    /// @brief Override point for on_run_safe_end() -- see the class's own doc comment. log() is valid inside this call.
    virtual void on_run_end([[maybe_unused]] const doc_set_counter&           counters,
                            [[maybe_unused]] const doc_set_dscr&              ds_dscr,
                            [[maybe_unused]] std::span<const pipeline_hooks*> worker_clones)
    {
    }
    /// @brief Override point for on_wrk_safe_start() -- see the class's own doc comment. log() is valid inside this call.
    virtual void on_wrk_start([[maybe_unused]] int worker_id, [[maybe_unused]] cstr_t thread_name) { }
    /// @brief Override point for on_wrk_safe_end() -- see the class's own doc comment. log() is valid inside this call.
    virtual void on_wrk_end([[maybe_unused]] int worker_id, [[maybe_unused]] cstr_t thread_name) { }
    /// @brief Override point for on_doc_safe_open() -- see the class's own doc comment. log() is valid inside this call.
    virtual void on_doc_open([[maybe_unused]] std::size_t doc_ndx, [[maybe_unused]] const doc_dscr& dscr) { }
    /// @brief Override point for on_doc_safe_cutting_finished() -- see the class's own doc comment. log() is valid inside this call.
    virtual void on_doc_cutting_finished([[maybe_unused]] std::size_t doc_ndx, [[maybe_unused]] const doc_dscr& dscr) { }
    /**
     * @brief Override point for on_doc_safe_sem_check() -- see the class's own doc comment. log()
     * and doc_data(doc_ndx) (see pipeline_hooks_crtp) are valid inside this call. Default: no
     * doc-level semantic opinion, always ok.
     */
    virtual bool on_doc_sem_check([[maybe_unused]] std::size_t doc_ndx) { return true; }
    /**
     * @brief Override point for on_doc_safe_close() -- see the class's own doc comment. log() and
     * doc_data(doc_ndx) are valid inside this call. Default: the final verdict is exactly
     * verdict.ok() (syntax AND validation AND semantic all passed) -- a derived class overrides
     * this to act on the decision (e.g. move a document to a done-path/err-path) while keeping
     * (or refining) that same default logic.
     */
    virtual bool on_doc_close([[maybe_unused]] std::size_t       doc_ndx,
                              const doc_status_t&                verdict,
                              [[maybe_unused]] const error_info& err,
                              [[maybe_unused]] const doc_dscr&   dscr)
    { return verdict.ok(); }
    /**
     * @brief Override point for on_doc_safe_finish() -- see the class's own doc comment. log() and
     * doc_data(doc_ndx) are valid inside this call (doc_data(doc_ndx).timing() is already
     * stopped). Default: no-op. This is the LAST chance to read this document's doc-level state
     * before the slot is recycled for a different document.
     */
    virtual void on_doc_finish([[maybe_unused]] std::size_t doc_ndx) { }
    /**
     * @brief Override point for on_seg_sem_safe_check() -- see the class's own doc comment. log() is
     * valid inside this call. Default mirrors technical completeness (result.values().complete()),
     * i.e. semantic == technical until a derived class adds real business logic.
     */
    virtual bool on_seg_sem_check([[maybe_unused]] const xml_segment& segment,
                                  segment_result&                     result,
                                  [[maybe_unused]] bool               is_first,
                                  [[maybe_unused]] bool               is_last)
    { return result.values().complete(); }
    /// @brief Override point for on_block_safe_store() -- see the class's own doc comment. log() is valid inside this call.
    virtual void on_block_store([[maybe_unused]] std::span<const std::size_t> indices,
                                [[maybe_unused]] segment_pool&                pool,
                                [[maybe_unused]] const doc_set_dscr&          ds_dscr)
    {
    }
    /// @brief Override point for on_failed_block_safe_store() -- see the class's own doc comment. log() is valid inside this call.
    virtual void on_failed_block_store([[maybe_unused]] std::span<const std::size_t> indices,
                                       [[maybe_unused]] std::span<const error_info>  errors,
                                       [[maybe_unused]] segment_pool&                pool,
                                       [[maybe_unused]] const doc_set_dscr&          ds_dscr)
    {
    }

    /**
     * @brief Seconds elapsed since on_run_safe_start() stamped run_start_. Only meaningful on the
     * ORIGINAL instance (on_run_safe_start/on_run_safe_end are the only two hooks called on it,
     * never on a clone) -- mirrors run_start_'s own precondition.
     */
    [[nodiscard]] double elapsed_run_sec() const
    { return std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - run_start_).count(); }
    /**
     * @brief Seconds elapsed since on_wrk_safe_start() stamped worker_start_. Per-clone, set and
     * read by the one thread that owns that clone -- mirrors worker_start_'s own precondition.
     */
    [[nodiscard]] double elapsed_worker_sec() const
    { return std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - worker_start_).count(); }

    /**
     * @brief Non-templated run-level shared-data accessor -- pipeline_hooks_crtp<Derived, RunData,
     * DocData>'s own run_data() wraps this with a static_cast<RunData&>. Defined in
     * pipeline_hooks.cpp (not inline here) so this header doesn't need pipeline.hpp's full
     * definition -- pipeline.hpp already includes THIS header, so the reverse would cycle.
     * Reaches pipeline_'s single, run-lifetime run_data_root instance (see
     * pipeline::process_files()) -- the SAME instance on every worker clone, see pipeline_'s own
     * doc comment.
     */
    [[nodiscard]] run_data_root& run_data_impl() const;
    /**
     * @brief Non-templated doc-level shared-data accessor -- pipeline_hooks_crtp<Derived, RunData,
     * DocData>'s own doc_data(doc_ndx) wraps this with a static_cast<DocData&>. See
     * run_data_impl()'s own doc comment for why this is defined out-of-line. Reaches pipeline_'s
     * recycling doc-data pool slot currently assigned to doc_ndx (see pipeline_worker::do_cut()).
     */
    [[nodiscard]] doc_data_root& doc_data_impl(std::size_t doc_ndx) const;
    /**
     * @brief Stops doc_ndx's doc-level timing_fields -- called unconditionally by
     * on_doc_safe_finish() before dispatching to on_doc_finish(), so duration() is already frozen
     * by the time a cb's override runs. Defined in pipeline_hooks.cpp, same reason as
     * run_data_impl().
     */
    void doc_data_timing_stop(std::size_t doc_ndx);
  private:
    std::chrono::steady_clock::time_point run_start_;
    std::chrono::steady_clock::time_point worker_start_;
    std::uint64_t                         next_doc_id_ = 1; // get_doc_id()'s default counter -- main-thread-only, see its doc comment
    // Not owned -- points at whichever logger::Logger on_run_safe_start()/on_wrk_safe_start() was
    // last called with (the original instance's own, or this clone's own worker thread logger
    // respectively). Both, ultimately, reference the SAME logger::Logger the whole run shares
    // (see importer::log_ptr_) -- only thread_local logger::log_thread_name differs per thread,
    // not the Logger object itself -- so storing a pointer here doesn't change WHICH physical
    // logger a hook's messages land in, only how a derived class reaches it (log() instead of a
    // parameter on every single hook).
    const logger::Logger* log_ = nullptr;
    // Not owned -- bound once by on_run_safe_start() (on the original instance) or
    // on_wrk_safe_start() (on each worker clone, see clone()), both of which happen before any
    // other hook fires (see the class's own doc comment) -- run_data_impl()/doc_data_impl() read
    // through this to reach pipeline_'s own run-level/doc-level shared-data storage. The SAME
    // pipeline instance for every clone of one process_files() call, so run_data_impl() always
    // resolves to the SAME run_data_root object across every clone (see pipeline_hooks_crtp's own
    // doc comment on why this is the point of the whole mechanism).
    pipeline* pipeline_ = nullptr;
  };

  /**
   * @brief Recommended base for a developer's own hook class: derive from
   * pipeline_hooks_crtp<my_hooks> instead of pipeline_hooks directly, and clone() is handled
   * correctly with zero extra code.
   *
   * @note An earlier version of this class also tried to detect, at construction time, whether
   * Derived had overridden on_seg_sem_check (via comparing &Derived::on_seg_sem_check
   * != &pipeline_hooks::on_seg_sem_check), caching the result in a bool so hot call sites
   * could skip the virtual call entirely when it hadn't been overridden. Verified by direct
   * testing that this does NOT work: pointer-to-virtual-member-function values are encoded as a
   * vtable slot index under the Itanium ABI (GCC/Clang on Linux), and overriding a virtual
   * function never changes its vtable slot -- so &Derived::f and &Base::f compare EQUAL
   * regardless of whether Derived actually overrides f. The comparison always reported "not
   * overridden", silently skipping on_seg_sem_check for every hook, including ones that did
   * override it. Reverted to a plain, always-invoked virtual call.
   * @tparam Derived The developer's own concrete hook class (Curiously Recurring Template
   * Pattern) -- must be copy-constructible (clone() copies *this via Derived's own copy ctor).
   * @note The constructor is protected, not private+friend Derived -- protected still blocks
   * pipeline_hooks_crtp<X> from being instantiated as a standalone (non-CRTP) type, but (unlike
   * friend Derived, which only grants access to Derived itself) also allows an intermediate
   * mixin between pipeline_hooks_crtp and Derived, such as typed_semantic_check.hpp's own
   * typed_semantic_check<Derived, Namespace>, whose own (implicitly generated) constructor needs
   * to reach this one.
   * @tparam RunData  a package's own run-level shared-data type (see run_doc_data.hpp) -- must
   * derive from run_data_root; defaults to run_data_root itself (timing only) for a package that
   * needs nothing more. A document type that needs nothing beyond an intermediate package-level
   * cb's own RunData simply doesn't repeat this template argument (see typed_semantic_check.hpp's
   * own doc comment for the worked ach_cb/ct_in_cb example) -- ordinary C++ default template
   * arguments give "prevzame lastnosti predhodnega nivoja" for free, no extra machinery needed.
   * @tparam DocData  same role as RunData, for doc-level shared data -- must derive from
   * doc_data_root; defaults to doc_data_root itself.
   */
  template <typename Derived, typename RunData = run_data_root, typename DocData = doc_data_root>
    requires std::derived_from<RunData, run_data_root> && std::derived_from<DocData, doc_data_root>
  class pipeline_hooks_crtp : public pipeline_hooks
  {
  protected:
    pipeline_hooks_crtp() = default; // NOLINT(bugprone-crtp-constructor-accessibility) -- see the class's own note above

    /**
     * @brief This run's own RunData instance -- the SAME object on every worker clone (see
     * pipeline_hooks::run_data_impl()'s own doc comment). Valid inside on_run_start()/
     * on_run_end() and every doc-scoped override point (see pipeline_hooks.hpp's own
     * availability table in its class doc comment). Unsynchronized by default -- wrap in
     * fsp::lock() (run_doc_data.hpp) when concurrent access from multiple worker clones is
     * actually possible (e.g. aggregating into it from on_seg_sem_check(), called by every
     * worker thread at once).
     */
    [[nodiscard]] RunData& run_data() const noexcept { return static_cast<RunData&>(run_data_impl()); }
    /**
     * @brief doc_ndx's own DocData instance -- the SAME object seen by whichever worker thread
     * happens to touch this document (recycled, not per-worker, see run_doc_data.hpp's own doc
     * comment on doc_data_root::reset()). Valid inside every doc-scoped override point. Same
     * locking contract as run_data() above.
     */
    [[nodiscard]] DocData& doc_data(std::size_t doc_ndx) const noexcept { return static_cast<DocData&>(doc_data_impl(doc_ndx)); }
  public:
    [[nodiscard]] std::unique_ptr<pipeline_hooks> clone() const override
    { return std::make_unique<Derived>(static_cast<const Derived&>(*this)); }
    [[nodiscard]] std::unique_ptr<run_data_root> make_run_data_struct() const override { return std::make_unique<RunData>(); }
    [[nodiscard]] std::unique_ptr<doc_data_root> make_doc_data_struct() const override { return std::make_unique<DocData>(); }
  };

  /**
   * @brief The no-op hook set used when a caller doesn't supply their own -- keeps
   * process_files() usable without hooks at all. Plain run_data_root/doc_data_root (timing only)
   * suffice, so no_op_hooks doesn't need to name either template argument.
   */
  class no_op_hooks : public pipeline_hooks_crtp<no_op_hooks>
  {
  };

  /**
   * @brief Shared default instance for process_files()'s hooks parameter. Never mutated itself
   * (only cloned), so sharing it across concurrent process_files() calls is safe.
   */
  inline no_op_hooks default_pipeline_hooks{}; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
} // namespace fsp

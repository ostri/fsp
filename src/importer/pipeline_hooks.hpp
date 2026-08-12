// pipeline_hooks.hpp
#pragma once
#include "doc_dscr.hpp"
#include "doc_set_counter.hpp"
#include "doc_set_dscr.hpp"
#include <logger/logger.hpp>
#include "result_values.hpp"
#include "segment_result.hpp"
#include "xml_segment.hpp"
#include <chrono>
#include <memory>
#include <span>

namespace fsp
{
  /**
   * @brief Lifecycle/observability hooks a caller of process_files() can plug into, without
   * touching pipeline/pipeline_worker/xml_worker themselves.
   *
   * One clone (see clone()) is made per worker thread -- each clone is exclusively owned and
   * called by that one thread for the thread's whole lifetime, so on_doc_open/close and
   * on_seg_proc never need any locking around a developer's own state. on_run_start()/
   * on_run_end() are the only two hooks called from the main thread, on the original instance the
   * caller passed in; on_run_end() receives every worker clone so the developer can aggregate
   * their own per-thread state however they like (sum, max, top-N, ...) in one place.
   *
   * All hook bodies default to a no-op (or, for on_seg_proc, a sensible default verdict)
   * so a derived class only needs to override what it actually cares about.
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
     * @brief Main thread, once, before any document is cut/processed. Stamps run_start_ so a
     * derived class's own on_run_end() can call elapsed_run_sec() -- a derived override that
     * wants this must call pipeline_hooks::on_run_start() itself, since overriding replaces
     * rather than extends the base body.
     */
    virtual void on_run_start([[maybe_unused]] const doc_set_dscr& ds_dscr, [[maybe_unused]] const logger::Logger& log)
    { run_start_ = std::chrono::steady_clock::now(); }
    /**
     * @brief Main thread, once, after every worker thread has finished. worker_clones holds one
     * entry per worker thread (see clone()); the original instance's own on_run_end() is the only
     * place all of them are visible at once.
     */
    virtual void on_run_end([[maybe_unused]] const doc_set_counter&           counters,
                            [[maybe_unused]] const doc_set_dscr&              ds_dscr,
                            [[maybe_unused]] std::span<const pipeline_hooks*> worker_clones,
                            [[maybe_unused]] const logger::Logger&            log)
    {
    }

    /**
     * @brief The pipeline_worker thread itself, once at start and once at end of its lifetime.
     * Stamps worker_start_ so a derived class's own on_wrk_end() can call
     * elapsed_worker_sec() -- see on_run_start()'s note on calling the base body explicitly.
     */
    virtual void on_wrk_start([[maybe_unused]] int                   worker_id,
                              [[maybe_unused]] cstr_t                thread_name,
                              [[maybe_unused]] const logger::Logger& log)
    { worker_start_ = std::chrono::steady_clock::now(); }
    virtual void on_wrk_end([[maybe_unused]] int worker_id, [[maybe_unused]] cstr_t thread_name, [[maybe_unused]] const logger::Logger& log)
    {
    }

    /** @brief The cutter thread for this specific document (cutting just started/just finished). */
    virtual void on_doc_open([[maybe_unused]] std::size_t           doc_ndx,
                             [[maybe_unused]] const doc_dscr&       dscr,
                             [[maybe_unused]] const logger::Logger& log)
    {
    }
    virtual void on_doc_close([[maybe_unused]] std::size_t           doc_ndx,
                              [[maybe_unused]] doc_status            status,
                              [[maybe_unused]] const doc_dscr&       dscr,
                              [[maybe_unused]] const logger::Logger& log)
    {
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
     * correct) -- the default just mirrors technical completeness (result.values().complete()),
     * i.e. semantic == technical until a derived class adds real business logic.
     * @note Called potentially millions of times per run -- always a genuine virtual call (see
     * pipeline_hooks_crtp's doc comment for why the earlier "detect override, skip the call"
     * idea was dropped).
     */
    virtual bool on_seg_proc([[maybe_unused]] const xml_segment&    segment,
                             segment_result&                        result,
                             [[maybe_unused]] bool                  is_first,
                             [[maybe_unused]] bool                  is_last,
                             [[maybe_unused]] const logger::Logger& log)
    { return result.values().complete(); }
  protected:
    /**
     * @brief Seconds elapsed since on_run_start() stamped run_start_. Only meaningful on the
     * ORIGINAL instance (on_run_start/on_run_end are the only two hooks called on it, never on a
     * clone) -- mirrors run_start_'s own precondition.
     */
    [[nodiscard]] double elapsed_run_sec() const
    { return std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - run_start_).count(); }
    /**
     * @brief Seconds elapsed since on_wrk_start() stamped worker_start_. Per-clone, set and read
     * by the one thread that owns that clone -- mirrors worker_start_'s own precondition.
     */
    [[nodiscard]] double elapsed_worker_sec() const
    { return std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - worker_start_).count(); }
  private:
    std::chrono::steady_clock::time_point run_start_;
    std::chrono::steady_clock::time_point worker_start_;
  };

  /**
   * @brief Recommended base for a developer's own hook class: derive from
   * pipeline_hooks_crtp<my_hooks> instead of pipeline_hooks directly, and clone() is handled
   * correctly with zero extra code.
   *
   * @note An earlier version of this class also tried to detect, at construction time, whether
   * Derived had overridden on_seg_proc (via comparing &Derived::on_seg_proc
   * != &pipeline_hooks::on_seg_proc), caching the result in a bool so hot call sites
   * could skip the virtual call entirely when it hadn't been overridden. Verified by direct
   * testing that this does NOT work: pointer-to-virtual-member-function values are encoded as a
   * vtable slot index under the Itanium ABI (GCC/Clang on Linux), and overriding a virtual
   * function never changes its vtable slot -- so &Derived::f and &Base::f compare EQUAL
   * regardless of whether Derived actually overrides f. The comparison always reported "not
   * overridden", silently skipping on_seg_proc for every hook, including ones that did
   * override it. Reverted to a plain, always-invoked virtual call.
   * @tparam Derived The developer's own concrete hook class (Curiously Recurring Template
   * Pattern) -- must be copy-constructible (clone() copies *this via Derived's own copy ctor).
   */
  template <typename Derived>
  class pipeline_hooks_crtp : public pipeline_hooks
  {
    pipeline_hooks_crtp() = default;
  public:
    [[nodiscard]] std::unique_ptr<pipeline_hooks> clone() const override
    { return std::make_unique<Derived>(static_cast<const Derived&>(*this)); }
    friend Derived;
  };

  /**
   * @brief The no-op hook set used when a caller doesn't supply their own -- keeps
   * process_files() usable without hooks at all.
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

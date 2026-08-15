// run_doc_data.hpp
#pragma once
#include <chrono>
#include <concepts>
#include <memory>
#include <mutex>

namespace fsp
{
  /**
   * @brief Shared timing fields common to run_data_root and doc_data_root -- NOT a base either
   * derives from (pipeline_hooks_crtp's own requires-clause constrains RunData/DocData to derive
   * from run_data_root/doc_data_root specifically, not a shared grandparent), so this stays a
   * plain mixin included by value in both, not a further inheritance layer.
   */
  class timing_fields
  {
  public:
    /**
     * @brief Stamped by the framework right before the corresponding on_run_start()/on_doc_open()
     * fires -- see run_data_root/doc_data_root's own doc comments for the exact call site.
     */
    void start() noexcept { start_ = std::chrono::steady_clock::now(); }
    /**
     * @brief Stamped by the framework right after the corresponding on_run_end()/on_doc_finish()
     * fires, freezing duration() before the struct is torn down (run-level) or recycled
     * (doc-level).
     */
    void                                                end() noexcept { end_ = std::chrono::steady_clock::now(); }
    [[nodiscard]] std::chrono::steady_clock::time_point start_time() const noexcept { return start_; }
    [[nodiscard]] std::chrono::steady_clock::time_point end_time() const noexcept { return end_; }
    /**
     * @brief end() - start(); end_ defaults to start_'s own value until end() is actually called,
     * so duration() reads as zero (not garbage/negative) for a struct that's still open.
     */
    [[nodiscard]] std::chrono::duration<double> duration() const noexcept { return end_ - start_; }
  private:
    std::chrono::steady_clock::time_point start_;
    std::chrono::steady_clock::time_point end_ = start_;
  };

  /**
   * @brief Root of the run-level shared-data hierarchy. A package's own run data derives from
   * this (or from an intermediate package-level type that itself derives from this) and adds
   * whatever fields it needs; if a package needs nothing beyond timing, it uses run_data_root
   * directly. See pipeline_hooks_crtp's requires-clause (pipeline_hooks.hpp) -- every RunData
   * template argument anywhere in the hierarchy must ultimately derive from THIS class
   * specifically. Non-templated, ordinary polymorphic base -- a cb reaches its own concrete
   * fields via static_cast to the exact type it declared as a template argument, never
   * dynamic_cast, since the concrete type is known at compile time through the CRTP parameter.
   */
  class run_data_root
  {
  public:
    run_data_root()                                               = default;
    virtual ~run_data_root()                                      = default;
    run_data_root(const run_data_root&)                           = delete;
    run_data_root& operator=(const run_data_root&)                = delete;
    run_data_root(run_data_root&&)                                = delete;
    run_data_root&                     operator=(run_data_root&&) = delete;
    [[nodiscard]] timing_fields&       timing() noexcept { return timing_; }
    [[nodiscard]] const timing_fields& timing() const noexcept { return timing_; }
    /**
     * @brief Guards cross-thread access to this instance's own derived fields -- see fsp::lock()
     * for the RAII wrapper a cb actually uses instead of calling lock()/unlock() on this
     * directly. Public (not protected): the mutex itself carries no invariant to protect from
     * the cb, so there's no reason to gate it behind an accessor.
     */
    [[nodiscard]] std::mutex& mutex() noexcept { return mutex_; }
  private:
    timing_fields timing_;
    std::mutex    mutex_;
  };

  /**
   * @brief Root of the document-level shared-data hierarchy, RECYCLED across documents (see
   * pipeline_hooks::make_doc_data_struct()) -- a concrete type must therefore be safe to reset()
   * and reused, not just constructed once. Same requires-clause role as run_data_root above.
   */
  class doc_data_root
  {
  public:
    doc_data_root()                                               = default;
    virtual ~doc_data_root()                                      = default;
    doc_data_root(const doc_data_root&)                           = delete;
    doc_data_root& operator=(const doc_data_root&)                = delete;
    doc_data_root(doc_data_root&&)                                = delete;
    doc_data_root&                     operator=(doc_data_root&&) = delete;
    [[nodiscard]] timing_fields&       timing() noexcept { return timing_; }
    [[nodiscard]] const timing_fields& timing() const noexcept { return timing_; }
    /** @brief See run_data_root::mutex() -- same role, for cross-thread access to a document's own derived fields. */
    [[nodiscard]] std::mutex& mutex() noexcept { return mutex_; }
    /**
     * @brief Called by the framework immediately before handing a recycled instance to a new
     * document (see the pool in pipeline.cpp) -- a derived type overrides this to clear ITS OWN
     * added fields; the override must call doc_data_root::reset() itself (chains, like a
     * destructor) so timing_ is always reset too. Not pure virtual: the base has nothing else to
     * clear.
     */
    virtual void reset() { timing_ = {}; }
  private:
    timing_fields timing_;
    std::mutex    mutex_;
  };

  /**
   * @brief RAII guard: locks Root's own mutex on construction, unlocks on destruction (same
   * shape as std::lock_guard, scoped to exactly Root's own mutex instead of an externally-passed
   * one) -- returned by fsp::lock() so "lock, use, forget to unlock" is structurally impossible.
   * @tparam Root the concrete run/doc data type being locked (run_data_root/doc_data_root or
   * anything derived from either).
   */
  template <typename Root>
  class locked_root
  {
  public:
    explicit locked_root(Root& root)
    : root_(root)
    , guard_(root.mutex())
    {
    }
    [[nodiscard]] Root* operator->() const noexcept { return &root_; }
    [[nodiscard]] Root& operator*() const noexcept { return root_; }
  private:
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members) -- same reference-holding
    // shape as std::lock_guard/std::scoped_lock itself; locked_root is only ever a short-lived
    // scope-local (see fsp::lock()'s own doc comment), never stored, so the usual "reference
    // member can dangle if copied/moved/outlived" hazard this check guards against doesn't apply.
    Root&                       root_;
    std::lock_guard<std::mutex> guard_;
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
  };

  /**
   * @brief Locks root's own mutex for the lifetime of the returned guard -- the easy, RAII entry
   * point a cb uses instead of calling root.mutex().lock()/unlock() directly. Typical use:
   * @code
   * auto guard = fsp::lock(run_data());
   * guard->my_field += 1;
   * // ~locked_root() unlocks here, at the end of the enclosing scope
   * @endcode
   * @tparam T deduced from root -- any type derived from run_data_root/doc_data_root.
   */
  template <typename T>
  [[nodiscard]] locked_root<T> lock(T& root)
  { return locked_root<T>(root); }
} // namespace fsp

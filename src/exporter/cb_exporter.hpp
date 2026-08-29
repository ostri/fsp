#pragma once
/**
 * @file cb_exporter.hpp
 * @brief Callback interface a caller of fsp::exporter<T,Q> implements to supply document
 * content and receive lifecycle notifications, as specified in doc/opis_exporterja.txt.
 *
 * The caller
 * constructs one "prototype" cb_exporter instance and hands it to exporter<T,Q>::execute();
 * exporter clones it once per worker thread so each worker owns its own,
 * exclusively-used instance with its own private state -- no locking is ever needed inside a
 * cb_exporter method for its own state, since no two threads ever touch the same clone.
 */

#include "exporter_error.hpp"
#include "exporter_types.hpp"
#include <cstddef>
#include <logger/logger.hpp>
#include <memory>

namespace fsp
{
  /**
   * @brief Stateful, per-worker-thread callback interface for fsp::exporter<T,Q>.
   * @tparam T concrete transaction type, must derive from transaction_t (see transaction_like)
   * @tparam Q concrete run-qualifiers type, must derive from qualificators_t (see qualifiers_like)
   */
  template <transaction_like T, qualifiers_like Q>
  class cb_exporter // NOLINT(hicpp-special-member-functions)
  {
  public:
    explicit cb_exporter(const logger::Logger& log) noexcept;
    virtual ~cb_exporter() = default;
    /**
     * @brief Result of fetch_run_stat(): whole-run/whole-drain statistics for drain_id.
     * Alias for the non-template run_stat_pair_t, which exporter_state (itself non-template)
     * consumes directly -- see exporter_types.hpp.
     */
    using run_stat_t = run_stat_pair_t;
    using blk_t      = txn_block_t<T>;
    using blk_id_t   = unsigned int;
    /**
     * @brief Makes a fresh, independent instance of the caller's concrete cb_exporter type.
     * @details Called exactly once per worker thread, at that thread's startup, by exporter<T,Q>.
     * See cb_exporter_crtp for a way to get this for free.
     */
    [[nodiscard]] virtual std::unique_ptr<cb_exporter> clone() const = 0;
    ///< make filename from the components
    [[nodiscard]] virtual exp_result<str_t> fetch_doc_name(
      //
      const Q& q,               ///< readonly block of the run qualifiers
      cstr_t   path,            ///< path to the output file
      drain_t  drain_id,        ///< receiver of the document
      blk_id_t block_number,    ///< id of the block fetched from the source
      blk_id_t total_blocks,    ///< overall number of the blocks for this drain
      cstr_t   filename_prefix, ///< prefix of the filename
      cstr_t   filename_ext     ///< filename extension
    );
    /**
     * @brief Reports whole-run statistics for one drain, used to resume a crashed run: the
     * "existing_doc_count" returned here is honored as the starting point for that drain's
     * document numbering, so a re-run after a crash continues rather than overwriting.
     *
     * @return exp_result<run_stat_t> acumulated statistic
     */
    [[nodiscard]] virtual exp_result<run_stat_t> fetch_run_stat(
      //
      const Q& q,       ///< readonly block of the run qualifiers
      drain_t  drain_id ///< receiver of the document
      ) = 0;
    /**
     * @brief Fetches one block of transactions for one document. See fetch_doc_data_status for
     * the three possible outcomes (a block, end-of-data for this drain, or an error).
     */
    [[nodiscard]] virtual fetch_doc_data_result_t<T> fetch_doc_data(
      //
      const Q& q,        ///< read only block of run qualifiers
      drain_t  drain_id, ///< receiver of the document
      doc_id_t doc_id    ///< internal id of the document
      ) = 0;
    /**
     * @brief Converts one transaction's input data into its external/document string representation.
     * @return exp_result<str_t>
     */
    [[nodiscard]] virtual exp_result<str_t> prepare_transaction(
      //
      std::size_t ndx,      ///< id of the transaction within the document
      drain_t     drain_id, ///< receiver of the document
      doc_id_t    doc_id,   ///< internal id of the document
      const T&    data      ///< transaction content
      ) = 0;
    /// @brief Builds the document header string for the given block.
    [[nodiscard]] virtual exp_result<str_t> prepare_header(
      //
      [[maybe_unused]] const Q&     q,        ///< readonly block of run qualifiers
      [[maybe_unused]] drain_t      drain_id, ///< receiver of the document
      [[maybe_unused]] doc_id_t     doc_id,   ///< internal id of the document
      [[maybe_unused]] const blk_t& block);   ///< transaction block content
    [[nodiscard]] virtual exp_result<str_t> prepare_footer(
      //
      const Q&     q,        ///< readonly block of run qualifiers
      drain_t      drain_id, ///< receiver of the document
      doc_id_t     doc_id,   ///< internal id of the document
      const blk_t& block     ///< transaction block content
    );
    /**
     * @brief Notifies the callback that a document has been fully written to the staging area.
     * @return true if the document is valid -- exporter then atomically moves it from the
     * staging area to its final destination, and the callback may consider the underlying
     * transactions committed. false is treated as a fatal run error (not a soft per-document
     * rejection): exporter signals every worker thread to stop and execute() returns an error.
     */
    [[nodiscard]] virtual bool document_prepared(
      //
      const Q& q,        ///< readonly block of run qualifiers
      drain_t  drain_id, ///< receiver of the document
      doc_id_t doc_id    ///< internal id of the document
      ) = 0;
    /**
     * @brief Called once per worker thread, right after clone() and before that thread's first
     * fetch_run_stat()/fetch_doc_data() call - the place for a concrete callback to open its own
     * database connection and PREPARE whatever statements fetch_doc_data()/document_prepared()
     * etc. go on to reuse for the rest of this worker thread's own lifetime, mirroring
     * fsp::pipeline_hooks::on_wrk_start() on the importer side (see that class's own doc comment).
     * Default is a no-op (nothing to do) - override only if the concrete callback needs per-worker
     * setup beyond what its own clone()/constructor already did.
     * @return {} on success, or the error to log and treat as this worker thread's own fatal
     * startup failure - see exporter_worker.hpp's own on_wrk_safe_start() for exactly how a
     * failure here is surfaced.
     */
    [[nodiscard]] virtual ev_result on_wrk_start(
      //
      [[maybe_unused]] int    worker_id,  ///< 0-based index of this worker thread
      [[maybe_unused]] cstr_t thread_name ///< this worker thread's own log name (see logger::Logger::log_name())
    );
    /**
     * @brief Called once per worker thread, after its loop has ended (normal exit, stop requested,
     * or a fatal error) - the place for a concrete callback to close whatever on_wrk_start() opened
     * (its own database connection, in particular), mirroring fsp::pipeline_hooks::on_wrk_end() on
     * the importer side. Default is a no-op. Errors are logged, never propagated (this runs during
     * worker shutdown, with nothing left to fail back to - see exporter_worker.hpp's own
     * on_wrk_safe_end()), same convention on_wrk_start()'s own doc comment documents for startup.
     */
    virtual void on_wrk_end(
      //
      [[maybe_unused]] int    worker_id,  ///< 0-based index of this worker thread
      [[maybe_unused]] cstr_t thread_name ///< this worker thread's own log name
    );
  protected:
    [[nodiscard]] const logger::Logger& lg() const noexcept;
  private:
    const logger::Logger* log_; // logger
  };
  /**
   * @brief Recommended base for a developer's own callback class: derive from
   * cb_exporter_crtp<my_cb, T, Q> instead of cb_exporter<T,Q> directly, and clone() is handled
   * correctly with zero extra code. Mirrors fsp::pipeline_hooks_crtp exactly.
   * @tparam Derived the developer's own concrete cb_exporter type (Curiously Recurring Template
   * Pattern) -- must be copy-constructible (clone() copies *this via Derived's own copy ctor).
   * @note The constructor is protected, not private+friend Derived -- protected still blocks
   * cb_exporter_crtp<X> from being instantiated as a standalone (non-CRTP) type, but (unlike
   * friend Derived, which only grants access to Derived itself) also allows an intermediate
   * mixin between cb_exporter_crtp and Derived, mirroring pipeline_hooks_crtp's own constructor
   * (pipeline_hooks.hpp) -- see that class's own doc comment for the worked example
   * (typed_semantic_check.hpp) this mirrors.
   */
  template <typename Derived, transaction_like T, qualifiers_like Q>
  class cb_exporter_crtp : public cb_exporter<T, Q>
  {
  protected:
    explicit cb_exporter_crtp(const logger::Logger& log) noexcept;
  public:
    [[nodiscard]] std::unique_ptr<cb_exporter<T, Q>> clone() const override;
  };
  ///////////////////////////////////////////////////////////////////////////////////////////////
  /**
   * @brief Binds this instance to the run's logger.
   * @details A concrete Derived's own constructor must forward its own logger::Logger argument
   * here (see cb_exporter_crtp's own constructor, which does exactly that). clone() (see below)
   * copies *this via Derived's copy constructor, so log_ -- a reference, trivially copyable --
   * is carried over to every worker thread's own clone automatically, without clone() itself
   * needing to know about it.
   */
  template <transaction_like T, qualifiers_like Q>
  inline cb_exporter<T, Q>::cb_exporter(const logger::Logger& log) noexcept
  : log_(&log)
  {
  }

  /**
   * @brief Builds a new file name for the document at the given position in the run.
   * @param qualifiers this run's caller-defined qualifiers
   * @param path directory the document will be staged/produced under
   * @param drain_id the drain this document belongs to -- NOT part of doc/opis_exporterja.txt's
   * original parameter list, added deliberately: block_number is only unique WITHIN one drain
   * (every drain's document numbering starts at 1), so without drain_id here, two different
   * drains could ask for the same block_number and -- unless the caller's own filename scheme
   * happens to already disambiguate by some other means -- collide on the same file name.
   * @param block_number 1-based index of this document within its drain
   * @param total_blocks total number of documents expected for this drain (may be an estimate)
   * @param filename_prefix caller-configured prefix (exporter_config_t::filename_prefix)
   */
  template <transaction_like T, qualifiers_like Q>
  inline exp_result<str_t> cb_exporter<T, Q>::fetch_doc_name(
    //
    const Q& q,               ///< readonly block of the run qualifiers
    cstr_t   path,            ///< path to the output file
    drain_t  drain_id,        ///< receiver of the document
    blk_id_t block_number,    ///< id of the block fetched from the source
    blk_id_t total_blocks,    ///< overall number of the blocks for this drain
    cstr_t   filename_prefix, ///< prefix of the filename
    cstr_t   filename_ext     ///< filename extension
  )
  {
    return fmt::format("{}/{}-{}-{}-{:02}-{:02}.{}", path, filename_prefix, q.run_id, drain_id, block_number, total_blocks, filename_ext);
  };

  template <transaction_like T, qualifiers_like Q>
  inline exp_result<str_t> cb_exporter<T, Q>::prepare_header(
    //
    const Q& /*q*/,        ///< readonly block of run qualifiers
    drain_t /*drain_id*/,  ///< receiver of the document
    doc_id_t /*doc_id*/,   ///< internal id of the document
    const blk_t& /*block*/ //< block of transactions
  )
  { return {}; }

  /// @brief Builds the document footer string for the given block.
  template <transaction_like T, qualifiers_like Q>
  inline exp_result<str_t> cb_exporter<T, Q>::prepare_footer(
    //
    const Q& /*q*/,        ///< readonly block of run qualifiers
    drain_t /*drain_id*/,  ///< receiver of the document
    doc_id_t /*doc_id*/,   ///< internal id of the document
    const blk_t& /*block*/ //< block of transactions
  )
  { return {}; }

  template <transaction_like T, qualifiers_like Q>
  inline ev_result cb_exporter<T, Q>::on_wrk_start(
    //
    [[maybe_unused]] int    worker_id,  ///< 0-based index of this worker thread
    [[maybe_unused]] cstr_t thread_name ///< this worker thread's own log name
  )
  { return {}; }

  template <transaction_like T, qualifiers_like Q>
  inline void cb_exporter<T, Q>::on_wrk_end(
    //
    [[maybe_unused]] int    worker_id,  ///< 0-based index of this worker thread
    [[maybe_unused]] cstr_t thread_name ///< this worker thread's own log name
  )
  {
  }

  /// @brief This run's logger
  template <transaction_like T, qualifiers_like Q>
  inline const logger::Logger& cb_exporter<T, Q>::lg() const noexcept
  { return *log_; }

  // Derived's own constructor must forward its own logger::Logger argument to ITS base class
  // initializer list, which reaches this constructor -- see e.g. cb_exporter's own constructor
  // doc comment for why this is enough to make log() valid on every clone too.
  template <typename Derived, transaction_like T, qualifiers_like Q>
  inline cb_exporter_crtp<Derived, T, Q>::cb_exporter_crtp(const logger::Logger& log) noexcept
  : cb_exporter<T, Q>(log)
  {
  }

  template <typename Derived, transaction_like T, qualifiers_like Q>
  inline std::unique_ptr<cb_exporter<T, Q>> cb_exporter_crtp<Derived, T, Q>::clone() const
  { return std::make_unique<Derived>(static_cast<const Derived&>(*this)); }
} // namespace fsp

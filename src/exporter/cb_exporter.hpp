#pragma once

/**
 * @file cb_exporter.hpp
 * @brief Callback interface a caller of fsp::exporter<T,Q> implements to supply document
 * content and receive lifecycle notifications, as specified in doc/opis_exporterja.txt.
 *
 * Templated analog of fsp::pipeline_hooks / pipeline_hooks_crtp (see
 * src/importer/pipeline_hooks.hpp), adapted to the exporter's own callback list. The caller
 * constructs one "prototype" cb_exporter instance and hands it to exporter<T,Q>::execute();
 * exporter clones it once per worker thread (see clone()) so each worker owns its own,
 * exclusively-used instance with its own private state -- no locking is ever needed inside a
 * cb_exporter method for its own state, since no two threads ever touch the same clone.
 *
 * Every method stays virtual (including the per-transaction prepare_transaction()): this is not
 * the same ultra-hot-path concern as pipeline_hooks::on_seg_sem_check(), and pipeline_hooks_crtp's own
 * doc comment already explains why a "detect override, skip the vtable call" optimization
 * doesn't even work under the Itanium ABI -- so no such attempt is made here either.
 */

#include "exporter_error.hpp"
#include "exporter_types.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>

namespace fsp
{
  /**
   * @brief Stateful, per-worker-thread callback interface for fsp::exporter<T,Q>.
   * @tparam T concrete transaction type, must derive from transaction_t (see transaction_like)
   * @tparam Q concrete run-qualifiers type, must derive from qualificators_t (see qualifiers_like)
   */
  template <transaction_like T, qualifiers_like Q>
  class cb_exporter // NOLINT(hicpp-special-member-functions) -- mirrors pipeline_hooks
  {
  public:
    virtual ~cb_exporter() = default;

    /// @brief Result of fetch_run_stat(): whole-run/whole-drain statistics for drain_id.
    /// Alias for the non-template run_stat_pair_t, which exporter_state (itself non-template)
    /// consumes directly -- see exporter_types.hpp.
    using run_stat_t = run_stat_pair_t;

    /**
     * @brief Makes a fresh, independent instance of the caller's concrete cb_exporter type.
     * @details Called exactly once per worker thread, at that thread's startup, by exporter<T,Q>.
     * See cb_exporter_crtp for a way to get this for free.
     */
    [[nodiscard]] virtual std::unique_ptr<cb_exporter> clone() const = 0;

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
    [[nodiscard]] virtual exp_result<str_t> fetch_doc_name(const Q&    qualifiers,
                                                           cstr_t      path,
                                                           int         drain_id,
                                                           std::size_t block_number,
                                                           std::size_t total_blocks,
                                                           cstr_t      filename_prefix) = 0;

    /**
     * @brief Reports whole-run statistics for one drain, used to resume a crashed run: the
     * "existing_doc_count" returned here is honored as the starting point for that drain's
     * document numbering, so a re-run after a crash continues rather than overwriting.
     */
    [[nodiscard]] virtual exp_result<run_stat_t> fetch_run_stat(const Q& qualifiers, int drain_id) = 0;

    /**
     * @brief Fetches one block of transactions for one document. See fetch_doc_data_status for
     * the three possible outcomes (a block, end-of-data for this drain, or an error).
     */
    [[nodiscard]] virtual fetch_doc_data_result_t<T> fetch_doc_data(const Q& qualifiers, int drain_id, std::uint64_t doc_id) = 0;

    /// @brief Converts one transaction's input data into its external/document string representation.
    [[nodiscard]] virtual exp_result<str_t> prepare_transaction(std::size_t ndx, int drain_id, std::uint64_t doc_id, const T& data) = 0;
    /// @brief Builds the document header string for the given block.
    [[nodiscard]] virtual exp_result<str_t> prepare_header(const Q&              qualifiers,
                                                           int                   drain_id,
                                                           std::uint64_t         doc_id,
                                                           const txn_block_t<T>& block) = 0;
    /// @brief Builds the document footer string for the given block.
    [[nodiscard]] virtual exp_result<str_t> prepare_footer(const Q&              qualifiers,
                                                           int                   drain_id,
                                                           std::uint64_t         doc_id,
                                                           const txn_block_t<T>& block) = 0;

    /**
     * @brief Notifies the callback that a document has been fully written to the staging area.
     * @return true if the document is valid -- exporter then atomically moves it from the
     * staging area to its final destination, and the callback may consider the underlying
     * transactions committed. false is treated as a fatal run error (not a soft per-document
     * rejection): exporter signals every worker thread to stop and execute() returns an error.
     */
    [[nodiscard]] virtual bool document_prepared(const Q& qualifiers, int drain_id, std::uint64_t doc_id) = 0;
  };

  /**
   * @brief Recommended base for a developer's own callback class: derive from
   * cb_exporter_crtp<my_cb, T, Q> instead of cb_exporter<T,Q> directly, and clone() is handled
   * correctly with zero extra code. Mirrors fsp::pipeline_hooks_crtp exactly.
   * @tparam Derived the developer's own concrete cb_exporter type (Curiously Recurring Template
   * Pattern) -- must be copy-constructible (clone() copies *this via Derived's own copy ctor).
   */
  template <typename Derived, transaction_like T, qualifiers_like Q>
  class cb_exporter_crtp : public cb_exporter<T, Q>
  {
    cb_exporter_crtp() = default;
  public:
    [[nodiscard]] std::unique_ptr<cb_exporter<T, Q>> clone() const override
    { return std::make_unique<Derived>(static_cast<const Derived&>(*this)); }
    friend Derived;
  };
} // namespace fsp

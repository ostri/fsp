#pragma once

/**
 * @file exporter_types.hpp
 * @brief Plain domain types for the fsp::exporter module, as specified in
 * doc/opis_exporterja.txt: the transaction/qualifier base types a caller derives
 * from, the drain configuration/statistics types, and the exporter's own run
 * configuration and result aggregates.
 *
 * Header-only: every type here is either trivial or a template, so none of it
 * needs a .cpp translation unit.
 */

#include "exporter_error.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fsp
{
  /// @brief unsigned 64-bit integer, used for transaction_t::id (see doc/opis_exporterja.txt).
  using txn_id_t = int64_t;

  /**
   * @brief Formats a uint128_t in base 10.
   * @details Neither fmt nor <iostream> support __int128 natively, so this is a small
   * repeated-division helper -- needed by anything that logs or displays a transaction id.
   */
  inline str_t to_string(txn_id_t v)
  {
    if (v == 0) { return "0"; }
    str_t digits;
    while (v > 0)
    {
      digits.push_back(static_cast<char>('0' + static_cast<int>(v % 10))); // NOLINT(readability-magic-numbers)
      v /= 10;                                                             // NOLINT(readability-magic-numbers)
    }
    std::ranges::reverse(digits);
    return digits;
  }

  // struct members below carry no trailing underscore, per project convention for new
  // struct (as opposed to class) members.

  /**
   * @brief Base type for one exportable transaction; a caller derives their own concrete
   * transaction type T from this (see the transaction_like concept below).
   * @details Deliberately a plain struct with public data and a polymorphic (virtual) destructor
   * only -- it exists purely so transaction_like/derived_from can constrain T, not to enforce
   * invariants of its own, so no copy/move special members are declared either (mirrors
   * fsp::pipeline_hooks's own NOLINT(hicpp-special-member-functions) rationale in
   * src/importer/pipeline_hooks.hpp: a base meant only to be derived from, never sliced or
   * stored by value on its own).
   */
  struct transaction_t // NOLINT(hicpp-special-member-functions)
  {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes) -- plain data struct by design
    txn_id_t id   = 0; ///< unique transaction id (snowflake)
    int      type = 0; ///< transaction type
    str_t    value;    ///< transaction content (input to cb_exporter::prepare_transaction())
    // NOLINTEND(misc-non-private-member-variables-in-classes)
    virtual ~transaction_t() = default;
  };

  /**
   * @brief Base type for one run's qualifiers; a caller derives their own concrete
   * qualifiers type Q from this (see the qualifiers_like concept below). exporter itself
   * never interprets a qualifier's meaning -- only cb_exporter's concrete implementation does.
   * @details See transaction_t's @details for why this has no copy/move special members either.
   */
  struct qualificators_t // NOLINT(hicpp-special-member-functions)
  {
    int run_id                 = 0; // NOLINT(misc-non-private-member-variables-in-classes) -- plain data struct by design
    virtual ~qualificators_t() = default;
  };

  /// @brief Constrains exporter<T,Q>/cb_exporter<T,Q>'s T parameter to a transaction_t subtype.
  template <typename T>
  concept transaction_like = std::derived_from<T, transaction_t>;
  /// @brief Constrains exporter<T,Q>/cb_exporter<T,Q>'s Q parameter to a qualificators_t subtype.
  template <typename Q>
  concept qualifiers_like = std::derived_from<Q, qualificators_t>;

  /// @brief One block of transactions, as returned by cb_exporter::fetch_doc_data() for one document.
  template <transaction_like T>
  using txn_block_t = std::vector<T>;

  /// @brief Static description of one recipient ("drain"), supplied by the caller at construction.
  struct drain_dscr_t
  {
    drain_t     id = 0;          ///< unique drain id
    str_t       name;            ///< short drain name (e.g. a BIC)
    str_t       dscr;            ///< longer drain name (e.g. a bank's full name)
    std::size_t max_doc_txn = 0; ///< maximum number of transactions per document for this drain
  };

  /**
   * @brief Per-drain run statistics, lazily loaded once per drain via cb_exporter::fetch_run_stat().
   * @details last_doc_id is atomic (per doc/opis_exporterja.txt's "zadnji zaseden id dokumenta /
   * atomic") so document-id allocation is lock-free once a drain's stats are loaded. This makes
   * the type non-copyable/non-movable -- exporter_state sizes its vector of these once, at
   * construction, and never resizes it afterward, so no relocation ever happens.
   */
  struct drain_statistic_t
  {
    bool                     loaded                   = false; ///< whether the fields below are populated yet
    std::size_t              initial_txn_count        = 0;     ///< # transactions remaining at run start
    std::size_t              initial_doc_count        = 0;     ///< # pre-existing documents at run start
    std::size_t              initial_future_doc_count = 0;     ///< computed: initial_txn_count / max_doc_txn + 1
    std::atomic<std::size_t> last_doc_id{0};                   ///< last occupied document id, numbered from 1
  };

  /// @brief Bookkeeping for one document, made or currently in progress.
  struct doc_statistics_t
  {
    str_t                                 doc_name; ///< full path, once known
    std::size_t                           txn_count      = 0;
    bool                                  header_written = false;
    bool                                  footer_written = false;
    std::chrono::steady_clock::time_point start_ts;
    std::chrono::milliseconds             duration{0};
    int                                   worker_id = -1; ///< id of the worker thread that produced this document
    drain_t                               drain_id  = -1; ///< drain this document belongs to
  };

  /**
   * @brief Whole-run/whole-drain statistics returned by cb_exporter::fetch_run_stat(): a
   * non-template type (independent of T/Q) so exporter_state -- which is itself
   * non-template -- can consume it directly. cb_exporter<T,Q>::run_stat_t is an alias for this.
   */
  struct run_stat_pair_t
  {
    std::size_t remaining_txn_count = 0; ///< # transactions not yet exported for this drain
    std::size_t existing_doc_count  = 0; ///< # documents already produced for this drain (e.g. before a crash)
  };

  /**
   * @brief One phase-2 unit of work, as published into exporter_state's own work queue by phase 1
   * (see cb_exporter::compute_drain_stat()). fsp itself never looks inside the block this doc_id
   * refers to -- only WHICH drain and WHICH doc_id are its own concern; the block's own content
   * (e.g. a caller-defined id range) lives entirely in the concrete cb_exporter's own state,
   * looked up again from doc_id when fetch_doc_data() is called for it in phase 2.
   */
  struct drain_doc_slot_t
  {
    drain_t  drain_id = 0;
    doc_id_t doc_id   = 0;
  };

  /// @brief One entry of the caller-supplied drain list (exporter_config_t::drain_list).
  struct exporter_drain_cfg_t
  {
    std::uint8_t id = 0;
    str_t        name;
    std::size_t  max_doc_txn = 0;
  };

  /**
   * @brief Run configuration handed to exporter<T,Q>'s constructor.
   * @details Run qualifiers are deliberately NOT a field here: they are the exporter<T,Q>/
   * exporter_worker<T,Q> template parameter Q itself (see qualifiers_like above), built by the
   * caller and passed separately to exporter's constructor. exporter_config_t only carries the
   * qualifier-agnostic plumbing (drains, thread count, filename shape) that exporter itself
   * understands - no directory of its own: cb_exporter::fetch_doc_name() alone decides every
   * document's own full path (directory hierarchy included), and exporter_worker derives its own
   * tmp path from that same result (see exporter_worker.hpp's own resolve_unique_tmp_path()) -
   * there is nothing left for exporter_config_t itself to carry a directory for.
   */
  struct exporter_config_t
  {
    std::vector<exporter_drain_cfg_t> drain_list;
    std::size_t                       number_of_threads = 0;
    str_t                             filename_prefix;
    str_t                             filename_ext = "xml"; ///< passed through to fetch_doc_name(), no leading dot
  };

  /**
   * @brief The three possible outcomes of cb_exporter::fetch_doc_data(): a block of
   * transactions, an explicit end-of-data signal, or an error signal.
   * @details A plain std::expected<txn_block_t<T>, exp_error_info> only has room for
   * "value" or "error", which would force "no more data for this drain" -- a normal, expected
   * end-of-work condition -- to be encoded as an error too, blurring it together with a real
   * failure. This explicit status enum keeps "drain exhausted" and "something went wrong"
   * distinguishable at the call site.
   */
  enum class fetch_doc_data_status : std::uint8_t
  {
    ok,           ///< block is valid
    no_more_data, ///< this drain has no more transactions; block/error are both unused
    error,        ///< error is valid
  };

  /// @brief Return type of cb_exporter::fetch_doc_data(); see fetch_doc_data_status.
  template <transaction_like T>
  struct fetch_doc_data_result_t
  {
    fetch_doc_data_status status = fetch_doc_data_status::error;
    txn_block_t<T>        block; ///< valid only when status == ok
    exp_error_info        error; ///< valid only when status == error
  };

  /**
   * @brief Per-worker-thread run statistics, accumulated privately (no locking) by one
   * exporter_worker<T,Q> as it produces documents.
   * @details Never read by anyone else while the worker is running. Only once every worker
   * thread has been joined does exporter<T,Q>::execute() read each worker's final
   * exporter_thread_stats_t and sum them into the single exporter_run_stats_t below, which is
   * what execute() actually returns to the caller -- i.e. this struct is a private per-thread
   * accumulator, exporter_run_stats_t is the public whole-run result. Since every failure
   * anywhere aborts the whole run (see cb_exporter.hpp), there is no failed-document counter to
   * track here: a worker's documents are always successful, or the run never completes at all.
   */
  struct exporter_thread_stats_t
  {
    std::size_t successful_documents = 0;
    std::size_t total_transactions   = 0;
    double      processing_time_ms   = 0.0;
  };

  /**
   * @brief The public result of exporter<T,Q>::execute() on success -- the sum of every worker's
   * exporter_thread_stats_t, computed once after all workers have joined. This is
   * doc/opis_exporterja.txt's "main thread computes run statistics" step.
   */
  struct exporter_run_stats_t
  {
    std::size_t total_documents    = 0;
    std::size_t total_transactions = 0;
    double      elapsed_ms         = 0.0;
  };
} // namespace fsp

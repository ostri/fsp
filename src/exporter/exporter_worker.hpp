#pragma once

/**
 * @file exporter_worker.hpp
 * @brief Per-thread execution unit for fsp::exporter<T,Q>, implementing the worker loop from
 * doc/opis_exporterja.txt: pick a drain, fetch a block of transactions, assemble a document via
 * xml_writer, hand it off through cb_exporter::document_prepared(), repeat until every drain is
 * exhausted or a stop is requested.
 *
 * Its own class (not a private nested class or free function inside exporter<T,Q>), mirroring
 * src/importer/pipeline.hpp / pipeline_worker.hpp's split -- this keeps a worker independently
 * constructible/testable without spinning up exporter<T,Q>::execute()'s whole thread pool, and
 * matches the established precedent in this codebase (main class owns shared state + mutexes, a
 * separate per-thread class owns the per-thread toolkit and runs operator()).
 *
 * Error model (confirmed with the user, see the design plan): ANY failure anywhere in the making
 * of one document -- any cb_exporter call returning an error, any xml_writer I/O failure, or
 * cb_exporter::document_prepared() returning false -- is fatal to the whole run. There is no
 * per-document soft-failure path: a failure logs, records the error, calls
 * exporter_state::request_stop() (which every worker observes), and this worker's loop ends.
 */

#include "cb_exporter.hpp"
#include "exporter_error.hpp"
#include "exporter_state.hpp"
#include "exporter_types.hpp"
#include "xml_writer.hpp"
#include <chrono>
#include <filesystem>
#include <logger/logger.hpp>
#include <memory>
#include <optional>

namespace fsp
{
  namespace fs = std::filesystem;

  /**
   * @brief Runs one worker thread's share of an exporter<T,Q> run.
   * @tparam T concrete transaction type (see transaction_like)
   * @tparam Q concrete run-qualifiers type (see qualifiers_like)
   */
  template <transaction_like T, qualifiers_like Q>
  class exporter_worker
  {
  public:
    /// @brief Constructs a worker; proto_cb is cloned once here so this thread owns its own instance.
    exporter_worker(exporter_state&          state,
                    const exporter_config_t& cfg,
                    const Q&                 qualifiers,
                    const logger::Logger&    log,
                    str_t                    parent_log_name,
                    cb_exporter<T, Q>&       proto_cb)
    : state_(state)
    , cfg_(cfg)
    , qualifiers_(qualifiers)
    , log_(log)
    , parent_log_name_(std::move(parent_log_name))
    , cb_(proto_cb.clone())
    {
    }

    /// @brief Runs this worker's whole loop; call once, from its own std::jthread.
    void operator()(int worker_id)
    {
      logger::Logger::make_log_name(parent_log_name_, fmt::format("exp-wrk.{:02}", worker_id));
      const auto thread_name = logger::Logger::log_name();
      log_.info("{}: worker started", thread_name);

      // The one place a concrete cb_exporter opens its own per-worker-thread resources (database
      // connection, PREPAREd statements fetch_doc_data()/document_prepared() etc. go on to reuse)
      // - see cb_exporter::on_wrk_start()'s own doc comment. A failure here is fatal: nothing this
      // worker thread does afterward (fetch_run_stat()/fetch_doc_data() in particular) has any
      // chance of working without whatever on_wrk_start() itself was supposed to set up, so this
      // skips straight to on_wrk_end()/writer_.close() rather than entering the loop at all.
      if (auto res = cb_->on_wrk_start(worker_id, thread_name); ! res)
      {
        std::ignore = fail(exp_error::wrk_start_failed, str_t(res.error().message()), 0, 0);
      }
      else
      {
        const auto thread_start = std::chrono::steady_clock::now();

        while (true)
        {
          if (state_.stop_token().stop_requested())
          {
            log_.warn("{}: stop requested -- forced exit", thread_name);
            break;
          }
          if (state_.available_drains_empty())
          {
            log_.info("{}: no drains left -- normal exit", thread_name);
            break;
          }
          if (! run_one_iteration(worker_id, thread_name)) { break; } // a fatal error occurred; stop already requested
        }

        stats_.processing_time_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - thread_start).count();
      }

      cb_->on_wrk_end(worker_id, thread_name); // mirrors on_wrk_start() above - closes whatever it opened, even after a fatal error
      writer_.close(); // safety net -- xml_writer::close() is itself idempotent and no-throw
      log_.info("{}: worker done ({} document(s), {} transaction(s))", thread_name, stats_.successful_documents, stats_.total_transactions);
    }

    [[nodiscard]] const exporter_thread_stats_t& stats() const noexcept { return stats_; }
    [[nodiscard]] bool                           fatal() const noexcept { return fatal_error_.has_value(); }
    /// @brief The fatal error this worker recorded. Precondition: fatal() == true (mirrors
    /// std::optional::value()'s own unchecked operator* -- callers (see exporter::execute(),
    /// which always checks fatal() first) are expected to guard this themselves).
    [[nodiscard]] const exp_error_info& fatal_error() const noexcept
    { return *fatal_error_; } // NOLINT(bugprone-unchecked-optional-access) -- precondition documented above
  private:
    /**
     * @brief One iteration of the worker loop's body (everything below the top-of-loop stop/empty
     * checks in operator()).
     * @return false if a fatal error occurred (stats_/fatal_error_ set, state_.request_stop()
     * already called) -- the caller must stop its loop; true to continue looping.
     */
    bool run_one_iteration(int worker_id, cstr_t thread_name)
    {
      const auto drain_id_opt = select_work_drain();
      if (! drain_id_opt.has_value()) { return true; } // lost a race for the last drain -- retry top-of-loop checks
      const drain_t drain_id = *drain_id_opt;

      ensure_drain_stats_loaded(drain_id);
      if (! state_.is_drain_loaded(drain_id)) { return fail(exp_error::fetch_run_stat_failed, "fetch_run_stat failed", drain_id, 0); }

      // doc_id must be known BEFORE fetch_doc_data() is called -- it is one of that call's own
      // parameters per doc/opis_exporterja.txt ("fetch_doc_data (..., drain-id, id-dokumenta)").
      // Allocating it here does mean a document id can be "used up" if this block turns out to
      // be empty (no_more_data) -- harmless, since ids only need to be unique, not contiguous.
      const doc_id_t doc_id = state_.next_doc_id(drain_id);

      auto fetched = cb_->fetch_doc_data(qualifiers_, drain_id, doc_id);
      if (fetched.status == fetch_doc_data_status::error)
      {
        return fail(exp_error::fetch_doc_data_failed, str_t(fetched.error.message()), drain_id, doc_id);
      }
      if (fetched.status == fetch_doc_data_status::no_more_data || fetched.block.empty())
      {
        work_drain_id_.reset();
        state_.remove_available_drain(drain_id); // safety net -- should already be absent in the common case below
        return true;
      }

      const auto* drain_dscr = state_.find_drain(drain_id);
      if (drain_dscr == nullptr) { return fail(exp_error::invalid_config, "unknown drain id", drain_id, doc_id); }
      if (fetched.block.size() < drain_dscr->max_doc_txn)
      {
        // Fewer transactions than a full block -- this was the last (partial) block for this
        // drain, so there is no point waiting for a subsequent no_more_data signal.
        state_.remove_available_drain(drain_id);
      }

      const auto        start_ts     = std::chrono::steady_clock::now();
      const std::size_t doc_stat_ndx = state_.register_doc_start(doc_statistics_t{.doc_name       = "",
                                                                                  .txn_count      = 0,
                                                                                  .header_written = false,
                                                                                  .footer_written = false,
                                                                                  .start_ts       = start_ts,
                                                                                  .duration       = {},
                                                                                  .worker_id      = worker_id,
                                                                                  .drain_id       = drain_id});

      auto write_res = write_document(drain_id, doc_id, fetched.block, doc_stat_ndx);
      if (! write_res.has_value())
      {
        const auto& err = write_res.error();
        return fail(err.code(), str_t(err.message()), drain_id, doc_id);
      }

      const bool prepared_ok = cb_->document_prepared(qualifiers_, drain_id, doc_id);
      if (! prepared_ok)
      {
        // Confirmed with the user: false is ALWAYS a fatal system error, not a soft
        // per-document rejection -- regardless of whether moving the tmp file elsewhere for
        // diagnostics would itself succeed.
        return fail(exp_error::document_rejected, "document_prepared() returned false", drain_id, doc_id);
      }

      auto move_res = move_to_final(write_res.value(), drain_id, doc_id);
      if (! move_res.has_value())
      {
        const auto& err = move_res.error();
        return fail(err.code(), str_t(err.message()), drain_id, doc_id);
      }

      state_.finalize_doc(
        doc_stat_ndx,
        doc_statistics_t{.doc_name       = move_res.value(),
                         .txn_count      = fetched.block.size(),
                         .header_written = true,
                         .footer_written = true,
                         .start_ts       = start_ts,
                         .duration  = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_ts),
                         .worker_id = worker_id,
                         .drain_id  = drain_id});
      stats_.successful_documents += 1;
      stats_.total_transactions += fetched.block.size();
      state_.increment_drain_doc_count(drain_id);
      log_.info("{}: document {} (drain {}) moved to target", thread_name, doc_id, drain_id);
      return true;
    }

    /// @brief "keep it if we already have one, else consult state_" -- see exporter_state::pick_or_keep_drain().
    [[nodiscard]] std::optional<drain_t> select_work_drain()
    {
      auto picked    = state_.pick_or_keep_drain(work_drain_id_);
      work_drain_id_ = picked;
      return picked;
    }

    /// @brief Lazily loads drain_id's initial run statistics, once, via cb_->fetch_run_stat().
    void ensure_drain_stats_loaded(drain_t drain_id)
    {
      if (state_.is_drain_loaded(drain_id)) { return; } // fast path -- already loaded by some worker
      const auto*       drain_dscr  = state_.find_drain(drain_id);
      const std::size_t max_doc_txn = drain_dscr == nullptr ? 0 : drain_dscr->max_doc_txn;
      state_.load_drain_stat_if_needed(drain_id,
                                       max_doc_txn,
                                       [this, drain_id]() -> exp_result<run_stat_pair_t>
                                       {
                                         auto res = cb_->fetch_run_stat(qualifiers_, drain_id);
                                         if (! res.has_value()) { return std::unexpected(res.error()); }
                                         return run_stat_pair_t{.remaining_txn_count = res->remaining_txn_count,
                                                                .existing_doc_count  = res->existing_doc_count};
                                       });
    }

    /**
     * @brief Writes one document to a staged tmp path: header, every transaction, footer, then
     * finalize()/close(). Delegates the spec's "accumulate transactions, flush on limit" step
     * entirely to xml_writer::append()'s own internal batching (BATCH_SIZE/flush_batch()) --
     * calling append() once per transaction string, rather than re-implementing a second
     * accumulation layer on top of it.
     * @return the tmp path on success.
     */
    [[nodiscard]] exp_result<str_t> write_document(drain_t               drain_id,
                                                   doc_id_t              doc_id,
                                                   const txn_block_t<T>& block,
                                                   std::size_t           doc_stat_ndx)
    {
      auto name_res = resolve_unique_doc_name(drain_id, doc_id);
      if (! name_res.has_value()) { return std::unexpected(name_res.error()); }
      // Not const: the final `return tmp_path;` below relies on NRVO/automatic move, which a
      // const local would silently downgrade to a copy.
      str_t tmp_path = (fs::path(cfg_.tmp_dir) / *name_res).string();

      if (auto res = writer_.open(tmp_path.c_str()); ! res)
      {
        return std::unexpected(wrap_xml_writer_error(res.error(), tmp_path, drain_id, doc_id));
      }

      // header_res is written ONLY via writer_.finalize() below, never via writer_.append() here -
      // xml_writer::finalize(header) already places it at the very start of the file (into the
      // space open() reserved for exactly this purpose - see xml_writer.hpp's own class comment
      // and usage example); appending it here too would duplicate it, once at the reserved offset
      // (finalize()'s copy) and once inline in the batch stream (this append()'s copy).
      auto header_res = cb_->prepare_header(qualifiers_, drain_id, doc_id, block);
      if (! header_res.has_value()) { return std::unexpected(header_res.error()); }

      for (std::size_t ndx = 0; ndx < block.size(); ++ndx)
      {
        auto txn_res = cb_->prepare_transaction(ndx, drain_id, doc_id, block.at(ndx));
        if (! txn_res.has_value()) { return std::unexpected(txn_res.error()); }
        if (auto res = writer_.append(*txn_res); ! res)
        {
          return std::unexpected(wrap_xml_writer_error(res.error(), tmp_path, drain_id, doc_id));
        }
      }

      auto footer_res = cb_->prepare_footer(qualifiers_, drain_id, doc_id, block);
      if (! footer_res.has_value()) { return std::unexpected(footer_res.error()); }
      if (auto res = writer_.append(*footer_res); ! res)
      {
        return std::unexpected(wrap_xml_writer_error(res.error(), tmp_path, drain_id, doc_id));
      }

      if (auto res = writer_.finalize(*header_res); ! res)
      {
        return std::unexpected(wrap_xml_writer_error(res.error(), tmp_path, drain_id, doc_id));
      }
      writer_.close();
      std::ignore = doc_stat_ndx; // header/footer flags recorded by the caller once the move succeeds
      return tmp_path;
    }

    /**
     * @brief Asks cb_->fetch_doc_name() for a candidate name, retrying with a worker-appended
     * random suffix (before the extension) on collision, bounded to avoid an infinite loop --
     * matching doc/opis_exporterja.txt's "a random number is appended to the existing name
     * before the extension, repeated until it succeeds". cb_exporter itself is never told about
     * collisions -- the spec's own wording places that responsibility on the exporter/worker
     * layer, not the callback's semantics.
     */
    [[nodiscard]] exp_result<str_t> resolve_unique_doc_name(drain_t drain_id, doc_id_t doc_id)
    {
      static constexpr int MAX_ATTEMPTS = 10;

      auto name_res = cb_->fetch_doc_name(qualifiers_, cfg_.tmp_dir, drain_id, doc_id, 0, cfg_.filename_prefix, cfg_.filename_ext);
      if (! name_res.has_value()) { return std::unexpected(name_res.error()); }

      str_t candidate = *name_res;
      for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt)
      {
        if (! fs::exists(fs::path(cfg_.tmp_dir) / candidate)) { return candidate; }

        // A sequential counter (rather than a random suffix) keeps this step deterministic and
        // testable, while still matching the spec's "append a number before the extension,
        // retry until it succeeds" -- uniqueness across concurrent workers is guaranteed by the
        // fs::exists() probe itself, not by the suffix's own entropy.
        const fs::path original(*name_res);
        candidate = fmt::format("{}_{}{}", original.stem().string(), attempt + 1, original.extension().string());
      }
      return std::unexpected(
        exp_error_info{exp_error::file_rename_collision, "collision retry attempts exhausted", candidate, drain_id, doc_id});
    }

    /// @brief Atomically moves tmp_path to cfg_.target_dir, returning the final path on success.
    [[nodiscard]] exp_result<str_t> move_to_final(const str_t& tmp_path, drain_t drain_id, doc_id_t doc_id)
    {
      const fs::path  final_path = fs::path(cfg_.target_dir) / fs::path(tmp_path).filename();
      std::error_code ec;
      fs::rename(tmp_path, final_path, ec); // atomic only when tmp_dir/target_dir share a filesystem
      if (ec) { return std::unexpected(exp_error_info{exp_error::file_move_failed, ec.message(), tmp_path, drain_id, doc_id}); }
      return final_path.string();
    }

    /// @brief Translates a fsp::error_info from xml_writer into this module's own error type.
    [[nodiscard]] static exp_error_info wrap_xml_writer_error(const error_info& src, cstr_t path, drain_t drain_id, doc_id_t doc_id)
    { return exp_error_info{exp_error::xml_writer_error, str_t(src.message()), path, drain_id, doc_id}; }

    /// @brief Records a fatal error and signals every worker to stop. Always returns false.
    bool fail(exp_error code, str_t msg, drain_t drain_id, doc_id_t doc_id)
    {
      fatal_error_ = exp_error_info{code, std::move(msg), "", drain_id, doc_id};
      log_.error("{}", fatal_error_->to_string());
      state_.request_stop();
      return false;
    }
  private:
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members) -- exporter_worker is
    // neither copied nor moved (mirrors pipeline_worker.hpp's identical rationale/precedent),
    // constructed once by exporter<T,Q>::execute() and driven exclusively through operator(), so
    // outliving every referenced object (state_/cfg_/qualifiers_/log_, all owned by the exporter
    // or its caller for the run's whole duration) is guaranteed by construction.
    exporter_state&          state_;
    const exporter_config_t& cfg_;
    const Q&                 qualifiers_;
    const logger::Logger&    log_;
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
    str_t                              parent_log_name_;
    std::unique_ptr<cb_exporter<T, Q>> cb_;     // this thread's own clone, made once at construction
    xml_writer                         writer_; // reopened fresh per document
    std::optional<drain_t>             work_drain_id_;
    exporter_thread_stats_t            stats_{};
    std::optional<exp_error_info>      fatal_error_;
  };
} // namespace fsp

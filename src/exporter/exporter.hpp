#pragma once

/**
 * @file exporter.hpp
 * @brief Main class of the fsp::exporter module: owns the shared exporter_state, starts and
 * joins one worker thread per exporter_config_t::number_of_threads, and computes final run
 * statistics -- matching doc/opis_exporterja.txt's spec: the main thread starts all worker
 * threads, waits for them to finish, then computes run statistics. Analogous to
 * src/importer/pipeline.hpp.
 *
 * Header-only, like exporter_worker.hpp: exporter<T,Q> is a template over the caller's own
 * transaction/qualifiers types, so there is no non-template translation unit to put in a .cpp.
 */

#include "cb_exporter.hpp"
#include "exporter_error.hpp"
#include "exporter_state.hpp"
#include "exporter_types.hpp"
#include "exporter_worker.hpp"
#include <algorithm>
#include <chrono>
#include <logger/logger.hpp>
#include <memory>
#include <thread>
#include <vector>

namespace fsp
{
  /**
   * @brief Runs a maximally parallel export of documents built from T-typed transactions, using
   * Q-typed run qualifiers and a caller-supplied cb_exporter<T,Q> implementation.
   * @tparam T concrete transaction type (see transaction_like)
   * @tparam Q concrete run-qualifiers type (see qualifiers_like)
   */
  template <transaction_like T, qualifiers_like Q>
  class exporter
  {
  public:
    exporter(exporter_config_t cfg, Q qualifiers, const logger::Logger& log, str_t parent_log_name)
    : cfg_(std::move(cfg))
    , qualifiers_(std::move(qualifiers))
    , log_(log)
    , parent_log_name_(std::move(parent_log_name))
    , state_(build_drain_list(cfg_), std::min(cfg_.drain_list.size(), cfg_.number_of_threads))
    {
    }

    /**
     * @brief Runs the whole export: calls proto_cb.on_init() once on this thread, then starts
     * cfg_.number_of_threads worker threads (each cloning proto_cb once), waits for all of them to
     * finish, and either returns the aggregated run statistics or the first fatal error any worker
     * (or on_init() itself) recorded.
     * @param proto_cb the caller's prototype callback instance; mutated here only by on_init() (see
     * cb_exporter::on_init()'s own doc comment), otherwise only cloned once per worker thread (see
     * cb_exporter::clone()).
     */
    [[nodiscard]] exp_result<exporter_run_stats_t> exec(cb_exporter<T, Q>& proto_cb)
    {
      if (auto res = validate_config(); ! res) { return std::unexpected(res.error()); }

      // Runs once, on this (the main) thread, before any worker thread is started -- see
      // cb_exporter::on_init()'s own doc comment. proto_cb is the very instance clone() goes on
      // to copy once per worker thread below, so whatever on_init() sets on it here is visible to
      // every clone already, before that clone's own on_wrk_start() ever runs.
      if (auto res = proto_cb.on_init(); ! res)
      {
        log_.error("exporter: on_init() failed -- {}", res.error().to_string());
        return std::unexpected(res.error());
      }

      // Phase-1 participant count -- see exporter_state's own constructor doc comment and
      // exporter_worker::operator()'s own phase-1/phase-2 split: only the first n_phase1 workers
      // (below) take part in phase 1 at all (compute_drain_stat() for one drain each, then
      // work-steal among themselves via pick_or_keep_drain() until every drain is claimed) - any
      // worker beyond that (only possible when cfg_.number_of_threads > cfg_.drain_list.size())
      // has no drain left to claim from the very start, so it skips phase 1 entirely and goes
      // straight to phase 2's own work-queue loop instead of occupying a phase-1 slot it could
      // never use.
      const auto n_phase1 = std::min(cfg_.drain_list.size(), cfg_.number_of_threads);
      log_.info(
        "exporter: starting {} thread(s) ({} in phase 1) for {} drain(s)", cfg_.number_of_threads, n_phase1, cfg_.drain_list.size());
      const auto start_time = std::chrono::steady_clock::now();

      std::vector<std::unique_ptr<exporter_worker<T, Q>>> workers;
      workers.reserve(cfg_.number_of_threads);
      for (std::size_t i = 0; i < cfg_.number_of_threads; ++i)
      {
        workers.push_back(std::make_unique<exporter_worker<T, Q>>(state_, cfg_, qualifiers_, log_, parent_log_name_, proto_cb));
      }

      {
        std::vector<std::jthread> threads;
        threads.reserve(workers.size());
        for (std::size_t i = 0; i < workers.size(); ++i)
        {
          const bool is_phase1_worker = i < n_phase1;
          threads.emplace_back([w = workers.at(i).get(), worker_id = static_cast<int>(i), is_phase1_worker]
                               { (*w)(worker_id, is_phase1_worker); });
        }
        // threads' destructor joins every std::jthread as this scope exits -- matching
        // doc/opis_exporterja.txt's "main thread waits for every worker to finish" step.
      }

      const double elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_time).count();

      for (const auto& w : workers)
      {
        if (w->fatal())
        {
          log_.error("exporter: run aborted -- {}", w->fatal_error().to_string());
          return std::unexpected(w->fatal_error());
        }
      }

      const auto stats = aggregate_stats(workers, elapsed_ms);
      log_.info("exporter: run complete -- {} document(s), {} transaction(s), {:.1f} ms",
                stats.total_documents,
                stats.total_transactions,
                stats.elapsed_ms);
      return stats;
    }

    [[nodiscard]] const exporter_state& state() const noexcept { return state_; }
  private:
    /// @brief Builds the drain_static_ list exporter_state needs from the caller's config.
    [[nodiscard]] static std::vector<drain_dscr_t> build_drain_list(const exporter_config_t& cfg)
    {
      std::vector<drain_dscr_t> drains;
      drains.reserve(cfg.drain_list.size());
      for (const auto& d : cfg.drain_list)
      {
        drains.push_back(drain_dscr_t{.id = d.id, .name = d.name, .dscr = d.name, .max_doc_txn = d.max_doc_txn});
      }
      return drains;
    }

    [[nodiscard]] ev_result validate_config() const
    {
      if (cfg_.drain_list.empty()) { return std::unexpected(exp_error_info{exp_error::invalid_config, "drain_list is empty"}); }
      if (cfg_.number_of_threads == 0) { return std::unexpected(exp_error_info{exp_error::invalid_config, "number_of_threads is zero"}); }
      return {};
    }
    /**
     * @brief calculate statistics for all workers
     *
     * @param workers list of workers
     * @param elapsed_ms acumulated time for all workers
     * @return exporter_run_stats_t statistical block
     */
    [[nodiscard]] static exporter_run_stats_t aggregate_stats( //
      const std::vector<std::unique_ptr<exporter_worker<T, Q>>>& workers,
      double                                                     elapsed_ms)
    {
      exporter_run_stats_t total;
      for (const auto& w : workers)
      {
        total.total_documents += w->stats().successful_documents;
        total.total_transactions += w->stats().total_transactions;
      }
      total.elapsed_ms = elapsed_ms;
      return total;
    }
  private:
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members) -- exporter is neither
    // copied nor moved (see pipeline.hpp for the identical rationale/precedent), only ever
    // constructed once and driven through execute(), so a reference member is safe here.
    exporter_config_t     cfg_;             ///< configuration of the exporter
    Q                     qualifiers_;      ///< readonly block of data that all worker share
    const logger::Logger& log_;             ///< central logger
    str_t                 parent_log_name_; ///< parent log name so that each worker is
                                            ///< uniquelly identified
    exporter_state state_;                  ///< built from cfg_.drain_list, see build_drain_list()
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
  };
} // namespace fsp

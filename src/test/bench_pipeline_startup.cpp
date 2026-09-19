// bench_pipeline_startup.cpp
//
// Temporary, standalone micro-benchmark: how expensive is it to construct fsp::pipeline's own
// internal worker infrastructure -- specifically the cost of pipeline::start_workers(), which
// loops calling pipeline_worker::init() for each worker (this sets up a doc_cutter per worker,
// which in turn sets up an XercesC SAX2XMLReader + grammar pool) -- at a realistic/maximal thread
// count?
//
// CAVEAT: pipeline::start_workers() itself is private (and pipeline::ds_dscr() only exposes a
// const accessor, so there is no public way to get a *pipeline's own* internal ds_dscr_ to
// has_grammar()==true without going through the private add_documents()/process_files() path
// too). So this benchmark instead reproduces start_workers()'s own loop body verbatim, from
// outside, using pipeline_worker's public constructor + public init():
//
//   auto w = std::make_unique<pipeline_worker>(pl, cfg, log, parent_log_name, hooks);
//   w->init();
//
// -- see pipeline.cpp's start_workers() for the loop this mirrors exactly (identical constructor
// arguments, identical init() call, identical failure check). Because pipeline's own ds_dscr_
// never gets a grammar loaded this way, doc_cutter::init() always takes the has_grammar()==false
// branch (setup_parser_no_validation()) -- still a full, real XercesC SAX2XMLReader construction
// (createXMLReader() + feature/property setup + Handler wiring), just not also loading/locking an
// XMLGrammarPoolImpl against an XSD. No document is ever cut/validated/stored: this isolates only
// the one-time worker-construction cost pipeline::process_files() pays once per call, never the
// per-document processing pipeline::run_workers() would add on top.
#include "pipeline.hpp"
#include "pipeline_worker.hpp"
#include "pipeline_hooks.hpp"
#include "importer_config.hpp"
#include "doc_cutter.hpp"
#include "doc_set_dscr.hpp"
#include "segment_pool.hpp"
#include "work.hpp" // fsp::work schema + fsp::proc_data_of<^^fsp::work>() -- same schema pacs8.cpp uses
#include "xerces_mgr.hpp" // RAII XMLPlatformUtils::Initialize()/Terminate() -- normally owned by fsp::importer, which this benchmark bypasses
#include <logger/logger.hpp>
#include <logger/logger_config.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <fmt/format.h>
#include <numeric>
#include <vector>

namespace
{
  using clock_t     = std::chrono::steady_clock;
  using duration_ms = std::chrono::duration<double, std::milli>;

  // Constructs `num_workers` pipeline_worker instances against `pl`/`cfg`/`log`/`hooks` (the exact
  // same sequence pipeline::start_workers() runs internally) and returns the wall-clock time spent
  // in that loop -- construction + init() for every worker, nothing else.
  [[nodiscard]] double time_start_workers_once(fsp::pipeline&              pl,
                                               const fsp::importer_config& cfg,
                                               const logger::Logger&       log,
                                               const fsp::str_t&           parent_log_name,
                                               fsp::pipeline_hooks&        hooks,
                                               std::size_t                 num_workers)
  {
    std::vector<std::unique_ptr<fsp::pipeline_worker>> workers;
    workers.reserve(num_workers);
    const auto t0 = clock_t::now();
    for (std::size_t i = 0; i < num_workers; ++i)
    {
      auto w = std::make_unique<fsp::pipeline_worker>(pl, cfg, log, parent_log_name, hooks);
      if (auto res = w->init(); ! res)
      {
        fmt::print(stderr, "pipeline_worker {} init failed: {}\n", i, res.error().to_string());
        std::exit(1); // NOLINT(concurrency-mt-unsafe) -- single-threaded benchmark driver
      }
      workers.push_back(std::move(w));
    }
    const auto t1 = clock_t::now();
    // workers (and, with them, every doc_cutter's XercesC SAX2XMLReader) are torn down here,
    // OUTSIDE the timed window -- this benchmark measures construction cost only.
    return duration_ms(t1 - t0).count();
  }

  // Same per-worker cost, but for the VALIDATING doc_cutter setup path (setup_parser_with_
  // validation(): loads+locks an XMLGrammarPoolImpl against a real XSD, in addition to the plain
  // SAX2XMLReader construction time_start_workers_once() above already measures) -- doc_cutter
  // is public API, constructed directly here (bypassing pipeline_worker) with a ds_dscr that
  // actually has a grammar loaded, since pipeline's own ds_dscr_ has no public mutator (see this
  // file's own header comment). cfg.cut_with_validation is forced to true so doc_cutter::init()'s
  // own condition (ds_dscr_.has_grammar() && cfg_.cut_with_validation.value_or(...)) always takes
  // the validating branch, matching what a real run does from 2 documents up (see importer_config.hpp).
  [[nodiscard]] double time_validating_cutters_once(const fsp::importer_config& cfg,
                                                     const logger::Logger&       log,
                                                     const fsp::doc_set_dscr&    ds_dscr,
                                                     fsp::segment_pool&          pool,
                                                     std::size_t                 num_workers)
  {
    std::vector<std::unique_ptr<fsp::doc_cutter>> cutters;
    cutters.reserve(num_workers);
    const auto t0 = clock_t::now();
    for (std::size_t i = 0; i < num_workers; ++i)
    {
      auto c = std::make_unique<fsp::doc_cutter>(cfg, log, pool, ds_dscr);
      if (auto res = c->init(); ! res)
      {
        fmt::print(stderr, "doc_cutter {} init failed: {}\n", i, res.error().to_string());
        std::exit(1); // NOLINT(concurrency-mt-unsafe) -- single-threaded benchmark driver
      }
      cutters.push_back(std::move(c));
    }
    const auto t1 = clock_t::now();
    return duration_ms(t1 - t0).count();
  }

  struct stats
  {
    double min_ms;  // NOLINT(misc-non-private-member-variables-in-classes)
    double max_ms;  // NOLINT(misc-non-private-member-variables-in-classes)
    double mean_ms; // NOLINT(misc-non-private-member-variables-in-classes)
  };

  [[nodiscard]] stats summarize(const std::vector<double>& samples)
  {
    const auto [lo, hi] = std::minmax_element(samples.begin(), samples.end());
    const double sum    = std::accumulate(samples.begin(), samples.end(), 0.0);
    return {.min_ms = *lo, .max_ms = *hi, .mean_ms = sum / static_cast<double>(samples.size())};
  }
} // namespace

int main()
{
  // Must be constructed before, and destructed after, any XercesC use (SAX2XMLReader/
  // XMLGrammarPoolImpl construction included) -- normally owned by fsp::importer::xerces_life_
  // (see importer.hpp), which this benchmark deliberately bypasses to reach pipeline_worker
  // directly.
  const fsp::xerces_mgr xerces_life;

  static constexpr std::array<std::size_t, 6> thread_counts{1, 2, 4, 8, 16, 20};
  static constexpr int                         iterations_per_count = 5;

  logger::logger_config log_cfg;
  log_cfg.app_name      = "bench_pipeline_startup";
  log_cfg.console_level = logger::level::off; // keep stdout clean for the results table
  log_cfg.file_level    = logger::level::off;
  auto log              = logger::Logger::create_or_exit(log_cfg);

  fsp::importer_config cfg{.targets = fsp::proc_data_of<^^fsp::work>(), .num_of_workers = 0, .log_config = log_cfg, .program_name = "bench"};

  fsp::no_op_hooks hooks;

  fmt::print("Benchmark 1: fsp::pipeline_worker construction + init() cost (no docs processed, no XSD grammar loaded)\n");
  fmt::print("Iterations per thread count: {}\n\n", iterations_per_count);
  fmt::print("{:>7}  {:>12}  {:>12}  {:>12}\n", "threads", "min (ms)", "max (ms)", "mean (ms)");

  for (const auto num_workers : thread_counts)
  {
    std::vector<double> samples;
    samples.reserve(iterations_per_count);
    for (int iter = 0; iter < iterations_per_count; ++iter)
    {
      // Fresh pipeline for every iteration -- avoids any state leaking between iterations (each
      // pipeline owns its own segment_pool/doc_set_dscr/queues).
      fsp::pipeline pl(cfg, *log, "bench");
      samples.push_back(time_start_workers_once(pl, cfg, *log, "bench", hooks, num_workers));
    }
    const auto s = summarize(samples);
    fmt::print("{:>7}  {:>12.3f}  {:>12.3f}  {:>12.3f}\n", num_workers, s.min_ms, s.max_ms, s.mean_ms);
  }

  // --- Benchmark 2: the VALIDATING doc_cutter setup path (real XSD, loaded+locked into a fresh
  // XMLGrammarPoolImpl per worker) -- see time_validating_cutters_once()'s own doc comment above
  // for why this needs a standalone doc_cutter/doc_set_dscr rather than pipeline_worker/pipeline.
  static constexpr auto* xsd_path = "xsd/EPC115-06_2025_V1.0_pacs.008.001.08.xsd";
  fsp::importer_config   validating_cfg      = cfg;
  validating_cfg.cut_with_validation         = true;

  fmt::print("\nBenchmark 2: fsp::doc_cutter construction + init() cost, VALIDATING (grammar: {})\n", xsd_path);
  fmt::print("Iterations per thread count: {}\n\n", iterations_per_count);
  fmt::print("{:>7}  {:>12}  {:>12}  {:>12}\n", "threads", "min (ms)", "max (ms)", "mean (ms)");

  for (const auto num_workers : thread_counts)
  {
    std::vector<double> samples;
    samples.reserve(iterations_per_count);
    for (int iter = 0; iter < iterations_per_count; ++iter)
    {
      fsp::doc_set_dscr ds_dscr(*log);
      if (! ds_dscr.set_grammar(xsd_path))
      {
        fmt::print(stderr, "Failed to load XSD grammar '{}' -- is the working directory fsp's repo root?\n", xsd_path);
        return 1;
      }
      fsp::segment_pool pool(*log, 1024UL * 1024UL * 8UL, cfg.pool_shard_count); // NOLINT(readability-magic-numbers) -- same sizing as pipeline's own ctor
      samples.push_back(time_validating_cutters_once(validating_cfg, *log, ds_dscr, pool, num_workers));
    }
    const auto s = summarize(samples);
    fmt::print("{:>7}  {:>12.3f}  {:>12.3f}  {:>12.3f}\n", num_workers, s.min_ms, s.max_ms, s.mean_ms);
  }

  return 0;
}

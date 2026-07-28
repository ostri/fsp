#include "parsing_util.hpp"
#include "pipeline_hooks.hpp"
#include "process_docs.hpp"
#include "xml_attr.hpp"
#include <chrono>
#include <fmt/format.h>
#include <iostream>
#include <magic_enum.hpp>
#include <spdlog/common.h>
#include <string>
#include <vector>
#include <filesystem>
namespace
{
  static constexpr auto fetch_ns()
  {
    // clang-format off
  return std::to_array<fsp::ns>({
    {.prefix = "",   .uri = "urn:iso:std:iso:20022:tech:xsd:pacs.008.001.08"}, // default namespace
    {.prefix = "x",  .uri = "urn:iso:std:iso:20022:tech:xsd:pacs.008.001.08"}, // default namespace
    {.prefix = "xy", .uri = "krneki"},      // explicitly defined namespace and prefix
  });
    // clang-format on
  }
  static constexpr auto fetch_targets()
  {
    // clang-format off
  return std::to_array<fsp::raw_attr>({
    {.name="header",      .path="/x:Document/FIToFICstmrCdtTrf/x:GrpHdr"},
    {.name="transaction", .path="/Document/x:FIToFICstmrCdtTrf/x:CdtTrfTxInf"},
  });
    // clang-format on
  }
  static constexpr auto fetch_hdr()
  {
    // clang-format off
    return std::to_array<fsp::raw_attr>({
      {.name="msg_id",     .path="x:GrpHdr/MsgId"},
      {.name="amount_sum", .path="GrpHdr/TtlIntrBkSttlmAmt"},
      {.name="currency",   .path="GrpHdr/TtlIntrBkSttlmAmt/@Ccy", .is_opt=true},
      {.name="msg_ts",     .path="x:GrpHdr/CreDtTm",              .is_opt=true},
      {.name="value_date", .path="GrpHdr/IntrBkSttlmDt",          .is_opt=true},
    });
    // clang-format on
  }
  static constexpr auto fetch_txn()
  {
    // clang-format off
    return std::to_array<fsp::raw_attr>({
      {.name="txn_id",        .path="CdtTrfTxInf/PmtId/TxId"},
      {.name="debtor.iban",   .path="CdtTrfTxInf/DbtrAcct/Id/IBAN"},
      {.name="debtor.bic",    .path="CdtTrfTxInf/DbtrAgt/FinInstnId/BICFI"},
      {.name="creditor.iban", .path="CdtTrfTxInf/CdtrAcct/Id/IBAN"},
      {.name="creditor.bic",  .path="CdtTrfTxInf/CdtrAgt/FinInstnId/BICFI"},
      {.name="amount",        .path="CdtTrfTxInf/IntrBkSttlmAmt"},
      {.name="currency",      .path="CdtTrfTxInf/IntrBkSttlmAmt/@Ccy",        .is_opt=true},
      {.name="instr.agent",   .path="CdtTrfTxInf/InstgAgt/FinInstnId/*BICFI", .is_opt=true},
    });
    // clang-format on
  }
  int help(const char* prog_name)
  {
    static constexpr auto* msg = "Usage: {0} <xml_file>* [<xsd_file>] \n"
                                 "Example: {0} data.xml schema.xsd \n";
    std::cerr << fmt::format(msg, prog_name);
    return 1;
  }

  /**
   * @brief Demo pipeline_hooks: every hook logs its own name and its parameters at info level.
   *
   * One instance per worker thread (see pipeline_hooks.hpp's clone() contract), so the counters
   * below are plain (non-atomic) -- each is only ever touched by the one thread that owns it.
   * on_run_end() folds every worker clone's counters into a cumulative total.
   */
  class pacs8_cb : public fsp::pipeline_hooks_crtp<pacs8_cb>
  {
  public:
    std::size_t documents_seen = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    std::size_t segments_seen  = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    std::size_t segments_ok    = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    std::size_t segments_error = 0; // NOLINT(misc-non-private-member-variables-in-classes)

    // run_start_ is only meaningful on the ORIGINAL instance (on_run_start/on_run_end are the
    // only two hooks called on it, never on a clone). worker_start_ is per-clone, set and read
    // by the one thread that owns that clone.
    std::chrono::steady_clock::time_point run_start_;    // NOLINT(misc-non-private-member-variables-in-classes)
    std::chrono::steady_clock::time_point worker_start_; // NOLINT(misc-non-private-member-variables-in-classes)

    void on_run_start(const fsp::doc_set_dscr& ds_dscr, const fsp::fsp_logger& log) override
    {
      run_start_ = std::chrono::steady_clock::now();
      log.info(fmt::format("[pacs8_cb] {:12}: ds_dscr.size()={}", "on_run_start", ds_dscr.size()));
    }

    void on_run_end(const fsp::doc_set_counter&           counters,
                    const fsp::doc_set_dscr&              ds_dscr,
                    std::span<const fsp::pipeline_hooks*> worker_clones,
                    const fsp::fsp_logger&                log) override
    {
      const auto elapsed_sec =
        std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - run_start_).count();
      log.info(fmt::format("[pacs8_cb] {:12}: counters.total_docs()={} ds_dscr.size()={} worker_clones.size()={} elapsed={:.3f} sec",
                           "on_run_end",
                           counters.total_docs(),
                           ds_dscr.size(),
                           worker_clones.size(),
                           elapsed_sec));

      std::size_t total_docs = documents_seen;
      std::size_t total_segs = segments_seen;
      std::size_t total_ok   = segments_ok;
      std::size_t total_err  = segments_error;
      for (const auto* w : worker_clones)
      {
        // Safe: every element of worker_clones was made by cloning THIS SAME pacs8_cb instance
        // (see pipeline_hooks_crtp<pacs8_cb>::clone()), so it's always actually a pacs8_cb.
        const auto* clone = static_cast<const pacs8_cb*>(w); // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
        total_docs += clone->documents_seen;
        total_segs += clone->segments_seen;
        total_ok += clone->segments_ok;
        total_err += clone->segments_error;
      }
      log.info(fmt::format("[pacs8_cb] {:12}: CUMULATIVE documents={} segments={} (ok={} error={}) total processing time={:.3f} sec",
                           "on_run_end",
                           total_docs,
                           total_segs,
                           total_ok,
                           total_err,
                           elapsed_sec));
    }

    void on_wrk_start(int worker_id, std::string_view thread_name, const fsp::fsp_logger& log) override
    {
      worker_start_ = std::chrono::steady_clock::now();
      log.info(fmt::format("[pacs8_cb] {:12}: worker_id={} thread_name='{}'", "on_wrk_start", worker_id, thread_name));
    }

    void on_wrk_end(int worker_id, std::string_view thread_name, const fsp::fsp_logger& log) override
    {
      const auto elapsed_sec =
        std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - worker_start_).count();
      log.info(fmt::format(
        "[pacs8_cb] {:12}: worker_id={} thread_name='{}' (documents_seen={} segments_seen={} ok={} error={}) thread runtime={:.3f} sec",
        "on_wrk_end",
        worker_id,
        thread_name,
        documents_seen,
        segments_seen,
        segments_ok,
        segments_error,
        elapsed_sec));
    }

    void on_doc_open(std::size_t doc_ndx, const fsp::doc_dscr& dscr, const fsp::fsp_logger& log) override
    {
      ++documents_seen;
      log.info(fmt::format("[pacs8_cb] {:12}: doc_ndx={} path='{}'", "on_doc_open", doc_ndx, dscr.path()));
    }

    void on_doc_close(std::size_t doc_ndx, fsp::doc_status status, const fsp::doc_dscr& dscr, const fsp::fsp_logger& log) override
    {
      log.info(fmt::format(
        "[pacs8_cb] {:12}: doc_ndx={} status={} path='{}'", "on_doc_close", doc_ndx, magic_enum::enum_name(status), dscr.path()));
    }

    bool on_seg_proc(std::size_t               seg_id,
                     std::size_t               doc_ndx,
                     const fsp::result_values& values,
                     bool                      is_first,
                     bool                      is_last,
                     const fsp::fsp_logger&    log) override
    {
      ++segments_seen;
      // Artificial rule for this demo: every ODD seg_id is a semantic error, every EVEN is ok.
      const bool ok = (seg_id % 2 == 0);
      if (ok) ++segments_ok;
      else ++segments_error;
      log.info(fmt::format("[pacs8_cb] {:12}: seg_id={} doc_ndx={} is_first={} is_last={} ok={}\nvalues:\n{}",
                           "on_seg_proc",
                           seg_id,
                           doc_ndx,
                           is_first,
                           is_last,
                           ok,
                           values.dump(2)));
      return ok;
    }
  };
}; // namespace
namespace fs = std::filesystem;
int main(int argc, const char* argv[])
{
  using str_t = std::string;
  if (argc == 1) return help(*argv);
  std::vector<std::string> args(argv, argv + argc); // NOLINT (cppcoreguidelines-pro-bounds-pointer-arithmetic)
  args.erase(args.begin());
  try
  {
    std::string        xsd_file;
    std::vector<str_t> files;
    files.reserve(args.size());
    for (const auto& file : args)
    {
      auto fn = fs::absolute(file).lexically_normal().string();
      if (fn.ends_with(".xsd")) xsd_file = fn;
      else files.push_back(fn);
    };

    constexpr auto        ns          = fetch_ns();
    constexpr auto        targets_raw = fetch_targets();
    constexpr auto        xpath_hdr   = fetch_hdr();
    constexpr auto        xpath_txn   = fetch_txn();
    static constexpr auto targets     = fsp::build(targets_raw, ns);
    static constexpr auto hdr         = fsp::build(xpath_hdr, ns);
    static constexpr auto txn         = fsp::build(xpath_txn, ns);
    static const auto     all         = fsp::proc_data{.targets = targets, .xpaths = {hdr, txn}};
    {
      static_assert(targets.size() == targets_raw.size(), "split xpaths are not ok.");
      static_assert(txn.size() == xpath_txn.size(), "split xpaths are not ok.");
    }
    assert(all.targets.size() == all.xpaths.size());
    //  Configure logging -- see fsp::load_logger_config() for the LOG_CONFIG env var /
    //  log_<program>_debug.log / _release.log lookup chain.
    auto log_cfg = fsp::load_logger_config(fs::path(argv[0]).filename().string()); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)

    const auto no_of_cores = 16U; // number of paralell worker threads

    auto cfg = fsp::processor_config{//
                                     .targets    = all,
                                     .num_docs   = no_of_cores,
                                     .log_config = log_cfg};

    auto     p = fsp::process_docs(cfg, "pacs8-hook");
    pacs8_cb hooks;
    auto     res = p.process_files(files, xsd_file, hooks);
    if (! res)
    {
      std::cerr << "Processing failed: " << res.error().to_string() << "\n";
      return 1;
    }
    assert(files.size() == res->total_docs());
    assert(p.get_results().size() + p.get_errors().size() == res->total_segments());

    std::cout << fmt::format("\n=== Document Statistics ===\n"
                             "  Total documents:              {:5}\n"
                             "  Total segments processed:     {:5}\n"
                             "    ok:                         {:5}\n"
                             "    error:                      {:5}\n"
                             "\n"
                             "  Syntactically correct docs:   {:5}\n"
                             "  Syntactically incorrect docs: {:5}\n"
                             "  Semantically correct docs:    {:5}\n"
                             "  Semantically incorrect docs:  {:5}\n",
                             res->total_docs(),
                             res->total_segments(),
                             res->total_segments_ok(),
                             res->total_segments_error(),
                             res->syntactically_correct_docs(),
                             res->syntactically_incorrect_docs(),
                             res->semantically_correct_docs(),
                             res->semantically_incorrect_docs());
  }
  catch (const std::exception& e)
  {
    std::cerr << "Fatal error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
/// ---

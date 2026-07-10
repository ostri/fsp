#include "parsing_util.hpp"
#include "xml_attr.hpp"
#include "xml_processor.hpp"
#include <fmt/format.h>
#include <iostream>
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
      static_assert(targets.min(0) == "Document", "Should be document");
      static_assert(targets.max(0) == "Document", "Should be document");
      static_assert(targets.min(1) == "FIToFICstmrCdtTrf", "Should be FIToFICstmrCdtTrf");
      static_assert(targets.max(1) == "FIToFICstmrCdtTrf", "Should be FIToFICstmrCdtTrf");
      static_assert(targets.min(2) == "CdtTrfTxInf", "Should be CdtTrfTxInf");
      static_assert(targets.max(2) == "GrpHdr", "Should be GrpHdr");
      static_assert(txn.size() == xpath_txn.size(), "split xpaths are not ok.");
      static_assert(txn.min(0) == "CdtTrfTxInf", "Should be CdtTrfTxInf");
      static_assert(txn.max(0) == "CdtTrfTxInf", "Should be CdtTrfTxInf");
      static_assert(txn.min(2) == "FinInstnId", "Should be FinInstnId");
      static_assert(txn.max(2) == "TxId", "Should be TxId");
    }
    assert(all.targets.size() == all.xpaths.size());
    //  Configure logging
    fsp::logger_config log_cfg{.enable_console = true,
                               .enable_file    = true,
                               .log_file_path  = "xml_processor.log",
                               .log_level      = spdlog::level::trace, // spdlog::level::info;
                               .logger_name    = "fsp"};

    const auto no_of_workers = 2U; // number of paralell workers

    auto cfg = fsp::processor_config{//
                                     .targets              = all,
                                     .num_workers          = no_of_workers,
                                     .validate_against_xsd = ! xsd_file.empty(),
                                     .log_config           = log_cfg};

    fsp::xml_processor proc(cfg);
    auto               res = proc.process_files(files, xsd_file, 4); // 4 parallel workers
    if (! res)
    {
      std::cerr << "Processing failed: " << res.error().to_string() << "\n";
      return 1;
    }
    // Get aggregated results
    auto results = proc.get_results();
    auto errors  = proc.get_errors();

    std::cout << "\n=== Processing Results ===\n";
    std::cout << "Total files processed: " << files.size() << "\n";
    std::cout << "Successful segments:   " << results.size() << "\n";
    std::cout << "Errors:                " << errors.size() << "\n";

    if (! errors.empty())
    {
      std::cout << "\n--- Errors ---\n";
      for (const auto& e : errors) { std::cout << "  " << e.seg_id() << "\n"; }
    }
  }
  catch (const std::exception& e)
  {
    std::cerr << "Fatal error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
/// ---

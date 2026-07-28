#include "parsing_util.hpp"
#include "process_docs.hpp"
#include "xml_attr.hpp"
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
  std::vector<str_t> args(argv, argv + argc); // NOLINT (cppcoreguidelines-pro-bounds-pointer-arithmetic)
  args.erase(args.begin());
  try
  {
    str_t              xsd_file;
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

    auto p   = fsp::process_docs(cfg, "pacs8");
    auto res = p.process_files(files, xsd_file);
    if (! res)
    {
      std::cerr << "Processing failed: " << res.error().to_string() << "\n";
      return 1;
    }
    assert(files.size() == res->total_docs());
        assert(p.get_results().size() + p.get_errors().size() == res->total_segments());

    std::cout << fmt::format(
      "\n=== Document Statistics ===\n"
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

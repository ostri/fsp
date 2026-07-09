#include "parsing_util.hpp"
#include "xml_attr.hpp"
#include "xml_processor.hpp"
#include <fmt/format.h>
#include <iostream>
#include <spdlog/common.h>
#include <string>
#include <vector>
int main(int argc, const char* argv[])
{
  std::vector<std::string> args(argv, argv + argc); // NOLINT (cppcoreguidelines-pro-bounds-pointer-arithmetic)
  if (argc != 3 && argc != 2)
  {
    static constexpr auto* msg = "Usage: {0} <xml_file> [<xsd_file>] \n"
                                 "Example: {0} data.xml schema.xsd \n";
    std::cerr << fmt::format(msg, args[0]);
    return 1;
  }

  try
  {
    const std::string xml_file(argv[1]);                  // NOLINT
    const std::string xsd_file(argc == 3 ? argv[2] : ""); // NOLINT

    // clang-format off
    static constexpr auto ns = std::to_array<fsp::ns>({
      {.prefix = "",   .uri = "urn:iso:std:iso:20022:tech:xsd:pacs.008.001.08"}, // default namespace
      {.prefix = "x",  .uri = "urn:iso:std:iso:20022:tech:xsd:pacs.008.001.08"}, // default namespace
      {.prefix = "xy", .uri = "krneki"},      // explicitly defined namespace and prefix
    });
    // --- targets --------------------------------------------------------------
    static constexpr auto targets_raw = std::to_array<fsp::raw_attr>({
      {.name="header",      .path="/x:Document/FIToFICstmrCdtTrf/x:GrpHdr"},
      {.name="transaction", .path="/Document/x:FIToFICstmrCdtTrf/x:CdtTrfTxInf"},
    });
    // --- attributes in header -------------------------------------------------
    static constexpr auto xpath_hdr = std::to_array<fsp::raw_attr>({
      {.name="msg_id",     .path="x:GrpHdr/MsgId"},
      {.name="amount_sum", .path="GrpHdr/TtlIntrBkSttlmAmt"},
      {.name="currency",   .path="GrpHdr/TtlIntrBkSttlmAmt/@Ccy", .is_opt=true},
      {.name="msg_ts",     .path="x:GrpHdr/CreDtTm",              .is_opt=true},
      {.name="value_date", .path="GrpHdr/IntrBkSttlmDt",          .is_opt=true},
    });
    // --- attributes in transaction --------------------------------------------
    static constexpr auto xpath_txn = std::to_array<fsp::raw_attr>({
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
    static constexpr auto targets = fsp::build(targets_raw, ns);
    static_assert(targets.size() == targets_raw.size(), "split xpaths are not ok.");
    static_assert(targets.min(0) == "Document", "Should be document");
    static_assert(targets.max(0) == "Document", "Should be document");
    static_assert(targets.min(1) == "FIToFICstmrCdtTrf", "Should be FIToFICstmrCdtTrf");
    static_assert(targets.max(1) == "FIToFICstmrCdtTrf", "Should be FIToFICstmrCdtTrf");
    static_assert(targets.min(2) == "CdtTrfTxInf", "Should be CdtTrfTxInf");
    static_assert(targets.max(2) == "GrpHdr", "Should be GrpHdr");
    static constexpr auto hdr = fsp::build(xpath_hdr, ns);
    static constexpr auto txn = fsp::build(xpath_txn, ns);
    static_assert(txn.size() == xpath_txn.size(), "split xpaths are not ok.");
    static_assert(txn.min(0) == "CdtTrfTxInf", "Should be CdtTrfTxInf");
    static_assert(txn.max(0) == "CdtTrfTxInf", "Should be CdtTrfTxInf");
    static_assert(txn.min(2) == "FinInstnId", "Should be FinInstnId");
    static_assert(txn.max(2) == "TxId", "Should be TxId");
    static const auto all = fsp::proc_data{.targets = targets, .xpaths = {hdr, txn}};
    assert(all.targets.size() == all.xpaths.size());
    //  Configure logging
    fsp::logger_config log_cfg{.enable_console = true,
                               .enable_file    = true,
                               .log_file_path  = "xml_processor.log",
                               .log_level      = spdlog::level::trace, // spdlog::level::info;
                               .logger_name    = "fsp"};

    const auto no_of_workers = 2U;                                   // number of paralell workers
    auto       result        = fsp::xml_processor::process_xml_file( //
      xml_file,                                                      // path to the xml file
      xsd_file,                                                      // path to the xsd file that xml file must comply with
      all,                                                           // array of xpaths that define split points of the xml tree
      no_of_workers,                                                 // number of workers that process the xml file in parallel (0=all)
      log_cfg                                                        // configuration of logging
    );

    if (! result)
    {
      std::cerr << "Processing failed: " << result.error().to_string() << "\n";
      return 1;
    }
    auto& [results, errors] = *result;

    std::cout << "\n=== Processing Results ===\n";
    std::cout << "Processed segments: " << results.size() << "\n";
    std::cout << "Errors: " << errors.size() << "\n";
  }
  catch (const std::exception& e)
  {
    std::cerr << "Fatal error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}

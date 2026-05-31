#include <iostream>
#include <string>
#include <vector>
#include "xml_processor.hpp"
#include <fmt/format.h>
#include "parsing_util.hpp"


int main(int argc, char* argv[])
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
    const std::string xml_file = argv[1];                  // NOLINT
    const std::string xsd_file = argc == 3 ? argv[2] : ""; // NOLINT

    std::vector<std::string> xpath_strings;

    xpath_strings.emplace_back("/Document/FIToFICstmrCdtTrf/GrpHdr");      // header
    xpath_strings.emplace_back("/Document/FIToFICstmrCdtTrf/CdtTrfTxInf"); // transaction

    // clang-format off
  static constexpr auto NS = std::to_array<fsp::ns>({
    {.prefix = "",   .uri = "urn:iso:std:iso:20022:tech:xsd:pacs.008.001.10"}, // default namespace
    {.prefix = "xy", .uri = "krneki"},      // explicitly defined namespace and prefix
  });
  static constexpr auto xpath_hdr = std::to_array<fsp::raw_attr>({
    {.name="txn_id",          .path="CdtTrfTxInf/PmtId/TxId"},
    {.name="debtor_.iban_",   .path="CdtTrfTxInf/DbtrAcct/Id/IBAN"},
    {.name="debtor_.bic_",    .path="CdtTrfTxInf/DbtrAgt/FinInstnId/BICFI"},
    {.name="creditor_.iban_", .path="CdtTrfTxInf/CdtrAcct/Id/IBAN"},
    {.name="creditor_.bic_",  .path="CdtTrfTxInf/CdtrAgt/FinInstnId/BICFI"},
    {.name="amount_",         .path="CdtTrfTxInf/IntrBkSttlmAmt"},
    {.name="currency_",       .path="CdtTrfTxInf/IntrBkSttlmAmt/@Ccy",        .is_opt=true},
    {.name="value_date_",     .path="CdtTrfTxInf/IntrBkSttlmDt",              .is_opt=true},
    {.name="remittance_",     .path="CdtTrfTxInf/RmtInf/Strd/RfrdDocInf/*Nb", .is_opt=true},
  });
    static const auto hdr = fsp::build(xpath_hdr, NS); // xml tree node(s)

  static constexpr auto xpath_txn = std::to_array<fsp::raw_attr>({
    {.name="txn_id",          .path="CdtTrfTxInf/PmtId/TxId"},
    {.name="debtor_.iban_",   .path="CdtTrfTxInf/DbtrAcct/Id/IBAN"},
    {.name="debtor_.bic_",    .path="CdtTrfTxInf/DbtrAgt/FinInstnId/BICFI"},
    {.name="creditor_.iban_", .path="CdtTrfTxInf/CdtrAcct/Id/IBAN"},
    {.name="creditor_.bic_",  .path="CdtTrfTxInf/CdtrAgt/FinInstnId/BICFI"},
    {.name="amount_",         .path="CdtTrfTxInf/IntrBkSttlmAmt"},
    {.name="currency_",       .path="CdtTrfTxInf/IntrBkSttlmAmt/@Ccy",        .is_opt=true},
    {.name="value_date_",     .path="CdtTrfTxInf/IntrBkSttlmDt",              .is_opt=true},
    {.name="remittance_",     .path="CdtTrfTxInf/RmtInf/Strd/RfrdDocInf/*Nb", .is_opt=true},
  });
    // clang-format on
    static const auto xtn = fsp::build(xpath_txn, NS); // xml tree node(s)
    // static_assert(xtn.size() == raw.size(), "The sizes must be equal");
    //  Configure logging
    fsp::logger_config log_cfg{.enable_console = true,
                               .enable_file    = true,
                               .log_file_path  = "xml_processor.log",
                               .log_level      = spdlog::level::info, // spdlog::level::info;
                               .logger_name    = "main_app"};
    const auto         no_of_workers = 7U;
    auto               result        = fsp::process_xml_file( //
      xml_file,                                               // path to the xml file
      xsd_file,                                               // path to the xsd file that xml file must comply with
      xpath_strings,                                          // array of xpaths that define split points of the xml tree
      no_of_workers,                                          // number of workers that process the xml file in parallel (0=all)
      log_cfg                                                 // configuration of logging
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

    // for (const auto& err : errors)
    // {
    //   std::cerr << "  ✗ Error in segment " << err.segment_id << " (XPath index " << err.xpath_index << "): " << err.error_message <<
    //   "\n";
    // }

    // for (const auto& res : results)
    // {
    //   std::cout << "  ✓ Segment " << res.segment_id << " (XPath index " << res.xpath_index << ") processed\n";
    // }
    // ///===============================================================================
    // // Example of using the processor directly
    // std::cout << "\n=== Advanced usage with custom config ===\n";

    // fsp::processor_config config;
    // for (const auto& xpath_str : xpath_strings)
    // {
    //   auto xpath = fsp::xpath_helpers::from_string(xpath_str);
    //   if (xpath) config.targets.push_back(std::move(*xpath));
    // }
    // config.num_workers            = 4;
    // config.validate_against_xsd   = true;
    // config.log_config             = log_cfg;
    // config.log_config.logger_name = "advanced_processor";

    // fsp::xml_processor processor(config);

    // auto process_result = processor.process_file(xml_file, xsd_file);
    // if (! process_result)
    // {
    //   std::cerr << "Advanced processing failed: " << process_result.error().to_string() << "\n";
    //   return 1;
    // }

    // auto advanced_results = processor.get_results();
    // auto advanced_errors  = processor.get_errors();
    // auto stats            = processor.get_stats();

    // std::cout << "\n=== Processing Statistics ===\n";
    // std::cout << fmt::format("  Total segments: {}\n", stats.total_segments);
    // std::cout << fmt::format("  Successful: {}\n", stats.successful_segments);
    // std::cout << fmt::format("  Failed: {}\n", stats.failed_segments);
    // std::cout << fmt::format("  Active workers: {}\n", stats.active_workers);
    // std::cout << fmt::format("  Processing time: {:.2f} ms\n", stats.processing_time_ms);

    // auto logger = processor.get_logger();
    // if (logger) logger->info("Application completed successfully");
  }
  catch (const std::exception& e)
  {
    std::cerr << "Fatal error: " << e.what() << "\n";
    return 1;
  }

  return 0;
}

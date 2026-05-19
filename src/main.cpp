#include <iostream>
#include <string>
#include <vector>
#include "xml_processor.hpp"
#include <fmt/format.h>


int main(int argc, char* argv[])
{
  std::vector<std::string> args(argv, argv + argc); // NOLINT (cppcoreguidelines-pro-bounds-pointer-arithmetic)
  // std::vector<std::string> args;
  // args.reserve(argc);
  // for (auto cnt = 0; cnt < argc; cnt++)
  // {
  //   args.emplace_back(argv[cnt]); // NOLINT (cppcoreguidelines-pro-bounds-pointer-arithmetic)
  //   std::cerr << fmt::format("argv[{}] = '{}'\n", cnt, args.at(cnt));
  // }
  if (argc < 4)
  {
    static constexpr auto* msg = "Usage: {0} <xml_file> <xsd_file> <xpath1> [xpath2 ...]\n"
                                 "Example: {0} data.xml schema.xsd /root/item /root/other\n";
    std::cerr << fmt::format(msg, args[0]);
    return 1;
  }

  try
  {
    const std::string& xml_file = args[1];
    const std::string& xsd_file = args[2];

    std::vector<std::string> xpath_strings;
    for (int i = 3; i < argc; ++i) { xpath_strings.push_back(args[i]); }

    // Configure logging
    fsp::logger_config log_cfg;
    log_cfg.enable_console = true;
    log_cfg.enable_file    = true;
    log_cfg.log_file_path  = "xml_processor.log";
    log_cfg.log_level      = spdlog::level::debug;
    log_cfg.logger_name    = "main_app";

    // Use the convenience function
    auto result = fsp::process_xml_file(xml_file, xsd_file, xpath_strings, 0, log_cfg);

    if (! result)
    {
      std::cerr << "Processing failed: " << result.error().to_string() << "\n";
      return 1;
    }

    auto& [results, errors] = *result;

    std::cout << "\n=== Processing Results ===\n";
    std::cout << "Processed segments: " << results.size() << "\n";
    std::cout << "Errors: " << errors.size() << "\n";

    for (const auto& err : errors)
    {
      std::cerr << "  ✗ Error in segment " << err.segment_id << " (XPath index " << err.xpath_index << "): " << err.error_message << "\n";
    }

    for (const auto& res : results)
    {
      std::cout << "  ✓ Segment " << res.segment_id << " (XPath index " << res.xpath_index << ") processed\n";
    }
    ///===============================================================================
    // Example of using the processor directly
    std::cout << "\n=== Advanced usage with custom config ===\n";

    fsp::processor_config config;
    for (const auto& xpath_str : xpath_strings)
    {
      auto xpath = fsp::xpath_helpers::from_string(xpath_str);
      if (xpath) config.targets.push_back(std::move(*xpath));
    }
    config.num_workers            = 4;
    config.validate_against_xsd   = true;
    config.log_config             = log_cfg;
    config.log_config.logger_name = "advanced_processor";

    fsp::xml_processor processor(config);

    auto process_result = processor.process_file(xml_file, xsd_file);
    if (! process_result)
    {
      std::cerr << "Advanced processing failed: " << process_result.error().to_string() << "\n";
      return 1;
    }

    auto advanced_results = processor.get_results();
    auto advanced_errors  = processor.get_errors();
    auto stats            = processor.get_stats();

    std::cout << "\n=== Processing Statistics ===\n";
    std::cout << fmt::format("  Total segments: {}\n", stats.total_segments);
    std::cout << fmt::format("  Successful: {}\n", stats.successful_segments);
    std::cout << fmt::format("  Failed: {}\n", stats.failed_segments);
    std::cout << fmt::format("  Active workers: {}\n", stats.active_workers);
    std::cout << fmt::format("  Processing time: {:.2f} ms\n", stats.processing_time_ms);

    auto logger = processor.get_logger();
    if (logger) logger->info("Application completed successfully");
  }
  catch (const std::exception& e)
  {
    std::cerr << "Fatal error: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
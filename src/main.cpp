#include "parsing_util.hpp"
#include "process_docs.hpp"
#include "work.hpp"
#include "xml_attr.hpp"
#include <filesystem>
#include <fmt/format.h>
#include <iostream>
#include <spdlog/common.h>
#include <string>
#include <vector>
int main(int argc, const char* argv[])
{
  std::vector<fsp::str_t> args(argv, argv + argc); // NOLINT (cppcoreguidelines-pro-bounds-pointer-arithmetic)
  if (argc != 3 && argc != 2)
  {
    static constexpr auto* msg = "Usage: {0} <xml_file> [<xsd_file>] \n"
                                 "Example: {0} data.xml schema.xsd \n";
    std::cerr << fmt::format(msg, args[0]);
    return 1;
  }

  try
  {
    const fsp::str_t xml_file(argv[1]);                  // NOLINT
    const fsp::str_t xsd_file(argc == 3 ? argv[2] : ""); // NOLINT

    // Every class in namespace `fsp::work` deriving from fsp::seg_schema (see
    // work.hpp) is one segment cut point; its own [[= "name=path"]] annotation
    // is the target entry, its annotated fields are the xpaths to extract.
    // fsp::proc_data_of() (see reflection.hpp) walks the namespace via C++26
    // reflection and builds the same fsp::proc_data this file used to build by
    // hand from targets_raw/xpath_hdr/xpath_txn.
    static const auto all = fsp::proc_data_of<^^fsp::work>();

    assert(all.targets.size() == all.xpaths.size());
    std::vector<fsp::str_t> files;
    files.push_back(xml_file);
    //  Configure logging -- see fsp::load_logger_config() for the LOG_CONFIG env var /
    //  log_<program>_debug.log / _release.log lookup chain.
    auto log_cfg =
      fsp::load_logger_config(std::filesystem::path(argv[0]).filename().string()); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)

    const auto no_of_workers = 20U;                  // number of paralell workers
    auto       cfg           = fsp::processor_config{//
                                                     .targets    = all,
                                                     .num_docs   = no_of_workers,
                                                     .log_config = log_cfg};

    auto p   = fsp::process_docs(cfg, "fsp");
    auto res = p.process_files(files, xsd_file);
    if (! res)
    {
      std::cerr << "Processing failed: " << res.error().to_string() << "\n";
      return 1;
    }
    // Get aggregated results
    const auto& results = p.get_results();
    const auto& errors  = p.get_errors();

    std::cout << "\n=== Processing Results ===\n";
    std::cout << "Total files processed: " << files.size() << "\n";
    std::cout << "Successful segments:   " << results.size() << "\n";
    std::cout << "Errors:                " << errors.size() << "\n";
    std::cout << "\n=== Document Statistics ===\n";
    std::cout << "Total segments processed:    " << res->total_segments() << "\n";
    std::cout << "Total documents:             " << res->total_docs() << "\n";
    std::cout << "Syntactically correct docs:  " << res->syntactically_correct_docs() << "\n";
    std::cout << "Syntactically incorrect docs:" << res->syntactically_incorrect_docs() << "\n";
    std::cout << "Semantically correct docs:   " << res->semantically_correct_docs() << "\n";
    std::cout << "Semantically incorrect docs: " << res->semantically_incorrect_docs() << "\n";

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

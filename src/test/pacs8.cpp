// #include "parsing_util.hpp"
#include "process_docs.hpp"
#include "common.hpp"
#include "work.hpp"
#include <fmt/format.h>
#include <iostream>
#include <spdlog/common.h>
// #include <string>
#include <vector>
#include <filesystem>
namespace
{
  int help(str_t prog_name)
  {
    static constexpr auto* msg = "Usage: {0} <xml_file>* [<xsd_file>] \n"
                                 "Example: {0} data.xml schema.xsd \n";
    fmt::print(msg, prog_name);
    return 1;
  }
}; // namespace
/////////////////////////////////////////////////////////////////////////////////////
int main(int argc, const char* argv[])
{
  fsp::param args = fsp::load_args(argc, argv);
  if (args.files.empty()) return help(args.p_name);
  try
  {
    const auto no_of_cores = 16U;                  // number of paralell worker threads
    auto       cfg         = fsp::processor_config{//
                                                   .targets        = fsp::proc_data_of<^^fsp::work>(),
                                                   .num_of_workers = no_of_cores,
                                                   .log_config     = fsp::load_logger_config(args.p_name),
                                                   .program_name   = args.p_name};
    auto       p           = fsp::process_docs(cfg);
    auto       res         = p.process_files(args.files, args.xsd_file);
    if (! res)
    {
      fmt::print("Processing failed: '{}'\n", res.error().to_string());
      return 2;
    }
    assert(args.files.size() == res->total_docs());
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

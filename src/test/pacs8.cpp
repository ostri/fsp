#include "process_docs.hpp"
#include "common.hpp"
#include "work.hpp"
#include <fmt/format.h>
#include <iostream>
#include <spdlog/common.h>
#include <vector>
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

    fmt::print("\n=== Document Statistics ===\n"
               "{:<33}{:>10}\n"
               "{:<33}{:>10}\n"
               "{:<33}{:>10}\n"
               "{:<33}{:>10}\n"
               "\n"
               "{:<33}{:>10}\n"
               "{:<33}{:>10}\n"
               "{:<33}{:>10}\n"
               "{:<33}{:>10}\n",
               "  Total documents:",
               res->total_docs(),
               "  Total segments processed:",
               res->total_segments(),
               "    ok:",
               res->total_segments_ok(),
               "    error:",
               res->total_segments_error(),
               "  Syntactically correct docs:",
               res->syntactically_correct_docs(),
               "  Syntactically incorrect docs:",
               res->syntactically_incorrect_docs(),
               "  Semantically correct docs:",
               res->semantically_correct_docs(),
               "  Semantically incorrect docs:",
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

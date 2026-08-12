#include "importer.hpp"
#include "common.hpp"
#include "exe_path.hpp"
#include "work.hpp" // IWYU pragma: keep
#include <fmt/format.h>
#include <iostream>
#include <logger/logger.hpp>
#include <logger/logger_config.hpp>
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

  // One config/log.<debug|release>.json (see add_log_config() in CMakeLists.txt) is shared by
  // every fsp program -- app_name (the log file name, and spdlog's %n) is overwritten here with
  // this program's own name after loading it, rather than baked into the file itself. The actual
  // logger::Logger is built later, inside importer's constructor (see importer.hpp's
  // detail::make_main_logger()) -- this only prepares the config it is built from. The path is
  // resolved against fsp::exe_dir() (the running binary's own directory, where add_log_config() in
  // CMakeLists.txt copies both files), not against the process's current working directory --
  // otherwise launching pacs8 from anywhere other than the build directory would silently miss the
  // config file and fall back to logger::load_logger_config()'s hardcoded defaults.
  [[nodiscard]] logger::logger_config load_program_logger_config(fsp::cstr_t program_name)
  {
    const auto config_path = fsp::exe_dir() / "config" / fmt::format("log.{}.json", logger::build_type_name());
    auto       cfg         = logger::load_logger_config(config_path.string());
    cfg.app_name           = program_name;
    return cfg;
  }
}; // namespace
/////////////////////////////////////////////////////////////////////////////////////
int main(int argc, const char* argv[])
{
  fsp::param args = fsp::load_args(argc, argv);
  if (args.files.empty()) return help(args.p_name);
  try
  {
    const auto no_of_cores = 16U;                 // number of paralell worker threads
    auto       cfg         = fsp::importer_config{//
                                                  .targets        = fsp::proc_data_of<^^fsp::work>(),
                                                  .num_of_workers = no_of_cores,
                                                  .log_config     = load_program_logger_config(args.p_name),
                                                  .program_name   = args.p_name};
    auto [p, res]          = fsp::importer::exec(cfg, args.files, args.xsd_file);
    if (! res)
    {
      fmt::print("Processing failed: '{}'\n", res.error().to_string());
      return 2;
    }
    assert(args.files.size() == res->total_docs());
    assert(p->get_results().size() + p->get_errors().size() == res->total_segments());

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

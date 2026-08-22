#include "importer.hpp"
#include "common.hpp"
#include "exe_path.hpp"
#include "work.hpp" // IWYU pragma: keep
#include <fmt/format.h>
#include <iostream>
#include <logger/logger.hpp>
#include <logger/logger_config.hpp>
#include <optional>
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

  // pipeline_hooks::get_doc_agent_id()'s own truly-unoverridden default returns 0 (fsp-core's own
  // "unresolved agent" convention -- see its own doc comment in pipeline_hooks.hpp) -- exec()
  // called with no hooks argument at all uses default_pipeline_hooks, which never overrides it, so
  // every document would be rejected before any cut/validate work. This program has no real agent
  // dictionary to resolve against and isn't demonstrating that mechanism (see "complex example" in
  // docs/importer_usage.md, and ach_hook::get_doc_agent_id() in the ach repository, for a real
  // BIC4-based resolution), so this minimal hook overrides only that one method, with a fixed,
  // non-zero id, to keep this the smallest program that actually processes its documents. Derives
  // from fsp::pipeline_hooks_crtp<Derived> directly (not fsp::typed_semantic_check<Derived,
  // ^^fsp::work>, unlike pacs8_cb.hpp) -- typed_semantic_check requires one on_type() overload per
  // fsp::work schema class (a compile-time static_assert), which this program has no use for at
  // all: it never inspects segment contents, only the summary counts exec() itself returns.
  class agent_id_hooks : public fsp::pipeline_hooks_crtp<agent_id_hooks>
  {
  protected:
    [[nodiscard]] std::optional<std::int16_t> get_doc_agent_id(fsp::cstr_t /*path*/) override { return 1; }
  };

  // log.conf (see add_log_config() in CMakeLists.txt, which picks and copies whichever ONE of
  // config/log.debug.json / config/log.release.json matches this build's own CMAKE_BUILD_TYPE)
  // is shared by every fsp program -- app_name (the log file name, and spdlog's %n) is
  // overwritten here with this program's own name after loading it, rather than baked into the
  // file itself. The actual logger::Logger is built later, inside importer's constructor (see
  // importer.hpp's detail::make_main_logger()) -- this only prepares the config it is built
  // from. The path is resolved against fsp::exe_dir() (the running binary's own directory,
  // where add_log_config() in CMakeLists.txt copies the file), not against the process's
  // current working directory -- otherwise launching pacs8 from anywhere other than the build
  // directory would silently miss the config file and fall back to
  // logger::load_logger_config()'s hardcoded defaults.
  [[nodiscard]] logger::logger_config load_program_logger_config(fsp::cstr_t program_name)
  {
    const auto config_path = fsp::exe_dir() / "log.conf";
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
    // 0 -> pipeline::plan_run() falls back to std::thread::hardware_concurrency() (see its own
    // doc comment in pipeline.cpp) -- a fixed number here would leave part of a bigger machine's
    // cores idle (max_concurrent_cutters_/num_processors are both derived from whatever this ends
    // up being, capped by hardware_concurrency() regardless), and under-provision a smaller one.
    const auto     no_of_cores = 0U;
    auto           cfg         = fsp::importer_config{//
                                                      .targets        = fsp::proc_data_of<^^fsp::work>(),
                                                      .num_of_workers = no_of_cores,
                                                      .log_config     = load_program_logger_config(args.bare_name),
                                                      .program_name   = args.bare_name};
    agent_id_hooks hooks;
    auto [p, res] = fsp::importer::exec(cfg, args.files, args.xsd_file, hooks);
    if (! res)
    {
      fmt::print("Processing failed: '{}'\n", res.error().to_string());
      return 2;
    }
    const auto& ds_dscr = p->ds_dscr();
    assert(args.files.size() == res->total_docs());
    assert(res->total_segments_ok(ds_dscr) + res->total_segments_error(ds_dscr) == res->total_segments(ds_dscr));

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
               res->total_segments(ds_dscr),
               "    ok:",
               res->total_segments_ok(ds_dscr),
               "    error:",
               res->total_segments_error(ds_dscr),
               "  Syntactically correct docs:",
               res->syntactically_correct_docs(ds_dscr),
               "  Syntactically incorrect docs:",
               res->syntactically_incorrect_docs(ds_dscr),
               "  Semantically correct docs:",
               res->semantically_correct_docs(ds_dscr),
               "  Semantically incorrect docs:",
               res->semantically_incorrect_docs(ds_dscr));
  }
  catch (const std::exception& e)
  {
    std::cerr << "Fatal error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
/// ---

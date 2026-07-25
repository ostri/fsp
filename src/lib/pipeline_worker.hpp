#pragma once

#include "doc_cutter.hpp"
#include "doc_validator.hpp"
#include "xml_worker.hpp"
#include "logger.hpp"
#include "processor_config.hpp"
#include <stop_token>
#include <memory>

namespace fsp
{
  class pipeline; // forward declaration is enough — only a reference is used here

  // A hybrid worker thread: owns one C toolkit (doc_cutter) and one P toolkit (xml_worker),
  // and decides on every iteration which role (C, V, or P) currently has useful work to do.
  class pipeline_worker
  {
  public:
    pipeline_worker(pipeline& pl, const processor_config& cfg, const fsp_logger& log, str_t parent_log_name);
    void_result init(); // sets up the doc_cutter (xerces parser + Handler); call once before operator()
    void        operator()(const std::stop_token& st, int worker_id);
  private:
    void do_cut(std::size_t doc_ndx);
    void do_validate(std::size_t doc_ndx);
  private:
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
    pipeline&                      pipeline_;
    const fsp_logger&              log_;
    str_t                          parent_log_name_;
    std::unique_ptr<doc_cutter>    cutter_;
    std::unique_ptr<xml_worker>    processor_;
    std::unique_ptr<doc_validator> validator_;
    const bool                     log_info_ = log_.active(lvl_enum::info);
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
  };
} // namespace fsp
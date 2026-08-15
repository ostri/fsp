#include "pipeline_hooks.hpp"
#include "pipeline.hpp" // full pipeline definition -- see run_data_impl()/doc_data_impl()'s own doc comments in pipeline_hooks.hpp for why these three are defined here, not inline in the header
namespace fsp
{
  run_data_root& pipeline_hooks::run_data_impl() const
  {
    assert(pipeline_ != nullptr &&
           "pipeline_hooks::run_data_impl() called before on_run_safe_start()/on_wrk_safe_start() -- see their own doc comments");
    return pipeline_->run_data();
  }

  doc_data_root& pipeline_hooks::doc_data_impl(std::size_t doc_ndx) const
  {
    assert(pipeline_ != nullptr &&
           "pipeline_hooks::doc_data_impl() called before on_run_safe_start()/on_wrk_safe_start() -- see their own doc comments");
    return pipeline_->doc_data(doc_ndx);
  }

  void pipeline_hooks::doc_data_timing_stop(std::size_t doc_ndx) { doc_data_impl(doc_ndx).timing().end(); }
} // namespace fsp
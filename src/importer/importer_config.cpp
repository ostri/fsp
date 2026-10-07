#include "importer_config.hpp"

namespace fsp
{
  std::size_t importer_config::min_seg_cache(std::size_t num_workers) const noexcept
  { return num_workers * (ok_block_flush_size + nak_block_flush_size); }
} // namespace fsp

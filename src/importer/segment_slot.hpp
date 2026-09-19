#pragma once

#include "xml_segment.hpp"
#include "segment_result.hpp"

namespace fsp
{
  /**
   * @brief One segment_pool slot: the xml_segment the cutter (C-role) produces and the
   * segment_result a worker (P-role) later fills in for the same slot index. Combining the two
   * into a single blocked_vector<segment_slot> (segment_pool's slots_) replaces the pool's former
   * pair of independently fetch()'d blocked_vector<xml_segment>/blocked_vector<segment_result> --
   * two separate atomic size_ counters that acquire_slot() relied on advancing in lockstep on
   * every call, an invariant that held only because both fetch() calls always ran in the same
   * order, never because anything made it impossible to violate. One blocked_vector::fetch() call
   * against slots_ removes the two-counter parity question entirely.
   */
  struct segment_slot
  {
    xml_segment    segment; //< filled by segment_pool::set_segment() (cutter/C-role)
    segment_result result;  //< filled by segment_pool::set_result()/result_at() (worker/P-role)
  };
} // namespace fsp

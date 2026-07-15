#pragma once

#include <cstddef>
namespace fsp
{
  struct stats_t
  {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::size_t successful_doc      = 0;   // successful documents (SAX+XSD)
    std::size_t failed_doc          = 0;   // failed documents (validaiton or sax)
    std::size_t successful_segments = 0;   // succesfull segments
    std::size_t failed_segments     = 0;   // failed segments (semantic errors)
    std::size_t active_workers      = 0;   // number of workers processing the document
    double      processing_time_ms  = 0.0; // real thread processing time
    // NOLINTEND(misc-non-private-member-variables-in-classes)
    [[nodiscard]] std::size_t total_segments() const { return successful_segments + failed_segments; }
  };

} // namespace fsp
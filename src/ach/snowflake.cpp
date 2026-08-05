#include "snowflake.hpp"
#include <stdexcept>
namespace fsp::ach
{

  // Helper: gets current timestamp in milliseconds
  [[nodiscard]] snowflake::snowflake(uint64_t node_id)
  : node_id_(node_id)
  {
    if (node_id > MAX_NODE_ID) { throw std::invalid_argument("Node ID exceeds maximum allowed limit."); }
  }
  // Generates the next unique 64-bit ID
  uint64_t snowflake::next_id()
  {
    std::lock_guard<std::mutex> lock(mtx_);

    uint64_t timestamp = current_time_ms();

    // Prevent ID collision if system clock moves backwards
    if (timestamp < last_timestamp_) { throw std::runtime_error("System clock moved backwards."); }

    if (timestamp == last_timestamp_)
    {
      // Increment sequence and wrap around using bitmask
      sequence_ = (sequence_ + 1) & SEQUENCE_MASK;

      // If sequence overflowed (reached 4096), wait for next millisecond
      if (sequence_ == 0) { timestamp = wait_for_next_millis(last_timestamp_); }
    }
    else
    {
      // Reset sequence for the new millisecond
      sequence_ = 0;
    }

    last_timestamp_ = timestamp;

    // Assemble the 64-bit ID
    return ((timestamp - EPOCH) << TIMESTAMP_SHIFT) | (node_id_ << NODE_ID_SHIFT) | sequence_;
  }
  uint64_t snowflake::current_time_ms() const
  {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
  }

  // Helper: spins until the next millisecond is reached
  [[nodiscard]] uint64_t snowflake::wait_for_next_millis(uint64_t last_ts) const
  {
    uint64_t ts = current_time_ms();
    while (ts <= last_ts) { ts = current_time_ms(); }
    return ts;
  }

}; // namespace fsp::ach
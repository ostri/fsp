#include <mutex>
#include <cstdint>

namespace fsp::ach
{
  class snowflake
  {
  public:
    explicit snowflake(uint64_t node_id);
    uint64_t next_id();
  private:                                                               // helper functions
    [[nodiscard]] uint64_t current_time_ms() const;                      //< Gets the current timestamp in milliseconds
    [[nodiscard]] uint64_t wait_for_next_millis(uint64_t last_ts) const; //< Spins until the next millisecond is reached
  private:                                                               // data members
    // January 1, 2024 (in milliseconds)
    static constexpr uint64_t EPOCH         = 1704067200000ULL; //< Custom epoch for our Snowflake IDs
    static constexpr uint64_t NODE_ID_BITS  = 10;               //< Number of bits allocated for the node ID
    static constexpr uint64_t SEQUENCE_BITS = 12;               //< Number of bits allocated for the sequence number

    static constexpr uint64_t MAX_NODE_ID     = (1ULL << NODE_ID_BITS) - 1;   //< Maximum node ID value (1023 for 10 bits)
    static constexpr uint64_t SEQUENCE_MASK   = (1ULL << SEQUENCE_BITS) - 1;  //< Mask to wrap sequence number (4095 for 12 bits)
    static constexpr uint64_t NODE_ID_SHIFT   = SEQUENCE_BITS;                //< Shift for node ID in the final 64-bit ID
    static constexpr uint64_t TIMESTAMP_SHIFT = SEQUENCE_BITS + NODE_ID_BITS; //< Shift for timestamp in the final 64-bit ID

    uint64_t   node_id_;           //< Node ID for this instance (0-1023)
    uint64_t   sequence_{0};       //< Sequence number for IDs generated in the same millisecond (0-4095)
    uint64_t   last_timestamp_{0}; //< Last timestamp when an ID was generated (in milliseconds)
    std::mutex mtx_;               //< Mutex to ensure thread-safe ID generation
  };

}; // namespace fsp::ach
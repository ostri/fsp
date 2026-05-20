#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>

#include "xml_segment.hpp"

namespace fsp
{
  class segment_queue
  {
  public:
    void push(xml_segment&& s);

    // Blokira dokler ni elementa ali finished.
    // Vrne false ko je finished in vrsta prazna.
    bool pop(xml_segment& s);

    void set_finished();

    [[nodiscard]] bool is_finished() const noexcept { return finished.load(); }
    [[nodiscard]] std::size_t size() const
    {
      std::lock_guard lock(mtx);
      return queue.size();
    }

  private:
    std::queue<xml_segment> queue;
    mutable std::mutex      mtx;
    std::condition_variable cv;
    std::atomic<bool>       finished{false};
  };
} // namespace fsp

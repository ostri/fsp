#pragma once

#include <condition_variable>
#include <mutex>
#include <queue>
#include "lib/xml_segment.hpp"
namespace fsp
{
  class segment_queue
  {
    std::queue<xml_segment> queue;
    std::mutex              mtx;
    std::condition_variable cv;
    std::atomic<bool>       finished{false};
  public:
    void push(xml_segment&& s)
    {
      {
        std::lock_guard lock(mtx);
        queue.push(std::move(s));
      }
      cv.notify_one();
    }

    bool pop(xml_segment& s)
    {
      std::unique_lock lock(mtx);
      cv.wait(lock, [this] { return ! queue.empty() || finished; });
      if (queue.empty() && finished) return false;
      s = std::move(queue.front());
      queue.pop();
      return true;
    }

    void set_finished()
    {
      finished = true;
      cv.notify_all();
    }
  };
}; // namespace fsp
#pragma once

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
    bool pop(xml_segment& s);
    void set_finished();
  private:
    std::queue<xml_segment> queue;
    std::mutex              mtx;
    std::condition_variable cv;
    std::atomic<bool>       finished{false};
  };
}; // namespace fsp
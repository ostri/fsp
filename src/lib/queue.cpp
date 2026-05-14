#include "queue.hpp"
namespace fsp
{

  void segment_queue::push(xml_segment&& s)
  {
    {
      std::lock_guard lock(mtx);
      queue.push(std::move(s));
    }
    cv.notify_one();
  }

  bool segment_queue::pop(xml_segment& s)
  {
    std::unique_lock lock(mtx);
    cv.wait(lock, [this] { return ! queue.empty() || finished; });
    if (queue.empty() && finished) return false;
    s = std::move(queue.front());
    queue.pop();
    return true;
  }

  void segment_queue::set_finished()
  {
    finished = true;
    cv.notify_all();
  }

}; // namespace fsp
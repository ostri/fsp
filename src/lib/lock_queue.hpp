#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
namespace fsp
{
  template <class T>
  class lock_queue
  {
  public:
    void                      push(T&& s);                  //< add new element ot be processed to the queue
    void                      push(const T& s);             //< add new element ot be processed to the queue
    bool                      pop(T& s);                    //< block till available element or finished
    void                      set_finished();               //< we finished processing
    [[nodiscard]] bool        is_finished() const noexcept; //< are we finished processing?
    [[nodiscard]] std::size_t size() const;                 //< size of the waiting queue
    std::optional<T>          try_pop();
  private:
    std::queue<T>           queue_;
    mutable std::mutex      mtx_;
    std::condition_variable cv_;
    std::atomic<bool>       finished_{false};
  };
  /// --- implementation ---

  /**
   * @brief are we finisehd?
   *
   * @tparam T element
   * @return true - yes we finished, no more elements in the queue to be processed
   * @return false - no we still have elements in the queue to be processed
   */
  template <class T>
  inline bool lock_queue<T>::is_finished() const noexcept
  { return finished_.load(); }
  /**
   * @brief how many elements we have in the queue
   *
   * @tparam T element
   * @return std::size_t number of the elements in the queue
   */
  template <class T>
  inline std::size_t lock_queue<T>::size() const
  {
    std::lock_guard lock(mtx_);
    return queue_.size();
  }
  template <class T>
  inline std::optional<T> lock_queue<T>::try_pop()
  {
    std::unique_lock lock(mtx_);
    if (queue_.empty()) return std::nullopt; // queue is empty
    T value = std::move(queue_.front());
    queue_.pop();
    return value; // return element from the queue
  }

  /**
   * @brief push new element to the queue
   *
   * @tparam T element
   * @param s element to be pushed
   */
  template <class T>
  void lock_queue<T>::push(T&& s)
  {
    {
      std::lock_guard lock(mtx_);
      queue_.push(std::move(s));
    }
    cv_.notify_one();
  }
  template <class T>
  void lock_queue<T>::push(const T& s)
  {
    {
      std::lock_guard lock(mtx_);
      queue_.push(s);
    }
    cv_.notify_one();
  }
  /**
   * @brief block until element or finished
   *
   * @param s element to be returned
   * @return true - there is an element to be processed
   * @return false - end of work
   */
  template <class T>
  bool lock_queue<T>::pop(T& s)
  {
    std::unique_lock lock(mtx_);
    cv_.wait(lock, [this] { return ! queue_.empty() || finished_; });
    if (queue_.empty() && finished_) return false;
    s = std::move(queue_.front());
    queue_.pop();
    return true;
  }

  /**
   * @brief mark processing to be finished
   *
   * @tparam T element
   */
  template <class T>
  void lock_queue<T>::set_finished()
  {
    finished_ = true;
    cv_.notify_all();
  }
} // namespace fsp

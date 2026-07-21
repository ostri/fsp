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
    void                           push(T&& s);                  //< add new element ot be processed to the queue
    void                           push(const T& s);             //< add new element ot be processed to the queue
    bool                           pop(T& s);                    //< block till available element or finished
    void                           set_finished();               //< we finished processing
    [[nodiscard]] bool             is_finished() const noexcept; //< are we finished processing?
    [[nodiscard]] std::size_t      size() const;                 //< size of the waiting queue
    [[nodiscard]] std::optional<T> try_pop();                    //< non blocking pop try
    [[nodiscard]] std::size_t      size_approx() const noexcept; //< lock-free hint, no mutex; used by role-picking hot path
    [[nodiscard]] bool             drained() const noexcept;     //< finished AND empty -> permanently done, no more work will ever appear
  private:
    std::queue<T>            queue_;           //< queue to store values
    mutable std::mutex       mtx_;             //< mutex to protect the pop/push operations
    std::condition_variable  cv_;              //< conditional variable
    std::atomic<bool>        finished_{false}; //< are we finished with processing (no more new entries into queue)
    std::atomic<std::size_t> size_approx_{0};  //< maintained alongside queue_ push/pop, read without locking mtx_
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
    size_approx_.fetch_add(1, std::memory_order_relaxed);
    cv_.notify_one();
  }
  template <class T>
  void lock_queue<T>::push(const T& s)
  {
    {
      std::lock_guard lock(mtx_);
      queue_.push(s);
    }
    size_approx_.fetch_add(1, std::memory_order_relaxed);
    cv_.notify_one();
  }
  /**
   * @brief block until element or finished
   *
   * @tparam T element
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
    size_approx_.fetch_sub(1, std::memory_order_relaxed);
    return true;
  }
  /**
   * @brief non blocking queue read
   *
   * @tparam T element
   * @return std::optional<T> true - element is available
   *                          false - queue is currently empty
   */
  template <class T>
  inline std::optional<T> lock_queue<T>::try_pop()
  {
    std::unique_lock lock(mtx_);
    if (queue_.empty()) return std::nullopt; // queue is empty
    T value = std::move(queue_.front());
    queue_.pop();
    lock.unlock();
    size_approx_.fetch_sub(1, std::memory_order_relaxed);
    return value; // return element from the queue
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
  // Cheap, racy hint of queue occupancy. Never takes mtx_, so many hybrid workers
  // can poll several queues in decide_role() without contending on each other's locks.
  // A stale read only ever causes a wasted try_pop() attempt, never incorrect data access.
  template <class T>
  inline std::size_t lock_queue<T>::size_approx() const noexcept
  { return size_approx_.load(std::memory_order_relaxed); }

  // True only once set_finished() was called AND the queue is empty: i.e. this source
  // is permanently exhausted and will never produce another element. Distinct from
  // "try_pop() returned nullopt", which can also mean "temporarily empty, more coming".
  template <class T>
  inline bool lock_queue<T>::drained() const noexcept
  {
    if (! finished_.load(std::memory_order_acquire)) return false;
    std::lock_guard lock(mtx_);
    return queue_.empty();
  }
} // namespace fsp

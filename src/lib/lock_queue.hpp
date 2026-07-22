#pragma once

#include <atomic>
#include <condition_variable>
#include <expected>
#include <mutex>
#include <queue>
namespace fsp
{
  enum class queue_status : char8_t
  {
    active,   // queue is running normally
    finished, // queue is closed for new items, draining remaining items
    aborted,  // queue is forcefully stopped, discard remaining items
    empty,    // queue is active, but currently empty
  };
  template <class T>
  class lock_queue
  {
  public:
    void                           push(T&& s);      //< add new element ot be processed to the queue
    void                           push(const T& s); //< add new element ot be processed to the queue
    queue_status                   pop(T& s);        //< block till available element or finished
    void                           set_finished();   //< we finished processing
    void                           set_abort();
    queue_status                   state() const;
    [[nodiscard]] bool             is_finished() const noexcept; //< are we finished processing?
    [[nodiscard]] bool             is_aborted() const noexcept;
    [[nodiscard]] bool             is_active() const noexcept;
    [[nodiscard]] std::size_t      size() const; //< size of the waiting queue
    std::expected<T, queue_status> try_pop();
    [[nodiscard]] std::ptrdiff_t   size_approx() const noexcept; //< lock-free hint, no mutex; used by role-picking hot path
    [[nodiscard]] bool             drained() const noexcept;     //< finished AND empty -> permanently done, no more work will ever appear
  private:
    std::queue<T>               q_;              //< queue to store values
    mutable std::mutex          mtx_;            //< mutex to protect the pop/push operations
    std::condition_variable     cv_;             //< conditional variable
    std::atomic<std::ptrdiff_t> size_approx_{0}; //< maintained alongside queue_ push/pop, read without locking mtx_
    std::atomic<queue_status>   state_{queue_status::active};
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
  { return state_.load(std::memory_order_acquire) == queue_status::finished; }
  template <class T>
  inline bool lock_queue<T>::is_aborted() const noexcept
  { return state_.load(std::memory_order_acquire) == queue_status::aborted; }
  template <class T>
  inline bool lock_queue<T>::is_active() const noexcept
  { return state_.load(std::memory_order_acquire) == queue_status::active; }
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
    return q_.size();
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
      q_.push(std::move(s));
      size_approx_.fetch_add(1, std::memory_order_relaxed);
    }
    cv_.notify_one();
  }
  template <class T>
  void lock_queue<T>::push(const T& s)
  {
    {
      std::lock_guard lock(mtx_);
      q_.push(s);
      size_approx_.fetch_add(1, std::memory_order_relaxed);
    }
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
  queue_status lock_queue<T>::pop(T& s)
  {
    std::unique_lock lock(mtx_);
    cv_.wait(lock, [this] { return ! q_.empty() || state_ != queue_status::active; });
    // Immediate exit if aborted, even if queue is not empty
    if (state_ == queue_status::aborted) return queue_status::aborted;
    // Exit gracefully only if empty AND finished
    if (q_.empty() && state_ == queue_status::finished) return queue_status::finished;

    s = std::move(q_.front()); // must be move and not copy (T can be move only type)
    q_.pop();
    size_approx_.fetch_sub(1, std::memory_order_relaxed);
    return queue_status::active;
  }

  /**
   * @brief non blocking queue read
   *
   * @tparam T element
   * @return std::optional<T> true - element is available
   *                          false - queue is currently empty
   */
  template <class T>
  inline std::expected<T, queue_status> lock_queue<T>::try_pop()
  {
    std::unique_lock lock(mtx_);
    queue_status     s = state_.load(std::memory_order_relaxed);
    if (s == queue_status::aborted) return std::unexpected<queue_status>(queue_status::aborted);
    if (q_.empty())
    {
      if (s == queue_status::finished) return std::unexpected<queue_status>(queue_status::finished);
      return std::unexpected<queue_status>(queue_status::empty);
    }
    T value = std::move(q_.front());
    q_.pop();
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
    std::lock_guard lock(mtx_);
    state_ = queue_status::finished;
    cv_.notify_all();
  }
  template <class T>
  void lock_queue<T>::set_abort()
  {
    std::lock_guard lock(mtx_);
    state_ = queue_status::aborted;
    cv_.notify_all();
  }
  // Cheap, racy hint of queue occupancy. Never takes mtx_, so many hybrid workers
  // can poll several queues in decide_role() without contending on each other's locks.
  // A stale read only ever causes a wasted try_pop() attempt, never incorrect data access.
  // in rare conditions size_approx_ can be negative for short period of time
  template <class T>
  inline std::ptrdiff_t lock_queue<T>::size_approx() const noexcept
  { return size_approx_.load(std::memory_order_relaxed); }

  template <class T>
  inline queue_status lock_queue<T>::state() const
  { return state_.load(std::memory_order_relaxed); }

  // True only once set_finished() was called AND the queue is empty: i.e. this source
  // is permanently exhausted and will never produce another element. Distinct from
  // "try_pop() returned nullopt", which can also mean "temporarily empty, more coming".
  template <class T>
  inline bool lock_queue<T>::drained() const noexcept
  {
    std::lock_guard lock(mtx_);
    return is_finished() && q_.empty();
  }
} // namespace fsp

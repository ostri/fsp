#pragma once

#include <vector>
#include <array>
#include <memory>
#include <atomic>
#include <mutex>
#include <optional>
#include <utility>
#include <cstddef>
#include <stdexcept>

namespace fsp
{
  template <typename T>
  class blocked_vector
  {
  public: ///< compile time constants
    static constexpr std::size_t choose_block_size() noexcept;
    static constexpr std::size_t BlockSize = choose_block_size();
    static constexpr std::size_t BlockMask = BlockSize - 1;
    static constexpr std::size_t calc_shift() noexcept;
    static constexpr std::size_t BlockShift = calc_shift();
  public:
    explicit blocked_vector(std::size_t max_elements);
    blocked_vector(const blocked_vector&)            = delete;
    blocked_vector& operator=(const blocked_vector&) = delete;
    blocked_vector(blocked_vector&& other) noexcept;
    blocked_vector& operator=(blocked_vector&& other) noexcept;
    ~blocked_vector();
    [[nodiscard]] std::size_t                               size() const noexcept;
    [[nodiscard]] std::size_t                               capacity() const noexcept;
    [[nodiscard]] bool                                      is_full() const noexcept;
    [[nodiscard]] static constexpr std::size_t              block_size() noexcept;
    [[nodiscard]] std::optional<std::pair<std::size_t, T&>> fetch();
    template <typename... Args>
    [[nodiscard]] std::optional<std::pair<std::size_t, T&>> fetch(Args&&... args);
    T&                                                      operator[](std::size_t i) noexcept;
    const T&                                                operator[](std::size_t i) const noexcept;
    // T&                                                      at(std::size_t i);
    // const T&                                                at(std::size_t i) const;
  private: /// private methods
    T&   get_storage(std::size_t i);
    void destroy_all() noexcept;
  private:                                           ///< data
    using byte_block = std::unique_ptr<std::byte[]>; // NOLINT(hicpp-avoid-c-arrays)
    std::size_t                    max_size_;     ///< maximum number of elements (from constructor)
    std::vector<byte_block>        raw_blocks_;   ///< owns the raw memory - sized once, in the constructor,
                                                  ///< to max_blocks (see get_storage()'s own doc comment on why
                                                  ///< this vector itself is never resized again after that)
    std::vector<std::atomic<T*>>   block_ptrs_;   ///< fast pointers to the start of each block - each
                                                  ///< element is std::atomic<T*>, not a plain T*, so
                                                  ///< get_storage()'s own fast path (below, outside
                                                  ///< blocks_mutex_) can load() one without racing the slow
                                                  ///< path's store() (see that method's own doc comment)
    mutable std::mutex       blocks_mutex_;          ///< protects only block allocation
    std::atomic<std::size_t> size_{0};               ///< first free index
  };
  // Choose a power-of-two block size that fits reasonably into 4/8/16/32 KiB
  template <typename T>
  constexpr std::size_t blocked_vector<T>::choose_block_size() noexcept
  {
    constexpr std::size_t blk_4k  = 4096U;
    constexpr std::size_t blk_8k  = 8192U;
    constexpr std::size_t blk_16k = 16384U;
    constexpr std::size_t blk_32k = 32768U;
    constexpr std::array  candidates{blk_4k, blk_8k, blk_16k, blk_32k}; // NOLINT(hicpp-avoid-c-arrays)
    std::size_t           best = blk_4k;
    for (const std::size_t P : candidates)
    {
      if (P / sizeof(T) >= 1) { best = P; }
    }
    std::size_t elems = best / sizeof(T);
    std::size_t pow2  = 1;
    while (pow2 * 2 <= elems) pow2 *= 2;
    return pow2;
  }

  template <typename T>
  constexpr std::size_t blocked_vector<T>::calc_shift() noexcept
  {
    std::size_t s = 0;
    std::size_t v = BlockSize;
    while (v > 1)
    {
      v >>= 1U;
      ++s;
    }
    return s;
  }

  // Returns a reference to the storage of element i (allocates the block if necessary).
  //
  // block_ptrs_/raw_blocks_ are both sized to max_blocks ONCE, in the constructor, and never
  // resized again - concurrent fetch() calls from several pipeline_worker threads (see
  // segment_pool::acquire_slot()) can otherwise race a std::vector<T*>::resize() call here
  // against another thread's own unsynchronized read of block_ptrs_[b]/block_ptrs_.size() below
  // (confirmed directly via ThreadSanitizer: resize() can reallocate the vector's own backing
  // array out from under a concurrent read with no lock at all involved on the read side) - fixed
  // size means indexing block_ptrs_[b] itself is always safe without blocks_mutex_, no resize()
  // ever competes with it again after construction.
  //
  // What still needs synchronizing is the pointer STORED at block_ptrs_[b]: nullptr (not yet
  // allocated) until the first fetch() that reaches index b takes the slow path below and
  // allocates it. block_ptrs_ is therefore std::vector<std::atomic<T*>>, not std::vector<T*> - the
  // fast path's load(acquire) is what happens-before-pairs with the slow path's store(release), so
  // a thread that observes a non-null pointer here is also guaranteed to observe the fully
  // constructed byte block it points into (raw_blocks_[b]'s own make_unique<std::byte[]>() call,
  // sequenced-before that store release) - a plain T* load/store pair here would still leave that
  // second guarantee entirely unspecified by the standard, independent of the vector-resize race
  // above.
  template <typename T>
  inline T& blocked_vector<T>::get_storage(std::size_t i)
  {
    const std::size_t b = i >> BlockShift;
    const std::size_t e = i & BlockMask;

    // Fast path - block_ptrs_ itself is fixed-size (see this method's own doc comment), so b is
    // always a valid index once i < max_size_ (fetch()'s own precondition for calling this at
    // all) - only the pointer stored at it can still be null.
    if (T* p = block_ptrs_[b].load(std::memory_order_acquire); p != nullptr) // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
      return p[e];                                                         // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)

    // Slow path - allocate this block's own backing storage, once, the first time any thread
    // reaches index b.
    std::scoped_lock lock(blocks_mutex_);

    // Re-check under the lock: another thread may have already allocated this exact block while
    // this one was waiting on blocks_mutex_ above.
    T* p = block_ptrs_[b].load(std::memory_order_relaxed); // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
    if (p == nullptr)
    {
      raw_blocks_[b] = std::make_unique<std::byte[]>(BlockSize * sizeof(T)); // NOLINT(hicpp-avoid-c-arrays,cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      p             = reinterpret_cast<T*>(raw_blocks_[b].get());           // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      block_ptrs_[b].store(p, std::memory_order_release);                  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
    }

    return p[e]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }

  template <typename T>
  inline blocked_vector<T>::blocked_vector(std::size_t max_elements)
  : max_size_(max_elements)
  {
    if (max_elements == 0) { throw std::invalid_argument("blocked_vector: max_elements must be > 0"); }

    // Sized once, here, to their final max_blocks length - see get_storage()'s own doc comment on
    // why neither vector is ever resized again afterwards (a resize() racing a concurrent
    // unsynchronized read is exactly the data race this class exists to avoid). Each element of
    // block_ptrs_ default-constructs to nullptr (std::atomic<T*>'s own default), same "not yet
    // allocated" meaning the old reserve()+resize(..., nullptr)-on-first-use scheme relied on.
    const std::size_t max_blocks = (max_elements + BlockSize - 1) / BlockSize;
    block_ptrs_ = std::vector<std::atomic<T*>>(max_blocks);
    raw_blocks_.resize(max_blocks);
  }

  template <typename T>
  inline blocked_vector<T>::blocked_vector(blocked_vector&& other) noexcept
  : max_size_(other.max_size_)
  , raw_blocks_(std::move(other.raw_blocks_))
  , block_ptrs_(std::move(other.block_ptrs_))
  , size_(other.size_.load(std::memory_order_relaxed))
  {
    other.size_.store(0, std::memory_order_relaxed);
    other.max_size_ = 0;
  }

  // Destroys every already-constructed element (same walk ~blocked_vector() does) before this
  // instance's own storage is replaced by other's -- shared by operator=(&&) and the destructor
  // rather than duplicated between them. Single-threaded by the time either caller runs (a
  // destructor/move-assignment target must already be unreachable from any other thread), so a
  // plain load() (no concurrent store() to acquire-pair with) is enough here.
  template <typename T>
  inline void blocked_vector<T>::destroy_all() noexcept
  {
    const std::size_t s = size_.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < s; ++i)
      block_ptrs_[i >> BlockShift].load(std::memory_order_relaxed)[i & BlockMask].~T(); // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
  }

  template <typename T>
  inline blocked_vector<T>& blocked_vector<T>::operator=(blocked_vector&& other) noexcept
  {
    if (this == &other) return *this;
    destroy_all();
    max_size_  = other.max_size_;
    raw_blocks_ = std::move(other.raw_blocks_);
    block_ptrs_ = std::move(other.block_ptrs_);
    size_.store(other.size_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    other.size_.store(0, std::memory_order_relaxed);
    other.max_size_ = 0;
    return *this;
  }

  template <typename T>
  inline blocked_vector<T>::~blocked_vector() { destroy_all(); }

  // Basic queries
  template <typename T>
  inline std::size_t blocked_vector<T>::size() const noexcept
  { return size_.load(std::memory_order_acquire); }

  template <typename T>
  inline std::size_t blocked_vector<T>::capacity() const noexcept
  { return max_size_; }

  template <typename T>
  inline bool blocked_vector<T>::is_full() const noexcept
  { return size() >= max_size_; }

  template <typename T>
  [[nodiscard]] constexpr std::size_t blocked_vector<T>::block_size() noexcept
  { return BlockSize; }

  // Thread-safe fetch – constructs the element and returns (index, reference)
  // Returns std::nullopt when the vector is already full
  template <typename T>
  inline std::optional<std::pair<std::size_t, T&>> blocked_vector<T>::fetch()
  {
    const std::size_t idx = size_.fetch_add(1, std::memory_order_acq_rel);

    if (idx >= max_size_)
    {
      size_.fetch_sub(1, std::memory_order_relaxed);
      return std::nullopt;
    }

    T& ref = get_storage(idx);
    new (&ref) T();
    return {{idx, ref}};
  }

  template <typename T>
  template <typename... Args>
  inline std::optional<std::pair<std::size_t, T&>> blocked_vector<T>::fetch(Args&&... args)
  {
    const std::size_t idx = size_.fetch_add(1, std::memory_order_acq_rel);

    if (idx >= max_size_)
    {
      size_.fetch_sub(1, std::memory_order_relaxed);
      return std::nullopt;
    }

    T& ref = get_storage(idx);
    new (&ref) T(std::forward<Args>(args)...);
    return {{idx, ref}};
  }

  // operator[] and at - only valid for already fetched elements (i < size()), i.e. a block whose
  // own fetch() call has already returned (see get_storage()'s own doc comment on the
  // acquire/release pairing that guarantees this load() sees a fully constructed block).
  template <typename T>
  inline T& blocked_vector<T>::operator[](std::size_t i) noexcept
  {
    return block_ptrs_[i >> BlockShift].load(std::memory_order_acquire)[i & BlockMask]; // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }

  template <typename T>
  inline const T& blocked_vector<T>::operator[](std::size_t i) const noexcept
  {
    return block_ptrs_[i >> BlockShift].load(std::memory_order_acquire)[i & BlockMask]; // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }

  // template <typename T>
  // inline T& blocked_vector<T>::at(std::size_t i)
  // {
  //   if (i >= size()) throw std::out_of_range("blocked_vector::at");
  //   return (*this)[i];
  // }

  // template <typename T>
  // inline const T& blocked_vector<T>::at(std::size_t i) const
  // {
  //   if (i >= size()) throw std::out_of_range("blocked_vector::at");
  //   return (*this)[i];
  // }
} // namespace fsp

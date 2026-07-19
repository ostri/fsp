#pragma once
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <string>
// #include <concepts>
#include <span>
#include <expected>

namespace fsp
{

  class mmap_file
  {
  public:
    using value_type      = std::byte;
    using size_type       = size_t;
    using difference_type = std::ptrdiff_t;
    using pointer         = std::byte*;
    using const_pointer   = const std::byte*;
    using reference       = std::byte&;
    using const_reference = const std::byte&;
    using iterator        = const_pointer;
    using const_iterator  = const_pointer;
    using byte_span       = std::span<const std::byte>;

    mmap_file() = default;

    explicit mmap_file(const std::string& path);
    mmap_file(const mmap_file&)            = delete;
    mmap_file& operator=(const mmap_file&) = delete;
    mmap_file(mmap_file&& other) noexcept;
    mmap_file& operator=(mmap_file&& other) noexcept;
    ~mmap_file();
    void                                   open(const std::string& path);
    void                                   close() noexcept;
    [[nodiscard]] std::string_view         string_view() const;
    [[nodiscard]] mmap_file::const_pointer data() const noexcept;
    [[nodiscard]] size_type                size() const noexcept;
    [[nodiscard]] bool                     empty() const noexcept;
    [[nodiscard]] bool                     is_open() const noexcept;
    const_reference                        operator[](size_type pos) const;
    [[nodiscard]] const_reference          at(size_type pos) const;
    [[nodiscard]] iterator                 begin() const noexcept;
    [[nodiscard]] const_iterator           cbegin() const noexcept;
    [[nodiscard]] iterator                 end() const noexcept;
    [[nodiscard]] const_iterator           cend() const noexcept;
    [[nodiscard]] mmap_file::byte_span     span() const noexcept;
    [[nodiscard]] mmap_file::byte_span     subspan(size_type offset, size_type count) const;
    static constexpr size_type             prefetch_size = 4096;
    void                                   prefetch(size_type offset, size_type count = prefetch_size) const noexcept;
    explicit                               operator bool() const noexcept;
    [[nodiscard]] std::string_view         path() const;
  private:
    const_pointer data_ = nullptr; // address of start of the file; null if error or closed
    size_type     size_ = 0;       // size of the file
    int           fd_   = -1;      // fd of the open file or -1 if file not opened
    std::string   path_;           // path of the opened file
  };

  // Helper function for better error handling using std::expected
  std::expected<mmap_file, std::string> try_mmap_file(const std::string& path);
  inline mmap_file::iterator            mmap_file::begin() const noexcept { return data_; }
  inline mmap_file::const_iterator      mmap_file::cbegin() const noexcept { return data_; }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  inline mmap_file::iterator mmap_file::end() const noexcept { return data_ + size_; }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  inline mmap_file::const_iterator mmap_file::cend() const noexcept { return data_ + size_; }
  inline mmap_file::byte_span      mmap_file::span() const noexcept { return {data_, size_}; }
  inline mmap_file::byte_span      mmap_file::subspan(size_type offset, size_type count) const
  {
    if (offset >= size_) return {};
    auto actual_count = std::min(count, size_ - offset);
    return std::span<const std::byte>{data_ + offset, actual_count}; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }
  inline void mmap_file::prefetch(size_type offset, size_type count) const noexcept
  {
    if (data_ != nullptr && offset < size_)
    {
      auto actual_count = std::min(count, size_ - offset);
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic, cppcoreguidelines-pro-type-const-cast)
      ::madvise(const_cast<void*>(static_cast<const void*>(data_ + offset)), actual_count, MADV_WILLNEED);
    }
  }
  inline mmap_file::      operator bool() const noexcept { return is_open(); }
  inline std::string_view mmap_file::path() const { return path_; }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  inline std::string_view mmap_file::string_view() const
  {
    if (is_open()) return {reinterpret_cast<const char*>(data()), size()};
    return {};
  }
  inline mmap_file::const_pointer mmap_file::data() const noexcept { return data_; }
  inline mmap_file::size_type     mmap_file::size() const noexcept { return size_; }
  inline bool                     mmap_file::empty() const noexcept { return size_ == 0; }
  inline bool                     mmap_file::is_open() const noexcept { return fd_ != -1; }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  inline mmap_file::const_reference mmap_file::operator[](size_type pos) const { return data_[pos]; }

} // namespace fsp

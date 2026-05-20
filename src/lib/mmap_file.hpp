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
    using pointer         = const std::byte*;
    using const_pointer   = const std::byte*;
    using reference       = const std::byte&;
    using const_reference = const std::byte&;
    using iterator        = const_pointer;
    using const_iterator  = const_pointer;

    mmap_file() = default;

    explicit mmap_file(const std::string& path);
    mmap_file(const mmap_file&)            = delete;
    mmap_file& operator=(const mmap_file&) = delete;
    mmap_file(mmap_file&& other) noexcept;
    mmap_file& operator=(mmap_file&& other) noexcept;
    ~mmap_file();
    void                                     open(const std::string& path);
    void                                     close() noexcept;
    [[nodiscard]] std::string_view           view() const;
    [[nodiscard]] mmap_file::const_pointer   data() const noexcept;
    [[nodiscard]] size_type                  size() const noexcept;
    [[nodiscard]] bool                       empty() const noexcept;
    [[nodiscard]] bool                       is_open() const noexcept;
    const_reference                          operator[](size_type pos) const;
    [[nodiscard]] const_reference            at(size_type pos) const;
    [[nodiscard]] iterator                   begin() const noexcept;
    [[nodiscard]] const_iterator             cbegin() const noexcept;
    [[nodiscard]] iterator                   end() const noexcept;
    [[nodiscard]] const_iterator             cend() const noexcept;
    [[nodiscard]] std::span<const std::byte> span() const noexcept;

    explicit                       operator bool() const noexcept;
    [[nodiscard]] std::string_view path() const;
  private:
    const_pointer data_ = nullptr; // address of start of the file; null if error or closed
    size_type     size_ = 0;       // size of the file
    int           fd_   = -1;      // fd of the open file or -1 if file not opened
    std::string   path_;           // path of the opened file
  };

  // Helper function for better error handling using std::expected
  inline std::expected<mmap_file, std::string> try_mmap_file(const std::string& path)
  {
    try
    {
      return mmap_file(path);
    }
    catch (const std::exception& e)
    {
      return std::unexpected(std::string(e.what()));
    }
  }

} // namespace fsp

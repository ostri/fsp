#include "mmap_file.hpp"
#include <stdexcept>
namespace fsp
{

  mmap_file::mmap_file(const std::string& path) { open(path); }

  mmap_file::mmap_file(mmap_file&& other) noexcept
  : data_(other.data_)
  , size_(other.size_)
  , fd_(other.fd_)
  , path_(std::move(other.path_))
  {
    other.data_ = nullptr;
    other.size_ = 0;
    other.fd_   = -1;
    other.path_.clear();
  }

  mmap_file& mmap_file::operator=(mmap_file&& other) noexcept
  {
    if (this != &other)
    {
      close();
      data_       = other.data_;
      size_       = other.size_;
      fd_         = other.fd_;
      other.data_ = nullptr;
      other.size_ = 0;
      other.fd_   = -1;
      other.path_ = "";
    }
    return *this;
  }

  mmap_file::~mmap_file() { close(); }

  void mmap_file::open(const std::string& path)
  {
    close();

    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC); // NOLINT(hicpp-vararg)
    if (fd_ == -1) { throw std::runtime_error("Failed to open file: " + path); }

    struct stat st{};
    if (fstat(fd_, &st) == -1)
    {
      ::close(fd_);
      fd_ = -1;
      throw std::runtime_error("Failed to get file size: '" + path + "'.");
    }

    size_ = static_cast<size_type>(st.st_size);

    if (size_ > 0)
    {
      void* mapped = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
      if (mapped == MAP_FAILED)
      {
        ::close(fd_);
        fd_   = -1;
        size_ = 0;
        throw std::runtime_error("Failed to mmap file: " + path);
      }
      data_ = static_cast<pointer>(mapped);
    }
    else
    {
      data_ = nullptr;
    }
    path_ = path;
  }

  void mmap_file::close() noexcept
  {
    if (data_ != nullptr)
    {
      ::munmap(const_cast<void*>(static_cast<const void*>(data_)), size_); // NOLINT(cppcoreguidelines-pro-type-const-cast)
      data_ = nullptr;
    }
    if (fd_ != -1)
    {
      ::close(fd_);
      fd_ = -1;
    }
    size_ = 0;
    path_.clear();
  }

  [[nodiscard]] std::string_view mmap_file::view() const //
  {                                                      //
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return {reinterpret_cast<const char*>(data()), size()};
  }

  [[nodiscard]] mmap_file::const_pointer mmap_file::data() const noexcept { return data_; }

  [[nodiscard]] mmap_file::size_type mmap_file::size() const noexcept { return size_; }

  [[nodiscard]] bool mmap_file::empty() const noexcept { return size_ == 0; }

  [[nodiscard]] bool mmap_file::is_open() const noexcept { return fd_ != -1; }

  mmap_file::const_reference mmap_file::operator[](size_type pos) const
  { return data_[pos]; } // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)

  [[nodiscard]] mmap_file::const_reference mmap_file::at(size_type pos) const
  {
    if (pos >= size_) { throw std::out_of_range("mmap_file::at: index out of range"); }
    return data_[pos]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }

  [[nodiscard]] mmap_file::iterator mmap_file::begin() const noexcept { return data_; }

  [[nodiscard]] mmap_file::const_iterator mmap_file::cbegin() const noexcept { return data_; }

  [[nodiscard]] mmap_file::iterator mmap_file::end() const noexcept
  { return data_ + size_; } // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)

  [[nodiscard]] mmap_file::const_iterator mmap_file::cend() const noexcept
  { return data_ + size_; } // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)

  std::string_view mmap_file::path() const { return path_; }

  [[nodiscard]] std::span<const std::byte> mmap_file::span() const noexcept { return {data_, size_}; }

  mmap_file::operator bool() const noexcept { return is_open(); }
}; // namespace fsp
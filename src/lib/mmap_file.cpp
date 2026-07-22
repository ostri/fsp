#include "mmap_file.hpp"
#include <fmt/format.h>
#include <stdexcept>
#include <filesystem>
namespace fsp
{
  namespace fs = std::filesystem;
  mmap_file::mmap_file(cstr_t path) { open(path); }
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
  //  void mmap_file::open(const std::string& path)
  void mmap_file::open(cstr_t path)
  {
    close();
    fd_ = ::open(std::string(path.data(), path.size()).data(), O_RDONLY | O_CLOEXEC); // NOLINT(hicpp-vararg)
    if (fd_ == -1)
    {
      fs::path absolute = fs::absolute(path).lexically_normal();
      throw std::runtime_error(fmt::format("Failed to open file: '{}'", absolute.string()));
    }

    struct stat st{};
    if (fstat(fd_, &st) == -1)
    {
      ::close(fd_);
      fd_               = -1;
      fs::path absolute = fs::absolute(path).lexically_normal();
      throw std::runtime_error(fmt::format("Failed to get file size: '{}'", absolute.string()));
    }

    size_ = static_cast<size_type>(st.st_size);

    if (size_ > 0)
    {
      void* mapped = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
      if (mapped == MAP_FAILED)
      {
        ::close(fd_);
        fd_               = -1;
        size_             = 0;
        fs::path absolute = fs::absolute(path).lexically_normal();
        throw std::runtime_error(fmt::format("Failed to mmap file: '{}'", absolute.string()));
      }
      data_ = static_cast<pointer>(mapped);
      // we are going to read sequentially
      //      if (sequential) ::madvise(mapped, size_, MADV_SEQUENTIAL);
      // if (sequential) ::madvise(mapped, size_, MADV_RANDOM);
    }
    else data_ = nullptr;
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
  [[nodiscard]] mmap_file::const_reference mmap_file::at(size_type pos) const
  {
    if (pos >= size_) { throw std::out_of_range("mmap_file::at: index out of range"); }
    return data_[pos]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }
  std::expected<mmap_file, std::string> try_mmap_file(const std::string& path)
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
}; // namespace fsp
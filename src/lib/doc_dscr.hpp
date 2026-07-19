// doc_dscr.hpp
#pragma once
#include "mmap_file.hpp"
#include <string_view>

namespace fsp
{
  class doc_dscr
  {
  public:
    doc_dscr() = default;
    explicit doc_dscr(const std::string& path)
    : doc_(path)
    {
    }

    // Allow construction from existing mmap_file
    explicit doc_dscr(mmap_file&& file)
    : doc_(std::move(file))
    {
    }
    ~doc_dscr()
    {
      if (doc_.is_open()) doc_.close();
    }
    // Copy/move semantics
    doc_dscr(const doc_dscr&)            = delete;
    doc_dscr& operator=(const doc_dscr&) = delete;
    doc_dscr(doc_dscr&&)                 = default;
    doc_dscr& operator=(doc_dscr&&)      = default;

    // Opening/closing
    void open(const std::string& path) { doc_.open(path); }
    void close() noexcept { doc_.close(); }

    // Accessors
    [[nodiscard]] bool             is_open() const noexcept { return doc_.is_open(); }
    [[nodiscard]] bool             empty() const noexcept { return doc_.empty(); }
    [[nodiscard]] size_t           size() const noexcept { return doc_.size(); }
    [[nodiscard]] const std::byte* data() const noexcept { return doc_.data(); }
    [[nodiscard]] std::string_view path() const { return doc_.path(); }

    // String view access (same as mmap_file::string_view())
    [[nodiscard]] std::string_view string_view() const { return doc_.string_view(); }

    // Element access
    [[nodiscard]] std::byte operator[](size_t pos) const { return doc_[pos]; }
    [[nodiscard]] std::byte at(size_t pos) const { return doc_.at(pos); }

    // Iterator support
    [[nodiscard]] auto begin() const noexcept { return doc_.begin(); }
    [[nodiscard]] auto end() const noexcept { return doc_.end(); }
    [[nodiscard]] auto cbegin() const noexcept { return doc_.cbegin(); }
    [[nodiscard]] auto cend() const noexcept { return doc_.cend(); }

    // Span access
    [[nodiscard]] std::span<const std::byte> span() const noexcept { return doc_.span(); }
    [[nodiscard]] std::span<const std::byte> subspan(size_t offset, size_t count) const { return doc_.subspan(offset, count); }

    // Prefetch support
    void prefetch(size_t offset, size_t count = mmap_file::prefetch_size) const noexcept { doc_.prefetch(offset, count); }

    // Boolean operator - true if document is open
    [[nodiscard]] explicit operator bool() const noexcept { return doc_.is_open(); }

    // Access to underlying mmap_file (for advanced use)
    [[nodiscard]] const mmap_file& underlying() const noexcept { return doc_; }
  private:
    mmap_file doc_;
  };
}; // namespace fsp
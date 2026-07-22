// doc_dscr.hpp
#pragma once
#include "error_info.hpp"
#include "mmap_file.hpp"
#include <string_view>

namespace fsp
{
  enum doc_status : char8_t
  {
    unknown = 0,      // process of validation is going on
    validation_ok,    // validation was successful
    validation_failed // validation failed
  };
  class doc_dscr
  {
  public:
    doc_dscr() = default;
    explicit doc_dscr(const std::string& path);
    explicit doc_dscr(mmap_file&& file);
    ~doc_dscr();
    // Copy/move semantics
    doc_dscr(const doc_dscr&)                                      = delete;
    doc_dscr& operator=(const doc_dscr&)                           = delete;
    doc_dscr(doc_dscr&&)                                           = default;
    doc_dscr&                                operator=(doc_dscr&&) = default;
    void                                     close() noexcept;
    [[nodiscard]] bool                       is_open() const noexcept;
    [[nodiscard]] bool                       empty() const noexcept;
    [[nodiscard]] size_t                     size() const noexcept;
    [[nodiscard]] const std::byte*           data() const noexcept;
    [[nodiscard]] std::string_view           path() const;
    [[nodiscard]] std::string_view           string_view() const;
    [[nodiscard]] std::byte                  operator[](size_t pos) const;
    [[nodiscard]] std::byte                  at(size_t pos) const;
    [[nodiscard]] auto                       begin() const noexcept;
    [[nodiscard]] auto                       end() const noexcept;
    [[nodiscard]] auto                       cbegin() const noexcept;
    [[nodiscard]] auto                       cend() const noexcept;
    [[nodiscard]] std::span<const std::byte> span() const noexcept;
    [[nodiscard]] std::span<const std::byte> subspan(size_t offset, size_t count) const;
    void                                     prefetch(size_t offset, size_t count = mmap_file::prefetch_size) const noexcept;
    [[nodiscard]] explicit                   operator bool() const noexcept;
    [[nodiscard]] const mmap_file&           mmf() const noexcept;
    mmap_file&                               mmf() noexcept;
    [[nodiscard]] doc_status                 status() const noexcept;
    void                                     set_status(doc_status status) noexcept;
  private: //< methods
    void open(const std::string& path);
  private:
    mmap_file  doc_;             // core document functionality
    doc_status status_{unknown}; // validation status of the document
    error_info err_;             // if there is an error, here it is the error description
  };
  ///////////////////////////////////////////////////////////////////////////////////////////
  inline doc_dscr::doc_dscr(const std::string& path)
  : doc_(path)
  {
  }
  // Allow construction from existing mmap_file
  inline doc_dscr::doc_dscr(mmap_file&& file)
  : doc_(std::move(file))
  {
  }
  inline doc_dscr::~doc_dscr()
  {
    if (doc_.is_open()) doc_.close();
  }
  // Opening/closing
  inline void doc_dscr::open(const std::string& path) { doc_.open(path); }
  inline void doc_dscr::close() noexcept { doc_.close(); }
  // Accessors
  inline bool             doc_dscr::is_open() const noexcept { return doc_.is_open(); }
  inline bool             doc_dscr::empty() const noexcept { return doc_.empty(); }
  inline size_t           doc_dscr::size() const noexcept { return doc_.size(); }
  inline const std::byte* doc_dscr::data() const noexcept { return doc_.data(); }
  inline std::string_view doc_dscr::path() const { return doc_.path(); }
  // String view access (same as mmap_file::string_view())
  inline std::string_view doc_dscr::string_view() const { return doc_.string_view(); }
  // Element access
  inline std::byte doc_dscr::operator[](size_t pos) const { return doc_[pos]; }
  inline std::byte doc_dscr::at(size_t pos) const { return doc_.at(pos); }
  // Iterator support
  inline auto doc_dscr::begin() const noexcept { return doc_.begin(); }
  inline auto doc_dscr::end() const noexcept { return doc_.end(); }
  inline auto doc_dscr::cbegin() const noexcept { return doc_.cbegin(); }
  inline auto doc_dscr::cend() const noexcept { return doc_.cend(); }
  // Span access
  inline std::span<const std::byte> doc_dscr::span() const noexcept { return doc_.span(); }
  inline std::span<const std::byte> doc_dscr::subspan(size_t offset, size_t count) const { return doc_.subspan(offset, count); }
  // Prefetch support
  inline void doc_dscr::prefetch(size_t offset, size_t count) const noexcept { doc_.prefetch(offset, count); }
  // Access to underlying mmap_file (for advanced use)
  inline const mmap_file& doc_dscr::mmf() const noexcept { return doc_; }
  inline mmap_file&       doc_dscr::mmf() noexcept { return doc_; }
  inline doc_dscr::       operator bool() const noexcept { return doc_.is_open(); }
  inline doc_status       doc_dscr::status() const noexcept { return status_; }
  inline void             doc_dscr::set_status(doc_status status) noexcept { status_ = status; }
}; // namespace fsp
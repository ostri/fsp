// doc_dscr.hpp
#pragma once
#include "error_info.hpp"
#include "mmap_file.hpp"
#include <atomic>
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
    doc_dscr(const doc_dscr&)            = delete;
    doc_dscr& operator=(const doc_dscr&) = delete;
    doc_dscr(doc_dscr&& o) noexcept;
    doc_dscr&                                operator=(doc_dscr&& o) noexcept;
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
    void                            set_validation_result(doc_status result, error_info err = {}) noexcept; // SPREMENJENO iz set_status()
    [[nodiscard]] const error_info& error() const noexcept;                                                 // NOVO
  private:                                                                                                  //< methods
    void open(const std::string& path);
  private:
    mmap_file               doc_;             // core document functionality
    std::atomic<doc_status> status_{unknown}; // validation status of the document
    error_info              err_;             // if there is an error, here it is the error description
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
  inline doc_dscr::doc_dscr(doc_dscr&& o) noexcept
  : doc_(std::move(o.doc_))
  , status_(o.status_.load(std::memory_order_relaxed))
  , err_(std::move(o.err_))
  {
  }
  inline doc_dscr& doc_dscr::operator=(doc_dscr&& o) noexcept
  {
    if (this != &o)
    {
      doc_ = std::move(o.doc_);
      status_.store(o.status_.load(std::memory_order_relaxed), std::memory_order_relaxed);
      err_ = std::move(o.err_);
    }
    return *this;
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
  inline doc_status       doc_dscr::status() const noexcept { return status_.load(std::memory_order_acquire); }
  inline void             doc_dscr::set_status(doc_status status) noexcept { status_.store(status, std::memory_order_release); }
  // inline doc_status       doc_dscr::status() const noexcept { return status_.load(std::memory_order_acquire); }

  /**
   * @brief Records a validation outcome for this document (called by either the C or V worker).
   *
   * Concurrency contract: C and V run independently and may report a result for the same
   * document from different threads with no ordering between them. The rules below make that safe:
   *
   * - 'validation_failed' is sticky and authoritative: it always wins, regardless of arrival
   *   order or source (malformed XML from C, or a genuine schema violation from V) — a document
   *   that is not even well-formed can never legitimately become "valid" afterwards.
   * - 'validation_ok' is only accepted while the document is still 'unknown'; it can never
   *   downgrade an already-'validation_failed' document back to valid.
   * - err_ is plain (non-atomic) data, so only the thread that actually performs the
   *   unknown -> validation_failed transition (via exchange()) is allowed to write it. Any
   *   concurrent, redundant "also invalid" report from the other side is safely dropped after
   *   the status itself, which is enough to know the document is invalid either way.
   */
  inline void doc_dscr::set_validation_result(doc_status result, error_info err) noexcept
  {
    if (result == doc_status::validation_failed)
    {
      doc_status prev = status_.exchange(doc_status::validation_failed, std::memory_order_acq_rel);
      if (prev != doc_status::validation_failed) err_ = std::move(err); // only the first reporter owns err_
      return;
    }
    doc_status expected = doc_status::unknown;
    status_.compare_exchange_strong(expected, result, std::memory_order_release, std::memory_order_relaxed);
  }
  inline const error_info& doc_dscr::error() const noexcept { return err_; }
}; // namespace fsp
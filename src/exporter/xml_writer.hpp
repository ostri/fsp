#pragma once

/**
 * @file xml_writer.hpp
 * @brief High-performance sequential XML writer with deferred header support.
 *
 * Designed for large XML files (hundreds of MB to several GB) where the document
 * header can only be fully constructed after all content has been written.
 *
 * Key features:
 *  - Uses C stdio (fwrite) with a large buffer for maximum sequential write speed
 *  - Reserves a fixed amount of space at the beginning of the file for the header
 *  - Batches small writes to reduce system-call overhead
 *  - Periodically advises the kernel to drop already-written pages from the page
 *    cache (POSIX_FADV_DONTNEED) to keep memory pressure low
 *  - Produces valid XML by turning any unused reserved space into an XML comment
 *
 * xml_writer never throws -- every fallible operation returns fsp::e_result
 * (std::expected<void, fsp::error_info>), which the caller must check.
 *
 * Typical usage:
 *
 * @code
 *   #include "xml_writer.hpp"
 *
 *   fsp::xml_writer out(log, "output.xml"); // logs a warning and stays closed on failure
 *   if (! out.is_open()) return;            // or: fsp::xml_writer out; out.open("output.xml");
 *
 *   // Append as many fragments as needed (transactions, records, ...)
 *   for (const auto& tx : transactions) {
 *     if (auto res = out.append(tx.to_xml()); ! res) { // handle res.error() }
 *   }
 *
 *   // Optionally append closing tags before finalizing
 *   out.append("</root>\n");
 *
 *   // Build the real header only now (size must be <= reserved space)
 *   std::string header =
 *       "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
 *       "<root ...>\n";
 *
 *   if (auto res = out.finalize(header); ! res) { // handle res.error() }
 * @endcode
 *
 * The reserved header space defaults to 2048 bytes (well above the 1 KB
 * requirement). Any unused bytes after the real header are turned into a
 * well-formed XML comment so the file remains valid.
 */

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
#include <cerrno>
#include <expected>
#include <algorithm>
#include <tuple>
#include <array>

#include <fcntl.h>  // posix_fadvise
#include <unistd.h> // fileno

#include <fmt/format.h>

#include "error_info.hpp"
#include "logger.hpp"

namespace fsp
{
  using cstr_t = std::string_view;
  using str_t  = std::string;

  /// @brief Result type shared by xml_writer's non-throwing methods.
  using e_result = std::expected<void, error_info>;

  namespace detail
  {
    /// @brief Thread-safe errno -> message lookup (std::strerror is not thread-safe).
    inline str_t errno_str(int err)
    {
      static constexpr std::size_t     ERRNO_BUF_SIZE = 256;
      std::array<char, ERRNO_BUF_SIZE> buf{};
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay,hicpp-no-array-decay)
      return {::strerror_r(err, buf.data(), buf.size())};
    }

    /// @brief Builds a file_open_failed error_info carrying @p msg and @p path.
    inline error_info io_error(str_t msg, cstr_t path = "") { return {processor_error::file_open_failed, std::move(msg), path, 0}; }
  } // namespace detail

  /// @brief High-performance sequential XML writer; see xml_writer.hpp for the full description.
  class xml_writer
  {
  public:
    // -------------------------------------------------------------------------
    // Configuration constants (can be tuned)
    // -------------------------------------------------------------------------
    static constexpr std::size_t HEADER_RESERVE  = 2048;               ///< Bytes reserved at file start for the header
    static constexpr std::size_t IO_BUFFER_SIZE  = 2UL * 1024 * 1024;  ///< stdio buffer size (2 MiB)
    static constexpr std::size_t BATCH_SIZE      = 512UL * 1024;       ///< Aggregate writes up to this size
    static constexpr std::size_t ADVISE_INTERVAL = 64UL * 1024 * 1024; ///< Drop pages from cache every N bytes
    static constexpr std::size_t BATCH_SLACK     = 64UL * 1024;        ///< Extra headroom reserved on top of BATCH_SIZE

    // -------------------------------------------------------------------------
    // Construction / destruction
    // -------------------------------------------------------------------------
    xml_writer() = default;
    /**
     * @brief Constructs a writer bound to @p log and immediately tries to open @p path.
     * @details Never throws: if opening fails, a warning is logged through @p log and the
     * writer is left closed (is_open() == false) rather than propagating the failure --
     * callers that need to react to the failure themselves should use open() directly instead.
     */
    xml_writer(const fsp_logger& log, const char* path) noexcept;
    // Non-copyable
    xml_writer(const xml_writer&)            = delete;
    xml_writer& operator=(const xml_writer&) = delete;
    xml_writer(xml_writer&& other) noexcept;
    xml_writer& operator=(xml_writer&& other) noexcept;
    ~xml_writer() noexcept;
    // -------------------------------------------------------------------------
    // Opening interface
    // -------------------------------------------------------------------------
    [[nodiscard]] e_result open(const char* path) noexcept;
    // -------------------------------------------------------------------------
    // Writing interface
    // -------------------------------------------------------------------------
    [[nodiscard]] e_result append(cstr_t data) noexcept;
    [[nodiscard]] e_result append(const char* data, std::size_t size) noexcept;
    [[nodiscard]] e_result append(const char* cstr) noexcept;
    [[nodiscard]] e_result finalize(cstr_t header) noexcept;
    void                   close() noexcept;
    /** Returns true if the underlying file is still open */
    [[nodiscard]] bool is_open() const noexcept { return file_ != nullptr; }
    /** Returns the underlying FILE* (use with care) */
    [[nodiscard]] FILE* native_handle() const noexcept { return file_; }
  private:
    // -------------------------------------------------------------------------
    // Internal helpers
    // -------------------------------------------------------------------------
    [[nodiscard]] e_result flush_batch() noexcept;
    [[nodiscard]] e_result write_padding_comment(std::size_t len) noexcept;
  private:
    // -------------------------------------------------------------------------
    // Data members
    // -------------------------------------------------------------------------
    const fsp_logger* log_  = nullptr;           // pointer (not reference) so the writer stays default-constructible/movable
    FILE*             file_ = nullptr;           // file interface
    std::string       batch_;                    //< strings to be flushed to the file
    std::size_t       written_since_advise_ = 0; //< # of characters written since last advise_
    // Cached level checks (see fsp_logger::active()) -- false when log_ is null (default-constructed
    // writer). Plain bool (not const, unlike segment_pool/doc_cutter's own log_*_ caches) because
    // xml_writer is movable and a move-assignment must be able to overwrite them.
    bool log_trace_ = false;
    bool log_debug_ = false;
    bool log_info_  = false;
    bool log_warn_  = false;
    bool log_error_ = false;
    bool log_crit_  = false;
  };
  /**
   * @brief Helper for better error handling using std::expected, mirroring
   * fsp::try_mmap_file() -- constructs and opens an xml_writer without throwing.
   */
  [[nodiscard]] inline std::expected<xml_writer, error_info> try_open_xml_writer(const char* path) noexcept
  {
    xml_writer writer;
    if (auto res = writer.open(path); ! res) { return std::unexpected(std::move(res.error())); }
    return writer;
  }
  /**
   * @brief Constructs a writer bound to @p log and immediately tries to open @p path.
   * @details Never throws; see the declaration's doc comment for the failure behavior.
   */
  inline xml_writer::xml_writer(const fsp_logger& log, const char* path) noexcept
  : log_(&log)
  , log_trace_(log_->active(lvl_enum::trace))
  , log_debug_(log_->active(lvl_enum::debug))
  , log_info_(log_->active(lvl_enum::info))
  , log_warn_(log_->active(lvl_enum::warn))
  , log_error_(log_->active(lvl_enum::err))
  , log_crit_(log_->active(lvl_enum::crit))
  {
    if (auto res = open(path); ! res && log_warn_) { log_->warn(fmt::format("xml_writer: {}", res.error().to_string())); }
  }
  // Movable
  inline xml_writer::xml_writer(xml_writer&& other) noexcept
  : log_(std::exchange(other.log_, nullptr))
  , file_(std::exchange(other.file_, nullptr))
  , batch_(std::move(other.batch_))
  , written_since_advise_(other.written_since_advise_)
  , log_trace_(other.log_trace_)
  , log_debug_(other.log_debug_)
  , log_info_(other.log_info_)
  , log_warn_(other.log_warn_)
  , log_error_(other.log_error_)
  , log_crit_(other.log_crit_)
  { other.written_since_advise_ = 0; }
  inline xml_writer& xml_writer::operator=(xml_writer&& other) noexcept
  {
    if (this != &other)
    {
      close();
      log_                        = std::exchange(other.log_, nullptr);
      file_                       = std::exchange(other.file_, nullptr);
      batch_                      = std::move(other.batch_);
      written_since_advise_       = other.written_since_advise_;
      log_trace_                  = other.log_trace_;
      log_debug_                  = other.log_debug_;
      log_info_                   = other.log_info_;
      log_warn_                   = other.log_warn_;
      log_error_                  = other.log_error_;
      log_crit_                   = other.log_crit_;
      other.written_since_advise_ = 0;
    }
    return *this;
  }
  inline xml_writer::~xml_writer() noexcept { close(); }
  /**
   * @brief Opens @p path for writing and reserves HEADER_RESERVE bytes at the start.
   * @details Never throws; any pre-existing open file is closed first (best-effort,
   * mirroring close()'s own no-throw contract) before the new one is opened.
   * @param path Path of the file to create/truncate for writing.
   * @return An empty expected on success, or the error message on failure.
   */
  inline e_result xml_writer::open(const char* path) noexcept
  {
    close();

    file_ = std::fopen(path, "wb"); // NOLINT(cppcoreguidelines-owning-memory)
    if (file_ == nullptr) { return std::unexpected(detail::io_error(str_t("xml_writer: cannot open: ") + detail::errno_str(errno), path)); }

    if (std::setvbuf(file_, nullptr, _IOFBF, IO_BUFFER_SIZE) != 0)
    {
      std::fclose(file_); // NOLINT(cert-err33-c,cppcoreguidelines-owning-memory) -- best-effort cleanup, failure has no better recourse
      file_ = nullptr;
      return std::unexpected(detail::io_error("xml_writer: setvbuf failed", path));
    }

    // Reserve header space
    const std::vector<char> pad(HEADER_RESERVE, ' ');
    if (std::fwrite(pad.data(), 1, HEADER_RESERVE, file_) != HEADER_RESERVE)
    {
      std::fclose(file_); // NOLINT(cert-err33-c,cppcoreguidelines-owning-memory) -- best-effort cleanup, failure has no better recourse
      file_ = nullptr;
      return std::unexpected(detail::io_error("xml_writer: failed to reserve header space", path));
    }

    batch_.clear();
    batch_.reserve(BATCH_SIZE + BATCH_SLACK);
    written_since_advise_ = HEADER_RESERVE;
    return {};
  }

  /**
   * @brief Appends a contiguous block of data (XML fragment).
   * @details Data is first collected in an internal batch and written when the batch
   * reaches BATCH_SIZE.
   */
  inline e_result xml_writer::append(cstr_t data) noexcept
  {
    if (data.empty() || file_ == nullptr) return {};

    // Flush current batch if the new data would make it too large
    if (! batch_.empty() && batch_.size() + data.size() > BATCH_SIZE)
    {
      if (auto res = flush_batch(); ! res) { return res; }
    }

    batch_.append(data.data(), data.size());

    if (batch_.size() >= BATCH_SIZE) { return flush_batch(); }
    return {};
  }

  /** Convenience overload */
  inline e_result xml_writer::append(const char* data, std::size_t size) noexcept { return append(cstr_t(data, size)); }

  /** Convenience overload for null-terminated C strings */
  inline e_result xml_writer::append(const char* cstr) noexcept
  {
    if (cstr != nullptr) return append(cstr_t(cstr));
    return {};
  }

  /**
   * @brief Finalizes the file:
   *  1. Flushes any remaining batched data
   *  2. Seeks to the beginning and overwrites the reserved area with @p header
   *  3. Turns any leftover reserved bytes into a well-formed XML comment
   *
   * After this call the writer should not be used for further appends.
   *
   * @param header The complete XML header (must be <= HEADER_RESERVE bytes)
   * @return An empty expected on success, or the error on failure.
   */
  inline e_result xml_writer::finalize(cstr_t header) noexcept
  {
    if (file_ == nullptr) { return std::unexpected(detail::io_error("xml_writer: finalize called on closed writer")); }

    if (auto res = flush_batch(); ! res) { return res; }

    if (header.size() > HEADER_RESERVE)
    {
      return std::unexpected(
        detail::io_error(str_t("xml_writer: header exceeds reserved space (") + std::to_string(HEADER_RESERVE) + " bytes)"));
    }

    if (std::fseek(file_, 0, SEEK_SET) != 0)
    {
      return std::unexpected(detail::io_error(str_t("xml_writer: fseek failed: ") + detail::errno_str(errno)));
    }

    if (std::fwrite(header.data(), 1, header.size(), file_) != header.size())
    {
      return std::unexpected(detail::io_error("xml_writer: failed to write header"));
    }

    // Turn remaining reserved space into an XML comment so the file stays valid
    const std::size_t remaining = HEADER_RESERVE - header.size();
    if (auto res = write_padding_comment(remaining); ! res) { return res; }

    if (std::fflush(file_) != 0)
    {
      return std::unexpected(detail::io_error(str_t("xml_writer: fflush failed: ") + detail::errno_str(errno)));
    }
    return {};
  }

  /**
   * @brief Explicitly close the file (also called by the destructor).
   * @details Any pending batch data is written, but the header is NOT finalized.
   * Best-effort and never throws, matching the destructor's no-throw contract;
   * prefer calling finalize() before the object goes out of scope so write
   * failures are observable.
   */
  inline void xml_writer::close() noexcept
  {
    if (file_ == nullptr) return;

    // Best-effort flush of remaining batch (no exceptions in destructor/close);
    // a failed write here is unrecoverable since the caller already gave up
    // ownership of the batch, so the result is deliberately discarded.
    if (! batch_.empty())
    {
      std::ignore = std::fwrite(batch_.data(), 1, batch_.size(), file_);
      batch_.clear();
    }

    std::fclose(file_); // NOLINT(cert-err33-c,cppcoreguidelines-owning-memory) -- best-effort cleanup, failure has no better recourse
    file_                 = nullptr;
    written_since_advise_ = 0;
  }

  inline e_result xml_writer::flush_batch() noexcept
  {
    if (batch_.empty() || file_ == nullptr) return {};

    const std::size_t n = batch_.size();
    if (std::fwrite(batch_.data(), 1, n, file_) != n)
    {
      return std::unexpected(detail::io_error(str_t("xml_writer: write failed: ") + detail::errno_str(errno)));
    }

    written_since_advise_ += n;
    batch_.clear();

    // Periodically tell the kernel it can drop the already-written pages
    if (written_since_advise_ >= ADVISE_INTERVAL)
    {
      if (std::fflush(file_) != 0)
      {
        return std::unexpected(detail::io_error(str_t("xml_writer: fflush failed: ") + detail::errno_str(errno)));
      }
      const int fd = fileno(file_);
      // Advise the whole file so far; kernel will drop clean pages. This is
      // a hint only -- POSIX_FADV_DONTNEED failures don't affect correctness,
      // so the return status is deliberately not surfaced as an error.
      std::ignore           = ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
      written_since_advise_ = 0;
    }
    return {};
  }

  /**
   * @brief Writes a well-formed XML comment that occupies exactly @p len bytes.
   * @details If there is not enough room for a proper comment, plain spaces are written.
   */
  inline e_result xml_writer::write_padding_comment(std::size_t len) noexcept
  {
    static constexpr std::array<char, 4> COMMENT_OPEN  = {'<', '!', '-', '-'};
    static constexpr std::array<char, 3> COMMENT_CLOSE = {'-', '-', '>'};
    // Minimum size of a non-empty comment: "<!-- -->"
    static constexpr std::size_t MIN_COMMENT_LEN  = COMMENT_OPEN.size() + COMMENT_CLOSE.size() + 1;
    static constexpr std::size_t SPACE_CHUNK_LEN  = 4096; // written in chunks to avoid a huge temporary buffer
    static constexpr std::size_t FALLBACK_BUF_LEN = 8;    // used when there's no room for a full comment

    if (len == 0) return {};

    if (len >= MIN_COMMENT_LEN)
    {
      if (std::fwrite(COMMENT_OPEN.data(), 1, COMMENT_OPEN.size(), file_) != COMMENT_OPEN.size())
      {
        return std::unexpected(detail::io_error("xml_writer: failed to write padding comment"));
      }
      len -= COMMENT_OPEN.size();

      if (len > COMMENT_CLOSE.size())
      {
        // Fill the middle with spaces
        const std::size_t                 spaces = len - COMMENT_CLOSE.size();
        std::array<char, SPACE_CHUNK_LEN> space_buf{};
        space_buf.fill(' ');

        std::size_t remaining_spaces = spaces;
        while (remaining_spaces > 0)
        {
          const std::size_t chunk = std::min(remaining_spaces, SPACE_CHUNK_LEN);
          if (std::fwrite(space_buf.data(), 1, chunk, file_) != chunk)
          {
            return std::unexpected(detail::io_error("xml_writer: failed to write padding comment"));
          }
          remaining_spaces -= chunk;
        }
      }

      if (std::fwrite(COMMENT_CLOSE.data(), 1, COMMENT_CLOSE.size(), file_) != COMMENT_CLOSE.size())
      {
        return std::unexpected(detail::io_error("xml_writer: failed to write padding comment"));
      }
    }
    else
    {
      // Not enough room for a comment - just write spaces
      std::array<char, FALLBACK_BUF_LEN> space_buf{};
      space_buf.fill(' ');
      if (std::fwrite(space_buf.data(), 1, len, file_) != len)
      {
        return std::unexpected(detail::io_error("xml_writer: failed to write padding comment"));
      }
    }
    return {};
  }
} // namespace fsp

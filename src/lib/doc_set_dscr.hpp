// doc_set_dscr.hpp
#pragma once
#include "doc_dscr.hpp"
#include "logger.hpp"
#include <vector>

namespace fsp
{
  /**
   * @brief Document set descriptor - manages a collection of documents
   *
   * This class manages a vector of document descriptors and a grammar document.
   * It provides access to individual documents in the set and supports logging.
   */
  class doc_set_dscr
  {
  public:
    // Type aliases for convenience
    using size_type                           = std::vector<doc_dscr>::size_type;
    using iterator                            = std::vector<doc_dscr>::iterator;
    using const_iterator                      = std::vector<doc_dscr>::const_iterator;
    static constexpr const auto init_vec_size = 16UL;

    explicit doc_set_dscr(const fsp_logger& logger, size_type initial_size = init_vec_size);
    ~doc_set_dscr() = default;
    // Copy/move semantics
    doc_set_dscr(const doc_set_dscr&)            = delete;
    doc_set_dscr& operator=(const doc_set_dscr&) = delete;

    doc_set_dscr(doc_set_dscr&&)            = default;
    doc_set_dscr& operator=(doc_set_dscr&&) = delete;

    /**
     * @brief Add a document to the set
     *
     * @param path Path to the document file
     * @return true if document was successfully added
     */
    bool add_document(const std::string& path)
    {
      try
      {
        doc_vec_.emplace_back(path);
        log_.info(fmt::format("Added document: '{}' ({} total documents)", path, doc_vec_.size()));
        return true;
      }
      catch (const std::exception& e)
      {
        log_.error(fmt::format("Failed to add document '{}': {}", path, e.what()));
        return false;
      }
    }

    /**
     * @brief Add a document by moving an existing doc_dscr
     *
     * @param doc Document descriptor to move into the set
     * @return true if document was successfully added
     */
    bool add_document(doc_dscr&& doc)
    {
      try
      {
        auto path = doc.path(); // Capture path before move
        doc_vec_.push_back(std::move(doc));
        log_.info(fmt::format("Added document: '{}' ({} total documents)", path, doc_vec_.size()));
        return true;
      }
      catch (const std::exception& e)
      {
        log_.error(fmt::format("Failed to add document: {}", e.what()));
        return false;
      }
    }

    /**
     * @brief Set the grammar document
     *
     * @param path Path to the grammar document file
     * @return true if grammar was successfully loaded
     */
    bool set_grammar(const std::string& path)
    {
      try
      {
        grammar_.open(path);
        log_.info(fmt::format("Grammar document set: '{}'", path));
        return true;
      }
      catch (const std::exception& e)
      {
        log_.error(fmt::format("Failed to set grammar '{}': {}", path, e.what()));
        return false;
      }
    }

    /**
     * @brief Set the grammar by moving an existing doc_dscr
     *
     * @param doc Grammar document descriptor to move
     * @return true if grammar was successfully set
     */
    bool set_grammar(doc_dscr&& doc)
    {
      try
      {
        auto path = doc.path();
        grammar_  = std::move(doc);
        log_.info(fmt::format("Grammar document set: '{}'", path));
        return true;
      }
      catch (const std::exception& e)
      {
        log_.error(fmt::format("Failed to set grammar: {}", e.what()));
        return false;
      }
    }

    /**
     * @brief Access document by index (non-const)
     *
     * @param pos Index of the document
     * @return Reference to the doc_dscr at the specified position
     * @throws std::out_of_range if pos is out of bounds
     */
    [[nodiscard]] doc_dscr& operator[](size_type pos)
    {
      if (pos >= doc_vec_.size())
      {
        log_.error(fmt::format("doc_set_dscr::operator[]: index {} out of range (size: {})", pos, doc_vec_.size()));
        throw std::out_of_range("doc_set_dscr::operator[]: index out of range");
      }
      return doc_vec_[pos];
    }

    /**
     * @brief Access document by index (const)
     *
     * @param pos Index of the document
     * @return Const reference to the doc_dscr at the specified position
     * @throws std::out_of_range if pos is out of bounds
     */
    [[nodiscard]] const doc_dscr& operator[](size_type pos) const
    {
      if (pos >= doc_vec_.size())
      {
        // Can't log from const method with non-const logger
        throw std::out_of_range("doc_set_dscr::operator[] const: index out of range");
      }
      return doc_vec_[pos];
    }

    /**
     * @brief Access document by index with bounds checking (non-const)
     *
     * @param pos Index of the document
     * @return Reference to the doc_dscr at the specified position
     * @throws std::out_of_range if pos is out of bounds
     */
    [[nodiscard]] doc_dscr& at(size_type pos)
    {
      if (pos >= doc_vec_.size())
      {
        log_.error(fmt::format("doc_set_dscr::at: index {} out of range (size: {})", pos, doc_vec_.size()));
        throw std::out_of_range("doc_set_dscr::at: index out of range");
      }
      return doc_vec_[pos];
    }

    /**
     * @brief Access document by index with bounds checking (const)
     *
     * @param pos Index of the document
     * @return Const reference to the doc_dscr at the specified position
     * @throws std::out_of_range if pos is out of bounds
     */
    [[nodiscard]] const doc_dscr& at(size_type pos) const
    {
      if (pos >= doc_vec_.size()) { throw std::out_of_range("doc_set_dscr::at const: index out of range"); }
      return doc_vec_[pos];
    }

    /**
     * @brief Get reference to the grammar document
     *
     * @return Reference to the grammar doc_dscr
     */
    [[nodiscard]] doc_dscr& grammar() noexcept { return grammar_; }

    /**
     * @brief Get const reference to the grammar document
     *
     * @return Const reference to the grammar doc_dscr
     */
    [[nodiscard]] const doc_dscr& grammar() const noexcept { return grammar_; }

    /**
     * @brief Get the number of documents in the set
     *
     * @return Size of the document vector
     */
    [[nodiscard]] size_type size() const noexcept { return doc_vec_.size(); }

    /**
     * @brief Check if the document set is empty
     *
     * @return true if there are no documents
     */
    [[nodiscard]] bool empty() const noexcept { return doc_vec_.empty(); }

    /**
     * @brief Clear all documents from the set
     */
    void clear() noexcept
    {
      size_t old_size = doc_vec_.size();
      doc_vec_.clear();
      log_.debug(fmt::format("Cleared {} documents from set", old_size));
    }

    /**
     * @brief Reserve capacity for documents
     *
     * @param new_capacity New capacity to reserve
     */
    void reserve(size_type new_capacity)
    {
      doc_vec_.reserve(new_capacity);
      log_.debug(fmt::format("Reserved capacity: {} for document set", new_capacity));
    }

    /**
     * @brief Check if the grammar document is loaded
     *
     * @return true if grammar is open
     */
    [[nodiscard]] bool has_grammar() const noexcept { return static_cast<bool>(grammar_); }

    // Iterator support
    [[nodiscard]] iterator       begin() noexcept { return doc_vec_.begin(); }
    [[nodiscard]] const_iterator begin() const noexcept { return doc_vec_.begin(); }
    [[nodiscard]] const_iterator cbegin() const noexcept { return doc_vec_.cbegin(); }
    [[nodiscard]] iterator       end() noexcept { return doc_vec_.end(); }
    [[nodiscard]] const_iterator end() const noexcept { return doc_vec_.end(); }
    [[nodiscard]] const_iterator cend() const noexcept { return doc_vec_.cend(); }

    /**
     * @brief Get the logger reference
     *
     * @return Reference to the logger
     */
    [[nodiscard]] const fsp_logger& log() const noexcept { return log_; }
  private:
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    const fsp_logger&     log_;     ///< Reference to logger (must outlive this object)
    std::vector<doc_dscr> doc_vec_; ///< Vector of document descriptors
    doc_dscr              grammar_; ///< Grammar document descriptor
  };

  /**
   * @brief Construct a document set descriptor
   *
   * @param logger Reference to logger (must outlive this object)
   * @param initial_size Initial capacity of the document vector
   *
   * @note The logger reference is stored as a reference, so it must remain
   *       valid for the lifetime of this object. Consider using shared_ptr
   *       if lifetime management is uncertain.
   */
  inline doc_set_dscr::doc_set_dscr(const fsp_logger& logger, size_type initial_size)
  : log_(logger)
  {
    doc_vec_.reserve(initial_size);
    log_.debug(fmt::format("doc_set_dscr constructed with initial capacity: {}", initial_size));
  }
} // namespace fsp
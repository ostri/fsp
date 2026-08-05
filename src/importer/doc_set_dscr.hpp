// doc_set_dscr.hpp
#pragma once
#include "doc_dscr.hpp"
#include <logger/logger.hpp>
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

    doc_set_dscr() = delete;
    explicit doc_set_dscr(const logger::Logger& logger, size_type initial_size = init_vec_size);
    ~doc_set_dscr() = default;
    // Copy/move semantics
    doc_set_dscr(const doc_set_dscr&)                                    = delete;
    doc_set_dscr& operator=(const doc_set_dscr&)                         = delete;
    doc_set_dscr(doc_set_dscr&&)                                         = default;
    doc_set_dscr&                              operator=(doc_set_dscr&&) = delete;
    bool                                       add_document(cstr_t path);
    bool                                       add_document(doc_dscr&& doc);
    bool                                       set_grammar(cstr_t path);
    bool                                       set_grammar(doc_dscr&& doc);
    [[nodiscard]] doc_dscr&                    operator[](size_type pos);
    [[nodiscard]] const doc_dscr&              operator[](size_type pos) const;
    [[nodiscard]] doc_dscr&                    at(size_type pos);
    [[nodiscard]] const doc_dscr&              at(size_type pos) const;
    [[nodiscard]] doc_dscr&                    grammar() noexcept;
    [[nodiscard]] const doc_dscr&              grammar() const noexcept;
    [[nodiscard]] doc_set_dscr::size_type      size() const noexcept;
    [[nodiscard]] bool                         empty() const noexcept;
    void                                       clear() noexcept;
    void                                       reserve(size_type new_capacity);
    [[nodiscard]] bool                         has_grammar() const noexcept;
    [[nodiscard]] iterator                     begin() noexcept;
    [[nodiscard]] const_iterator               begin() const noexcept;
    [[nodiscard]] const_iterator               cbegin() const noexcept;
    [[nodiscard]] iterator                     end() noexcept;
    [[nodiscard]] const_iterator               end() const noexcept;
    [[nodiscard]] const_iterator               cend() const noexcept;
    [[nodiscard]] const logger::Logger&        log() const noexcept;
    [[nodiscard]] const std::vector<doc_dscr>& doc_set() const;
    std::vector<doc_dscr>&                     doc_set();
    [[nodiscard]] cstr_t                       xsd_file() const;
  private:
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    const logger::Logger& log_;     ///< Reference to logger (must outlive this object)
    std::vector<doc_dscr> doc_set_; ///< Vector of document descriptors
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
  inline doc_set_dscr::doc_set_dscr(const logger::Logger& logger, size_type initial_size)
  : log_(logger)
  {
    doc_set_.reserve(initial_size);
    log_.debug(fmt::format("doc_set_dscr constructed with initial capacity: {}", initial_size));
  }

  /**
   * @brief Add a document to the set
   *
   * @param path Path to the document file
   * @return true if document was successfully added
   */
  inline bool doc_set_dscr::add_document(cstr_t path)
  {
    try
    {
      if (! path.empty())
      {
        doc_set_.emplace_back(path);
        log_.trace(fmt::format("Added document: '{}' ({} total documents)", path, doc_set_.size()));
        return true;
      }
      return false;
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
  inline bool doc_set_dscr::add_document(doc_dscr&& doc)
  {
    try
    {
      auto path = doc.path(); // Capture path before move
      doc_set_.push_back(std::move(doc));
      log_.trace(fmt::format("Added document: '{}' ({} total documents)", path, doc_set_.size()));
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
  inline bool doc_set_dscr::set_grammar(cstr_t path)
  {
    try
    {
      if (! path.empty())
      {
        grammar_ = doc_dscr(path);
        log_.info(fmt::format("Grammar document set: '{}'", path));
        return true;
      }
      return false;
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
  inline bool doc_set_dscr::set_grammar(doc_dscr&& doc)
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
  inline doc_dscr& doc_set_dscr::operator[](size_type pos)
  {
    if (pos >= doc_set_.size())
    {
      log_.error(fmt::format("doc_set_dscr::operator[]: index {} out of range (size: {})", pos, doc_set_.size()));
      throw std::out_of_range("doc_set_dscr::operator[]: index out of range");
    }
    return doc_set_[pos];
  }

  /**
   * @brief Access document by index (const)
   *
   * @param pos Index of the document
   * @return Const reference to the doc_dscr at the specified position
   * @throws std::out_of_range if pos is out of bounds
   */
  inline const doc_dscr& doc_set_dscr::operator[](size_type pos) const
  {
    if (pos >= doc_set_.size())
    {
      // Can't log from const method with non-const logger
      throw std::out_of_range("doc_set_dscr::operator[] const: index out of range");
    }
    return doc_set_[pos];
  }

  /**
   * @brief Access document by index with bounds checking (non-const)
   *
   * @param pos Index of the document
   * @return Reference to the doc_dscr at the specified position
   * @throws std::out_of_range if pos is out of bounds
   */
  inline doc_dscr& doc_set_dscr::at(size_type pos)
  {
    if (pos >= doc_set_.size())
    {
      log_.error(fmt::format("doc_set_dscr::at: index {} out of range (size: {})", pos, doc_set_.size()));
      throw std::out_of_range("doc_set_dscr::at: index out of range");
    }
    return doc_set_[pos];
  }

  /**
   * @brief Access document by index with bounds checking (const)
   *
   * @param pos Index of the document
   * @return Const reference to the doc_dscr at the specified position
   * @throws std::out_of_range if pos is out of bounds
   */
  inline const doc_dscr& doc_set_dscr::at(size_type pos) const
  {
    if (pos >= doc_set_.size()) { throw std::out_of_range("doc_set_dscr::at const: index out of range"); }
    return doc_set_[pos];
  }

  /**
   * @brief Get reference to the grammar document
   *
   * @return Reference to the grammar doc_dscr
   */
  inline doc_dscr& doc_set_dscr::grammar() noexcept { return grammar_; }

  /**
   * @brief Get const reference to the grammar document
   *
   * @return Const reference to the grammar doc_dscr
   */
  inline const doc_dscr& doc_set_dscr::grammar() const noexcept { return grammar_; }

  /**
   * @brief Get the number of documents in the set
   *
   * @return Size of the document vector
   */
  inline doc_set_dscr::size_type doc_set_dscr::size() const noexcept { return doc_set_.size(); }

  /**
   * @brief Check if the document set is empty
   *
   * @return true if there are no documents
   */
  inline bool doc_set_dscr::empty() const noexcept { return doc_set_.empty(); }

  /**
   * @brief Clear all documents from the set
   */
  inline void doc_set_dscr::clear() noexcept
  {
    size_t old_size = doc_set_.size();
    doc_set_.clear();
    log_.debug(fmt::format("Cleared {} documents from set", old_size));
  }

  /**
   * @brief Reserve capacity for documents
   *
   * @param new_capacity New capacity to reserve
   */
  inline void doc_set_dscr::reserve(size_type new_capacity)
  {
    doc_set_.reserve(new_capacity);
    log_.debug(fmt::format("Reserved capacity: {} for document set", new_capacity));
  }

  /**
   * @brief Check if the grammar document is loaded
   *
   * @return true if grammar is open
   */
  inline bool doc_set_dscr::has_grammar() const noexcept { return static_cast<bool>(grammar_); }
  // Iterator support
  inline doc_set_dscr::iterator       doc_set_dscr::begin() noexcept { return doc_set_.begin(); }
  inline doc_set_dscr::const_iterator doc_set_dscr::begin() const noexcept { return doc_set_.begin(); }
  inline doc_set_dscr::const_iterator doc_set_dscr::cbegin() const noexcept { return doc_set_.cbegin(); }
  inline doc_set_dscr::iterator       doc_set_dscr::end() noexcept { return doc_set_.end(); }
  inline doc_set_dscr::const_iterator doc_set_dscr::end() const noexcept { return doc_set_.end(); }
  inline doc_set_dscr::const_iterator doc_set_dscr::cend() const noexcept { return doc_set_.cend(); }
  /**
   * @brief Get the logger reference
   *
   * @return Reference to the logger
   */
  inline const logger::Logger&        doc_set_dscr::log() const noexcept { return log_; }
  inline const std::vector<doc_dscr>& doc_set_dscr::doc_set() const { return doc_set_; }
  inline std::vector<doc_dscr>&       doc_set_dscr::doc_set() { return doc_set_; }
  inline cstr_t                       doc_set_dscr::xsd_file() const
  {
    if (has_grammar()) return grammar_.path();
    return "";
  }
} // namespace fsp
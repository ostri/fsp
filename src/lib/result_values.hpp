// result_values.hpp
#pragma once
#include "xpath_set.hpp"
#include <bit>
#include <cstdint>
#include <fmt/format.h>
#include <optional>
#include <vector>

namespace fsp
{
  using vec_str_t = std::vector<str_t>;
  /**
   * @brief Named, typed view over one segment's extracted xpath values.
   *
   * Constructed empty (sized from schema, all slots unset) by process_segment() as the
   * destination that segment_sax::exec() fills in place while walking the segment's XML tree,
   * via set()/add(). schema_ points at the xpath_set for THIS segment's own type (proc_data's
   * 2nd level, e.g. targets.xpaths[seg_type]) -- the framework supplies it because the segment
   * already knows its own subtree_type().
   *
   * Scalar and array xpaths are stored in two separate vectors: most xpaths in a real schema
   * (e.g. all of SEPA pacs) are single-valued, so paying for a std::vector<str_t> per
   * field there would waste an allocation per field for something that only ever holds 0 or 1
   * entries.
   *
   * @note Constructed/destroyed millions of times per session (once per processed segment) --
   * kept rule-of-zero (no custom copy/move/dtor) and the constructor only pre-sizes the two
   * vectors, doing no scanning/validation work.
   */
  class result_values
  {
  public:
    result_values() = default;
    explicit result_values(const xpath_set& schema);
    void                           reset(const xpath_set& schema);
    void                           set(std::size_t idx, str_t value) noexcept;
    void                           set(cstr_t name, str_t value);
    void                           add(std::size_t idx, str_t value);
    void                           add(cstr_t name, str_t value);
    [[nodiscard]] std::size_t      size() const noexcept;
    [[nodiscard]] bool             empty() const noexcept;
    [[nodiscard]] bool             found(std::size_t idx) const noexcept;
    [[nodiscard]] bool             found(cstr_t name) const;
    [[nodiscard]] cstr_t           value(std::size_t idx) const noexcept;
    [[nodiscard]] cstr_t           value(cstr_t name) const;
    [[nodiscard]] const vec_str_t& values(std::size_t idx) const noexcept;
    [[nodiscard]] const vec_str_t& values(cstr_t name) const;
    [[nodiscard]] bool             complete() const;
    [[nodiscard]] str_t            dump(int offs = 0) const;
  private:
    [[nodiscard]] std::size_t index_of(cstr_t name) const;
    [[nodiscard]] bool        is_array_xpath(std::size_t idx) const noexcept;
    [[nodiscard]] std::size_t array_slot(std::size_t idx) const noexcept;
    [[nodiscard]] std::size_t scalar_slot(std::size_t idx) const noexcept;
  private: // members
    /** @brief Non-owning; points at a static/constexpr xpath_set that outlives the whole run. */
    const xpath_set* schema_ = nullptr;
    /** @brief One slot per non-array xpath; nullopt = not found. */
    std::vector<std::optional<str_t>> scalars_;
    /** @brief One slot per array xpath; empty vector = not found. */
    std::vector<std::vector<str_t>> arrays_;
  };

  /**
   * @brief Pre-sizes empty storage for every xpath in schema; call set()/add() to fill it in.
   * @param schema The xpath_set matching this segment's own subtree type. Must outlive this
   * object (expected to be a static/constexpr xpath_set living for the whole program).
   */
  inline result_values::result_values(const xpath_set& schema) { reset(schema); }

  /**
   * @brief Rewinds this instance to an empty state for schema, reusing scalars_/arrays_'
   * already-allocated capacity instead of reallocating -- the intended way to reuse one
   * instance across many segments (one per worker thread, reset() once per segment), so the
   * only per-segment allocations left are for the values actually extracted, not the
   * scaffolding around them. Safe to call with a different schema than last time (e.g.
   * alternating between a document's header and transaction segment types): scalars_/arrays_
   * are resized to match, which reallocates only if this schema's counts exceed any
   * previously-seen high-water mark.
   * @param schema The xpath_set matching this segment's own subtree type.
   */
  inline void result_values::reset(const xpath_set& schema)
  {
    schema_                 = &schema;
    const auto array_count  = static_cast<std::size_t>(std::popcount(schema.array_mask()));
    const auto scalar_count = schema.size() - array_count;
    scalars_.resize(scalar_count);
    arrays_.resize(array_count);
    for (auto& s : scalars_) s.reset();
    for (auto& a : arrays_) a.clear();
  }

  /**
   * @brief Records the value for a scalar (non-array) xpath, overwriting any previous value.
   * @param idx   Original index of the xpath within schema().
   * @param value Extracted value (may legitimately be empty -- still counts as found()).
   */
  inline void result_values::set(std::size_t idx, str_t value) noexcept { scalars_[scalar_slot(idx)] = std::move(value); }

  /** @brief Name-based overload of set() -- looks up idx via the schema. */
  inline void result_values::set(cstr_t name, str_t value) { set(index_of(name), std::move(value)); }

  /**
   * @brief Appends one more occurrence to an array xpath.
   * @param idx   Original index of the xpath within schema().
   * @param value Extracted value for this occurrence.
   */
  inline void result_values::add(std::size_t idx, str_t value) { arrays_[array_slot(idx)].push_back(std::move(value)); }

  /** @brief Name-based overload of add() -- looks up idx via the schema. */
  inline void result_values::add(cstr_t name, str_t value) { add(index_of(name), std::move(value)); }

  /** @brief Total number of xpaths declared in the schema (scalar + array). */
  inline std::size_t result_values::size() const noexcept { return schema_ != nullptr ? schema_->size() : 0; }

  /** @brief True iff the schema has no xpaths at all (mirrors the old container's empty()). */
  inline bool result_values::empty() const noexcept { return size() == 0; }

  /**
   * @brief Whether ANY value was recorded for this xpath.
   * @details Distinguishes "" found in the XML from "never set" -- for scalars via
   * std::optional, for arrays via vector emptiness.
   */
  inline bool result_values::found(std::size_t idx) const noexcept
  { return is_array_xpath(idx) ? ! arrays_[array_slot(idx)].empty() : scalars_[scalar_slot(idx)].has_value(); }

  /** @brief Name-based overload of found(). */
  inline bool result_values::found(cstr_t name) const { return found(index_of(name)); }

  /** @brief Value of a scalar xpath; empty string_view if found() == false. */
  inline cstr_t result_values::value(std::size_t idx) const noexcept
  {
    const auto& v = scalars_[scalar_slot(idx)];
    return v ? cstr_t(*v) : cstr_t{};
  }

  /** @brief Name-based overload of value(). */
  inline cstr_t result_values::value(cstr_t name) const { return value(index_of(name)); }

  /** @brief All occurrences of an array xpath; empty vector if found() == false. */
  inline const std::vector<str_t>& result_values::values(std::size_t idx) const noexcept { return arrays_[array_slot(idx)]; }

  /** @brief Name-based overload of values(). */
  inline const vec_str_t& result_values::values(cstr_t name) const { return values(index_of(name)); }

  /** @brief True iff every non-optional (is_opt() == false) xpath in the schema has found() == true. */
  inline bool result_values::complete() const
  {
    if (schema_ == nullptr) return false;
    for (std::size_t i = 0; i < schema_->size(); ++i)
      if (! (*schema_)[i].is_opt() && ! found(i)) return false;
    return true;
  }

  /** @brief One "name : [values]" line per xpath, in schema order. */
  inline str_t result_values::dump(int offs) const
  {
    str_t msg;
    for (std::size_t i = 0; i < size(); ++i)
    {
      str_t joined;
      if (is_array_xpath(i))
      {
        for (const auto& v : values(i)) joined += fmt::format("'{}', ", v);
        if (! joined.empty()) joined.resize(joined.size() - 2);
      }
      else if (found(i)) joined = fmt::format("'{}'", value(i));
      msg += fmt::format("{}{:15} : [{}]\n", str_t(offs, ' '), (*schema_)[i].name(), joined);
    }
    return msg;
  }

  /** @brief Resolves a field name to its original schema index. */
  inline std::size_t result_values::index_of(cstr_t name) const { return (*schema_)[name].original_ndx(); }

  /** @brief Whether the xpath at idx is array (multi-valued) type, per schema's array_mask(). */
  inline bool result_values::is_array_xpath(std::size_t idx) const noexcept
  { return ((schema_->array_mask() >> idx) & std::uint64_t{1}) != 0; }

  /** @brief This xpath's own slot within arrays_ (count of array-type xpaths below idx). */
  inline std::size_t result_values::array_slot(std::size_t idx) const noexcept
  { return static_cast<std::size_t>(std::popcount(schema_->array_mask() & ((std::uint64_t{1} << idx) - 1))); }

  /** @brief This xpath's own slot within scalars_ (count of scalar-type xpaths below idx). */
  inline std::size_t result_values::scalar_slot(std::size_t idx) const noexcept { return idx - array_slot(idx); }
} // namespace fsp
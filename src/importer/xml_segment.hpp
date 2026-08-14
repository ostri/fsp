#pragma once
#include "common.hpp"
#include "x_str.hpp"
#include <cstddef>
#include <fmt/format.h>
#include <string_view>

namespace fsp
{
  using str_t = std::string;
  class xml_segment
  {
  public:
    xml_segment() = default;
    xml_segment(std::size_t id,           //< unique segment id
                int         subtree_type, //< target index / subtree type
                int         doc_ndx,      //< index of the document the segment belongs to
                std::size_t offset,       //< start from the beggining of the buffer
                std::size_t length,       //< length of this segment (whole xml subtree)
                str_XMLCh_t ns,           //< set of ns as a string which belongs to top level tag
                str_XMLCh_t attrs         //< set of attribute values which belongs to top level tag
    );
    ~xml_segment() = default;
    // xml_segment.hpp — poenostavitev
    xml_segment(const xml_segment&)                = default;
    xml_segment(xml_segment&&) noexcept            = default;
    xml_segment& operator=(const xml_segment&)     = default;
    xml_segment& operator=(xml_segment&&) noexcept = default;

    [[nodiscard]] cstr_t view(const std::byte* mmap_base = nullptr) const noexcept;

    [[nodiscard]] bool        empty() const noexcept;
    [[nodiscard]] std::size_t id() const;
    [[nodiscard]] int         subtree_type() const;
    [[nodiscard]] std::size_t offset() const;
    [[nodiscard]] std::size_t length() const;
    // [[nodiscard]] str_XMLCh_t prefix() const;
    [[nodiscard]] str_t              subtree_str(cstr_t tree_content) const;
    [[nodiscard]] str_t              dump(int offs = 0) const;
    [[nodiscard]] str_t              dump_all(cstr_t base, int offs = 0) const;
    [[nodiscard]] str_t              dump_all(const std::byte* mmap_base = nullptr, int offs = 0) const;
    [[nodiscard]] cstr_t             extract_qname_from_offset(cstr_t base) const;
    [[nodiscard]] const str_XMLCh_t& ns_raw() const noexcept;
    [[nodiscard]] const str_XMLCh_t& attrs_raw() const noexcept;
    [[nodiscard]] int                doc_ndx() const;
    void                             set_doc_ndx(int doc_ndx);
    /**
     * @brief Semantic verdict from pipeline_hooks::on_seg_sem_check() (true = semantically correct),
     * set by pipeline::record_segment_done() right after the hook call returns. Lets a
     * on_block_safe_store()/on_failed_block_safe_store() hook, running later against a batch of pool
     * indices, tell which of the two blocks this slot's segment was already sorted into without
     * needing its own separate lookup -- see segment_pool::segment_at().
     */
    [[nodiscard]] bool valid() const noexcept;
    void               set_valid(bool valid) noexcept;
  private:
    std::size_t id_           = 0;  //< unique id of the segment
    int         subtree_type_ = -1; //< subtree type, used later for data extraction (index of the xpath rule)
    int         doc_ndx_      = -1; //< index of the document within the ds_dscr structure that this segment belongs to
    std::size_t offset_       = 0;  //< byte offset inside the buffer (segment starts at buffer[offset])
    std::size_t length_       = 0;  //< length of the subtree / segmetn in bytes
    str_XMLCh_t ns_;                //< namespaces values of the top tag (utf-16)
    str_XMLCh_t attrs_;             //< attribute values of the top tag (utf-16)
    bool        valid_ = false;     //< semantic verdict, see valid() above
  };

  inline bool               xml_segment::empty() const noexcept { return length_ == 0; }
  inline std::size_t        xml_segment::id() const { return id_; }
  inline int                xml_segment::subtree_type() const { return subtree_type_; }
  inline std::size_t        xml_segment::length() const { return length_; }
  inline const str_XMLCh_t& xml_segment::ns_raw() const noexcept { return ns_; }
  inline const str_XMLCh_t& xml_segment::attrs_raw() const noexcept { return attrs_; }
  inline int                xml_segment::doc_ndx() const { return doc_ndx_; }
  inline bool               xml_segment::valid() const noexcept { return valid_; }
  inline void               xml_segment::set_valid(bool valid) noexcept { valid_ = valid; }

} // namespace fsp

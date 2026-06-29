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
    xml_segment(std::size_t id,           // unique segment id
                int         subtree_type, // target index / subtree type
                std::size_t offset,       // start from the beggining of the buffer
                std::size_t length,       // length of this segment (whole xml subtree)
                // str_XMLCh_t prefix,       // prefix to be added before the buffer (actual start of the
                //                           // tag + inherited  ns)
                str_XMLCh_t ns,   // set of ns as a string which belongs to top level tag
                str_XMLCh_t attrs // set of attribute values which belongs to top level tag
                                  // x_str       ln,    // localname of top tag
                                  // x_str       uri    // uri of the top tag
    );
    [[nodiscard]] std::string_view view(const std::byte* mmap_base = nullptr) const noexcept;

    [[nodiscard]] bool        empty() const noexcept;
    [[nodiscard]] std::size_t id() const;
    [[nodiscard]] int         subtree_type() const;
    [[nodiscard]] std::size_t offset() const;
    [[nodiscard]] std::size_t length() const;
    // [[nodiscard]] str_XMLCh_t prefix() const;
    [[nodiscard]] std::string subtree_str(std::string_view tree_content) const;
    [[nodiscard]] std::string dump(int offs = 0) const;
    [[nodiscard]] std::string dump_all(std::string_view base, int offs = 0) const;
    [[nodiscard]] std::string dump_all(const std::byte* mmap_base = nullptr, int offs = 0) const;
    [[nodiscard]] cstr_t      extract_qname_from_offset(std::string_view base) const;
  private:
    std::size_t id_           = 0;  // unique id of the segmetn
    int         subtree_type_ = -1; // subtree type, used later for data extraction (index of the xpath rule)
    std::size_t offset_       = 0;  // byte offset inside the buffer (segment starts at buffer[offset])
    std::size_t length_       = 0;  // length of the subtree / segmetn in bytes
    // str_XMLCh_t prefix_;            // opening tag with inherited namespaces, tag namespaces and tag attributes
    str_XMLCh_t ns_;    // namespaces and attribute values of the top tag
    str_XMLCh_t attrs_; // namespaces and attribute values of the top tag
    // x_str       ln_;    // top tag localname
    // x_str       uri_;   // top tag uri
  };

} // namespace fsp

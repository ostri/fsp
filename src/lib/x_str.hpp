#pragma once

#include <xercesc/util/XMLString.hpp>
#include <string>
#include <string_view>
#include <functional>

namespace fsp
{
  class x_str
  {
  public:
    x_str() noexcept = default;
    ~x_str() { reset(); }
    explicit x_str(XMLCh* ptr) noexcept;
    explicit x_str(std::string_view utf8);
    explicit x_str(std::u16string_view u16);
    x_str(const x_str& other);
    x_str(x_str&& other) noexcept;
    /// operators
    x_str&                       operator=(const x_str& other);
    x_str&                       operator=(x_str&& other) noexcept;
    void                         reset() noexcept;
    void                         reset(XMLCh* ptr) noexcept;
    [[nodiscard]] std::string    to_string() const;
    [[nodiscard]] std::u16string to_u16string() const;
    bool                         operator==(const x_str& other) const noexcept;
    bool                         operator!=(const x_str& other) const noexcept;
    auto                         operator<=>(const x_str& other) const noexcept;
    bool                         operator==(const XMLCh* other) const noexcept;
    bool                         operator!=(const XMLCh* other) const noexcept;
    auto                         operator<=>(const XMLCh* other) const noexcept;
    bool                         operator==(std::string_view utf8) const;
    bool                         operator!=(std::string_view utf8) const;
    bool                         operator==(std::u16string_view u16) const;
    bool                         operator!=(std::u16string_view u16) const;
    explicit                     operator const XMLCh*() const noexcept;
    [[nodiscard]] const XMLCh*   c_str() const noexcept;
    [[nodiscard]] XMLCh*         data() const noexcept;
    [[nodiscard]] bool           empty() const noexcept;
    [[nodiscard]] std::size_t    length() const noexcept;
  private:
    XMLCh* m_data = nullptr;
  };
} // namespace fsp

// Hash specializacija
template <>
struct std::hash<fsp::x_str>
{
  std::size_t operator()(const fsp::x_str& s) const { return std::hash<std::string>{}(s.to_string()); }
};
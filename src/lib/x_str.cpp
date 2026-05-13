#include "x_str.hpp"
#include <stdexcept>
#include <utility>
#include <memory>
namespace fsp
{
  fsp::x_str::x_str(XMLCh* ptr) noexcept
  : m_data(ptr)
  {
  }

  x_str::x_str(std::string_view utf8)
  {
    if (! utf8.empty())
    {
      m_data = xercesc::XMLString::transcode(utf8.data()); // NOLINT(bugprone-suspicious-stringview-data-usage)
      if (m_data == nullptr) throw std::runtime_error("XMLString::transcode failed");
    }
  }

  x_str::x_str(std::u16string_view u16)
  {
    if (! u16.empty())
    {
      m_data =
        xercesc::XMLString::replicate(reinterpret_cast<const XMLCh*>(u16.data()) // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        );
      if (m_data == nullptr) throw std::runtime_error("XMLString::replicate failed");
    }
  }

  x_str::x_str(const x_str& other)
  {
    if (other.m_data != nullptr) m_data = xercesc::XMLString::replicate(other.m_data);
  }

  x_str::x_str(x_str&& other) noexcept
  : m_data(std::exchange(other.m_data, nullptr))
  {
  }

  x_str& x_str::operator=(const x_str& other)
  {
    if (this != &other)
    {
      reset();
      if (other.m_data != nullptr) m_data = xercesc::XMLString::replicate(other.m_data);
    }
    return *this;
  }

  x_str& x_str::operator=(x_str&& other) noexcept
  {
    if (this != &other)
    {
      reset();
      m_data = std::exchange(other.m_data, nullptr);
    }
    return *this;
  }

  void x_str::reset() noexcept
  {
    if (m_data != nullptr)
    {
      xercesc::XMLString::release(&m_data);
      m_data = nullptr;
    }
  }

  void x_str::reset(XMLCh* ptr) noexcept
  {
    reset();
    m_data = ptr;
  }

  [[nodiscard]] std::string x_str::to_string() const
  {
    if (empty()) return {};
    char* utf8 = xercesc::XMLString::transcode(m_data);
    if (utf8 == nullptr) throw std::runtime_error("XMLString::transcode failed");

    // RAII za Xerces char*
    auto deleter = [](char* p)
    {
      if (p) xercesc::XMLString::release(&p);
    };
    std::unique_ptr<char, decltype(deleter)> guard(utf8, deleter);

    return {utf8};
  }

  [[nodiscard]] std::u16string x_str::to_u16string() const
  {
    if (empty()) return {};
    return {reinterpret_cast<const char16_t*>(m_data)}; // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
  }

  bool x_str::operator==(const x_str& other) const noexcept { return xercesc::XMLString::equals(m_data, other.m_data); }
  bool x_str::operator!=(const x_str& other) const noexcept { return ! (*this == other); }
  auto x_str::operator<=>(const x_str& other) const noexcept { return xercesc::XMLString::compareString(m_data, other.m_data) <=> 0; }
  bool x_str::operator==(const XMLCh* other) const noexcept { return xercesc::XMLString::equals(m_data, other); }
  bool x_str::operator!=(const XMLCh* other) const noexcept { return ! (*this == other); }
  auto x_str::operator<=>(const XMLCh* other) const noexcept { return xercesc::XMLString::compareString(m_data, other) <=> 0; }
  bool x_str::operator==(std::string_view utf8) const
  {
    x_str temp(utf8);
    return *this == temp;
  }

  bool x_str::operator!=(std::string_view utf8) const { return ! (*this == utf8); }
  bool x_str::operator==(std::u16string_view u16) const
  {
    x_str temp(u16);
    return *this == temp;
  }

  bool                       x_str::operator!=(std::u16string_view u16) const { return ! (*this == u16); }
  x_str::                    operator const XMLCh*() const noexcept { return m_data; }
  [[nodiscard]] const XMLCh* x_str::c_str() const noexcept { return m_data; }
  [[nodiscard]] XMLCh*       x_str::data() const noexcept { return m_data; }
  [[nodiscard]] bool         x_str::empty() const noexcept { return m_data == nullptr || *m_data == 0; }
  [[nodiscard]] std::size_t  x_str::length() const noexcept { return m_data != nullptr ? xercesc::XMLString::stringLen(m_data) : 0; }
} // namespace fsp

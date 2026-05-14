#include "handler.hpp"
#include "x_str.hpp"
#include <iostream>


namespace fsp
{
  fsp::Handler::Handler(const std::vector<std::string>& t, segment_queue& q)
  : targets_(t)
  , queue_(q)
  {
  }

  void Handler::startElement([[maybe_unused]] const XMLCh* const         uri,
                             const XMLCh* const                          localname,
                             [[maybe_unused]] const XMLCh* const         qname,
                             [[maybe_unused]] const xercesc::Attributes& attrs)
  {
    if (m_locator != nullptr)
    {
      // Pridobivanje odmika v bajtih
      XMLFilePos offset = m_locator->getByteOffset();
      std::cout << "Element: '" << x_str(qname).to_string() << ":" << x_str(localname).to_string() << "' starts on byte: " << offset
                << "\n";
    }
    x_str name(localname);
    for (size_t i = 0; i < targets_.size(); ++i)
    {
      if (targets_[i] == name.to_string())
      {
        capturing_  = true;
        active_idx_ = static_cast<int>(i);
        buffer_     = "<" + name.to_string() + ">";
        break;
      }
    }
  }

  void Handler::characters(const XMLCh* const chars, [[maybe_unused]] const XMLSize_t length)
  {
    if (capturing_)
    {
      fsp::x_str text(chars);
      buffer_ += text.to_string();
    }
  }

  void Handler::endElement([[maybe_unused]] const XMLCh* const uri, const XMLCh* const localname, [[maybe_unused]] const XMLCh* const qname)
  {
    x_str name(localname);
    if (capturing_ && targets_[active_idx_] == name.to_string())
    {
      buffer_ += "</" + name.to_string() + ">";
      queue_.push({.id = counter_++, .xpath_index = active_idx_, .raw_content = std::move(buffer_)});
      capturing_ = false;
    }
  }

  void Handler::fatalError(const xercesc::SAXParseException& e) //
  {                                                             //
    throw std::runtime_error(x_str(e.getMessage()).to_string());
  }

  void Handler::setDocumentLocator(const xercesc::Locator* locator) { m_locator = dynamic_cast<const xercesc::DOMLocator*>(locator); }

}; // namespace fsp

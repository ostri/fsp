#pragma once

#include <xercesc/util/PlatformUtils.hpp>
#include <xercesc/sax2/XMLReaderFactory.hpp>

namespace fsp
{
  class xerces_mgr
  {
  public:
    xerces_mgr();
    ~xerces_mgr();
    // Disable copying
    xerces_mgr(const xerces_mgr&)            = delete;
    xerces_mgr& operator=(const xerces_mgr&) = delete;
    // Disable moving (to satisfy Rule of Five)
    xerces_mgr(xerces_mgr&&)            = delete;
    xerces_mgr& operator=(xerces_mgr&&) = delete;
  }; // namespace class XercesManager
}; // namespace fsp
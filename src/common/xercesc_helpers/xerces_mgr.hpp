#pragma once

#include "x_str.hpp"
#include <fmt/base.h>
#include <xercesc/util/PlatformUtils.hpp>
#include <xercesc/sax2/XMLReaderFactory.hpp>

namespace fsp
{
  class xerces_mgr
  {
  public:
    xerces_mgr();
    ~xerces_mgr();

    xerces_mgr(const xerces_mgr&)            = delete; // Disable copying
    xerces_mgr& operator=(const xerces_mgr&) = delete;
    xerces_mgr(xerces_mgr&&)                 = delete; // Disable moving (to satisfy Rule of Five)
    xerces_mgr& operator=(xerces_mgr&&)      = delete;
  };
  ////////////////////////////////////////////////////////////////////////
  inline xerces_mgr::xerces_mgr()
  {
    try
    {
      xercesc::XMLPlatformUtils::Initialize();
    }
    catch (const xercesc::XMLException& e)
    { // ugly, but it is rare
      auto msg = x_str(e.getMessage());
      fmt::print("Internal error: Error initializing Xerces: '{}'\n", msg.to_string_view());
      throw;
    }
  }
  inline xerces_mgr::~xerces_mgr() { xercesc::XMLPlatformUtils::Terminate(); }
}; // namespace fsp
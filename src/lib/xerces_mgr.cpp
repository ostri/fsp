#include "xerces_mgr.hpp"
#include <iostream>

namespace fsp
{
  fsp::xerces_mgr::xerces_mgr()
  {
    try
    {
      xercesc::XMLPlatformUtils::Initialize();
    }
    catch (const xercesc::XMLException& e)
    { // ugly but it is rare
      char* msg = xercesc::XMLString::transcode(e.getMessage());
      std::cerr << "Error initializing Xerces: " << msg << "\n";
      xercesc::XMLString::release(&msg);
      throw;
    }
  }

  xerces_mgr::~xerces_mgr() { xercesc::XMLPlatformUtils::Terminate(); }
}; // namespace fsp
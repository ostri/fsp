#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <memory>
#include <xercesc/util/PlatformUtils.hpp>
#include <xercesc/sax2/XMLReaderFactory.hpp>
#include "pugixml.hpp"
#include "lib/x_str.hpp"
#include "lib/queue.hpp"
#include "lib/handler.hpp"
#include <array>

namespace
{
  struct XercesDeleter
  {
    void operator()(xercesc::SAX2XMLReader* p) const
    {
      delete p; // NOLINT(cppcoreguidelines-owning-memory)
    }
  };
  class xerces_mgr
  {
  public:
    xerces_mgr() { xercesc::XMLPlatformUtils::Initialize(); }
    ~xerces_mgr() { xercesc::XMLPlatformUtils::Terminate(); }
    // Disable copying
    xerces_mgr(const xerces_mgr&)            = delete;
    xerces_mgr& operator=(const xerces_mgr&) = delete;
    // Disable moving (to satisfy Rule of Five)
    xerces_mgr(xerces_mgr&&)            = delete;
    xerces_mgr& operator=(xerces_mgr&&) = delete;
  }; // namespace class XercesManager


  // --- Main Worker Logic ---
  static void worker_proc(const std::stop_token& st, fsp::segment_queue& q)
  {
    while (! st.stop_requested())
    {
      xml_segment seg;
      if (! q.pop(seg)) break;

      pugi::xml_document doc;
      if (doc.load_string(seg.raw_content.c_str()))
      {
        // Business logic here
      }
    }
  }

} // namespace

int main(int argc, char* argv[])
{
  if (argc < 4) return 1;

  try
  {
    xerces_mgr               xerces_life;
    fsp::segment_queue       s_queue;
    std::vector<std::string> targets;
    std::string              xml_file = argv[1]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    std::string              xsd_file = argv[2]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    ;
    for (int i = 3; i < argc; ++i) targets.emplace_back(argv[i]); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)

    // Start Workers (jthread handles cleanup automatically)
    std::vector<std::jthread> workers;
    workers.reserve(std::thread::hardware_concurrency());
    for (unsigned int i = 0; i < std::thread::hardware_concurrency(); ++i) //
    {
      workers.emplace_back(worker_proc, std::ref(s_queue));
    }

    std::unique_ptr<xercesc::SAX2XMLReader, XercesDeleter> parser(xercesc::XMLReaderFactory::createXMLReader());

    // Setup validation
    parser->setFeature(xercesc::XMLUni::fgSAX2CoreValidation, true); // NOLINT(hicpp-no-array-decay)
    parser->setFeature(xercesc::XMLUni::fgXercesSchema, true);       // NOLINT(hicpp-no-array-decay)

    fsp::x_str xsd_path(xsd_file);
    parser->setProperty(xercesc::XMLUni::fgXercesSchemaExternalNoNameSpaceSchemaLocation, // NOLINT(hicpp-no-array-decay)
                        static_cast<void*>(xsd_path.to_u16string().data()));

    fsp::Handler handler(targets, s_queue);
    parser->setContentHandler(&handler);
    parser->setErrorHandler(&handler);

    parser->parse(xml_file.data());
    s_queue.set_finished();
  }
  catch (const std::exception& e)
  {
    std::cerr << "Runtime Error: " << e.what() << "\n";
    return 1;
  }
  catch (...)
  {
    return 1;
  }

  return 0;
}
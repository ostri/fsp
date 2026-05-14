#include <iostream>
#include <string>
#include <vector>
#include <thread>
// #include <memory>
#include "pugixml.hpp"
#include "x_str.hpp"
#include "queue.hpp"
#include "handler.hpp"
#include "xerces_mgr.hpp"
#include "mmap_file.hpp"
// #include <array>

namespace
{
  struct XercesDeleter
  {
    void operator()(xercesc::SAX2XMLReader* p) const
    {
      delete p; // NOLINT(cppcoreguidelines-owning-memory)
    }
  };


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
  const std::vector<std::string> args(argv, argv + argc); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  try
  {
    fsp::xerces_mgr          xerces_life;
    fsp::segment_queue       s_queue;
    std::vector<std::string> targets;
    const auto&              xml_file = args[1];
    const auto&              xsd_file = args[2];

    for (int i = 3; i < argc; ++i) targets.emplace_back(args[i]);

    // Start Workers (jthread handles cleanup automatically)
    std::vector<std::jthread> workers;
    workers.reserve(std::thread::hardware_concurrency());
    for (unsigned int i = 0; i < std::thread::hardware_concurrency(); ++i) //
    {
      workers.emplace_back(worker_proc, std::ref(s_queue));
    }

    std::unique_ptr<xercesc::SAX2XMLReader, XercesDeleter> parser(xercesc::XMLReaderFactory::createXMLReader());

    // NOLINTBEGIN(hicpp-no-array-decay)
    // Setup validation
    parser->setFeature(xercesc::XMLUni::fgSAX2CoreValidation, true);        // perform core validation
    parser->setFeature(xercesc::XMLUni::fgXercesSchema, true);              // use xsd scheme
    parser->setFeature(xercesc::XMLUni::fgXercesCalculateSrcOfs, true);     // enable locator object
    parser->setFeature(xercesc::XMLUni::fgSAX2CoreNameSpacePrefixes, true); // xmlns as attribute
    parser->setProperty(xercesc::XMLUni::fgXercesSchemaExternalNoNameSpaceSchemaLocation,
                        static_cast<void*>(fsp::x_str(xsd_file).to_u16string().data()));
    // NOLINTEND(hicpp-no-array-decay)

    fsp::Handler handler(targets, s_queue);
    parser->setContentHandler(&handler);
    parser->setErrorHandler(&handler);

    parser->parse(xml_file.data());
    s_queue.set_finished();
  }
  catch (const std::exception& e)
  {
    std::cerr << "Runtime Error: '" << e.what() << "'\n";
    return 1;
  }
  catch (...)
  {
    return 1;
  }

  return 0;
}
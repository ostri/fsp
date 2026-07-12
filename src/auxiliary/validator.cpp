#include "validator.hpp"
#include "x_str.hpp"
#include "xerces_mgr.hpp"
#include <fmt/base.h>
#include <memory>
#include <xercesc/sax2/SAX2XMLReader.hpp>
#include <xercesc/sax2/XMLReaderFactory.hpp>
#include <xercesc/sax2/DefaultHandler.hpp>
#include <xercesc/util/XMLString.hpp>
#include <xercesc/util/PlatformUtils.hpp>
#include <xercesc/framework/XMLGrammarPoolImpl.hpp>
#include <xercesc/framework/LocalFileInputSource.hpp>
#include <xercesc/validators/common/Grammar.hpp>
#include <xercesc/sax/ErrorHandler.hpp>
#include <xercesc/sax/SAXParseException.hpp>

#include <latch>
#include <atomic>
#include <vector>
#include <string>
#include <thread>
#include <chrono>

// using namespace xercesc;
namespace
{


  /**
   * @brief create sax reader for validator with assigned grammar pool
   *
   * @param gr_pool grammar pool
   * @return sax reader
   */
  fsp::sax_reader_t prepare_parser(const fsp::gr_pool_t& gr_pool)
  {
    // Create the parser ONCE outside the loop
    std::unique_ptr<xercesc::SAX2XMLReader> reader(
      xercesc::XMLReaderFactory::createXMLReader(xercesc::XMLPlatformUtils::fgMemoryManager, gr_pool.get()));
    // NOLINTBEGIN(hicpp-no-array-decay)
    reader->setFeature(xercesc::XMLUni::fgSAX2CoreValidation, true);
    reader->setFeature(xercesc::XMLUni::fgSAX2CoreNameSpaces, true);
    reader->setFeature(xercesc::XMLUni::fgXercesSchema, true);
    reader->setFeature(xercesc::XMLUni::fgXercesSchemaFullChecking, false);
    reader->setFeature(xercesc::XMLUni::fgXercesUseCachedGrammarInParse, true);
    // NOLINTEND(hicpp-no-array-decay)
    return reader;
  }
  // Static function for validating XML files
  static void validate_xml([[maybe_unused]] const std::stop_token& st,
                           const fsp::gr_pool_t&                   gr_pool,
                           std::latch&                             gr_latch,
                           std::atomic<bool>&                      gr_loaded,
                           const fsp::vec_str_t&                   xml_files)
  {
    gr_latch.wait();
    if (! gr_loaded) throw std::runtime_error("Grammar is not loaded. Validation aborted.");
    auto               reader = prepare_parser(gr_pool);
    fsp::valid_handler eh;
    reader->setErrorHandler(&eh);
    for (const auto& xml_file : xml_files)
    {
      auto start = std::chrono::high_resolution_clock::now();
      try
      {
        eh.reset(); // Reset error state for the new file
        eh.set_file(xml_file);
        XMLCh*                        xmlPath = xercesc::XMLString::transcode(xml_file.c_str());
        xercesc::LocalFileInputSource xmlSource(xmlPath);
        xercesc::XMLString::release(&xmlPath);
        reader->parse(xmlSource);
        std::chrono::duration<double, std::milli> elapsed = std::chrono::high_resolution_clock::now() - start;
        if (! eh.is_error()) fmt::print("File '{}' is valid. ({:.2f}ms)\n", xml_file, elapsed.count());
        else fmt::print("File '{}' is invalid. ({:.2f}ms)\n", xml_file, elapsed.count());
      }
      catch (const xercesc::XMLException& e)
      {
        fmt::print("Validation xerces exception '{}': {}\n", xml_file, xercesc::XMLString::transcode(e.getMessage()));
      }
      catch (const std::exception& e)
      {
        fmt::print("Validation standard exception '{}': {}\n", xml_file, e.what());
      }
      catch (...)
      {
        fmt::print("Validation unknown exception '{}'.\n", xml_file);
      }
    }
  }
  inline int help(const char** argv)
  {
    fmt::print("Usage: <grammar.xsd> <xml1> [xml2 ...]\n", *argv);
    return 1;
  }
} // namespace
int main(int argc, const char* argv[])
{
  if (argc < 3) return help(argv);
  try
  {
    fsp::xerces_mgr   x;                                                   // xercess environment
    fsp::gr_pool_t    gp(std::make_unique<xercesc::XMLGrammarPoolImpl>()); // std::make_shared<xercesc::XMLGrammarPoolImpl>();
    std::latch        gr_latch(1);                                         // just waiting for grammar to be loaded
    std::atomic<bool> gr_loaded{false};                                    // is grammar loaded?
    fsp::str_t        xsd_file = argv[1];                                  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    fsp::vec_str_t    xml_files(argv + 2, argv + argc);                    // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)

    std::jthread loader(fsp::load_grammar, std::ref(gp), std::ref(gr_latch), std::ref(gr_loaded), xsd_file);
    std::jthread validator(validate_xml, std::ref(gp), std::ref(gr_latch), std::ref(gr_loaded), std::cref(xml_files));
    // waiting for threads to finish;
    loader.join();
    validator.join();
    gp.reset();
    return 0;
  }
  catch (const std::runtime_error& e)
  {
    fmt::print("runtime error: {}", e.what());
    return 2;
  }
  catch (...)
  {
    fmt::print("Unhandled program exception");
    return 1;
  };
}
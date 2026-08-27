#include "xsd_validator.hpp"
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
#include <string>
#include <thread>
#include <chrono>

namespace fsp
{

  /**
   * @brief create sax reader for validator with assigned grammar pool
   *
   * @param gr_pool grammar pool
   * @return sax reader
   */
  sax_reader_t parser_for_validation(const fsp::gr_pool_t& gr_pool)
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
    reader->setFeature(xercesc::XMLUni::fgXercesValidationErrorAsFatal, true);
    reader->setFeature(xercesc::XMLUni::fgSAX2CoreNameSpacePrefixes, false);
    reader->setFeature(xercesc::XMLUni::fgXercesCalculateSrcOfs, false);
    reader->setFeature(xercesc::XMLUni::fgXercesCacheGrammarFromParse, false);
    // NOLINTEND(hicpp-no-array-decay)
    return reader;
  }
  // Static function for validating XML files
  void validate_xml([[maybe_unused]] const std::stop_token& st,
                    const fsp::gr_pool_t&                   gr_pool,
                    std::latch&                             gr_latch,
                    std::atomic<bool>&                      gr_loaded,
                    const fsp::vec_str_t&                   xml_files)
  {
    gr_latch.wait();
    if (! gr_loaded) throw std::runtime_error("Grammar is not loaded. Validation aborted.");
    auto               reader = parser_for_validation(gr_pool);
    fsp::valid_handler eh;
    reader->setErrorHandler(&eh);
    for (const auto& xml_file : xml_files)
    {
      auto start = std::chrono::high_resolution_clock::now();
      try
      {
        eh.reset(); // Reset error state for the new file
        eh.set_file(xml_file);
        XMLCh*                              xmlPath = xercesc::XMLString::transcode(xml_file.c_str());
        const xercesc::LocalFileInputSource xmlSource(xmlPath);
        xercesc::XMLString::release(&xmlPath);
        reader->parse(xmlSource);
        const std::chrono::duration<double, std::milli> elapsed = std::chrono::high_resolution_clock::now() - start;
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

} // namespace fsp
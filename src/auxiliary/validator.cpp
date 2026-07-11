#include <fmt/base.h>
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
#include <memory>

// using namespace xercesc;
namespace
{
  class ValidatorErrorHandler : public xercesc::DefaultHandler
  {
  public:
    void reset() { had_error = false; }

    void error(const xercesc::SAXParseException& exc) override
    {
      had_error = true;
      fmt::print("Napaka pri validaciji (vrstica {}, stolpec {}): {}\n",
                 exc.getLineNumber(),
                 exc.getColumnNumber(),
                 xercesc::XMLString::transcode(exc.getMessage()));
    }

    void fatalError(const xercesc::SAXParseException& exc) override
    {
      had_error = true;
      fmt::print("Fatalna napaka pri validaciji (vrstica {}, stolpec {}): {}\n",
                 exc.getLineNumber(),
                 exc.getColumnNumber(),
                 xercesc::XMLString::transcode(exc.getMessage()));
    }

    void warning(const xercesc::SAXParseException& exc) override
    {
      fmt::print("Opozorilo pri validaciji (vrstica '{}', stolpec {} ): {}\n",
                 exc.getLineNumber(),
                 exc.getColumnNumber(),
                 xercesc::XMLString::transcode(exc.getMessage()));
    }

    [[nodiscard]] bool is_error() const;
  private:
    bool had_error = false;
  };

  // Static function for loading the XSD schema
  static void load_grammar([[maybe_unused]] const std::stop_token&             st,
                           const std::shared_ptr<xercesc::XMLGrammarPoolImpl>& gr_pool,
                           std::latch&                                         gr_latch,
                           std::atomic<bool>&                                  gr_loaded,
                           const std::string&                                  xsd_file)
  {
    try
    {
      XMLCh*                        xsdPath = xercesc::XMLString::transcode(xsd_file.c_str());
      xercesc::LocalFileInputSource inputSource(xsdPath);
      xercesc::XMLString::release(&xsdPath);

      // Using the previously fixed reader creation pattern
      std::unique_ptr<xercesc::SAX2XMLReader> reader(
        xercesc::XMLReaderFactory::createXMLReader(xercesc::XMLPlatformUtils::fgMemoryManager, gr_pool.get()));
      // NOLINTBEGIN(hicpp-no-array-decay)
      reader->setFeature(xercesc::XMLUni::fgSAX2CoreValidation, true);
      reader->setFeature(xercesc::XMLUni::fgSAX2CoreNameSpaces, true);
      reader->setFeature(xercesc::XMLUni::fgXercesSchema, true);
      reader->setFeature(xercesc::XMLUni::fgXercesSchemaFullChecking, false);
      // NOLINTEND(hicpp-no-array-decay)

      xercesc::Grammar* grammar = reader->loadGrammar(inputSource, xercesc::Grammar::SchemaGrammarType, true);
      if (grammar != nullptr)
      {
        gr_pool->cacheGrammar(grammar);
        gr_loaded = true;
        fmt::print("Slovnica uspesno nalozena.\n");
      }
      else
      {
        fmt::print("Napaka: nalaganje slovnice ni uspelo (loadGrammar vrnil nullptr).\n");
      }
    }
    catch (const xercesc::XMLException& e)
    {
      fmt::print("Izjema pri nalaganju slovnice: '{}'.", xercesc::XMLString::transcode(e.getMessage()));
    }
    catch (const std::exception& e)
    {
      fmt::print("Standardna izjema pri nalaganju slovnice: '{}'\n'", e.what());
    }
    catch (...)
    {
      fmt::print("Neznana izjema pri nalaganju slovnice.\n");
    }

    // Decrease latch count to unblock the validator thread
    gr_latch.count_down();
  }

  // Static function for validating XML files
  static void validate_xml([[maybe_unused]] const std::stop_token&             st,
                           const std::shared_ptr<xercesc::XMLGrammarPoolImpl>& gr_pool,
                           std::latch&                                         gr_latch,
                           std::atomic<bool>&                                  gr_loaded,
                           const std::vector<std::string>&                     xml_files)
  {
    gr_latch.wait();

    if (! gr_loaded)
    {
      fmt::print("Slovnica ni bila nalozena, validacija se ne bo izvedla.\n");
      return;
    }

    for (const auto& xml_file : xml_files)
    {
      try
      {
        std::unique_ptr<xercesc::SAX2XMLReader> reader(
          xercesc::XMLReaderFactory::createXMLReader(xercesc::XMLPlatformUtils::fgMemoryManager, gr_pool.get()));
        // NOLINTBEGIN(hicpp-no-array-decay)
        reader->setFeature(xercesc::XMLUni::fgSAX2CoreValidation, true);
        reader->setFeature(xercesc::XMLUni::fgSAX2CoreNameSpaces, true);
        reader->setFeature(xercesc::XMLUni::fgXercesSchema, true);
        reader->setFeature(xercesc::XMLUni::fgXercesSchemaFullChecking, false);
        reader->setFeature(xercesc::XMLUni::fgXercesUseCachedGrammarInParse, true);
        // NOLINTEND(hicpp-no-array-decay)

        ValidatorErrorHandler errorHandler;
        reader->setErrorHandler(&errorHandler);

        XMLCh*                        xmlPath = xercesc::XMLString::transcode(xml_file.c_str());
        xercesc::LocalFileInputSource xmlSource(xmlPath);
        xercesc::XMLString::release(&xmlPath);

        reader->parse(xmlSource);

        if (! errorHandler.is_error()) fmt::print("Datoteka '{}' je veljavna.\n", xml_file);
        else fmt::print("Datoteka '{}' vsebuje napake.\n", xml_file);
      }
      catch (const xercesc::XMLException& e)
      {
        fmt::print("Izjema pri obdelavi '{}': {}\n'", xml_file, xercesc::XMLString::transcode(e.getMessage()));
      }
      catch (const std::exception& e)
      {
        fmt::print("Standardna izjema pri obdelavi '{}': {}\n", xml_file, e.what());
      }
      catch (...)
      {
        fmt::print("Neznana izjema pri obdelavi '{}'.\n", xml_file);
      }
    }
  }

  inline bool ValidatorErrorHandler::is_error() const { return had_error; }
} // namespace
int main(int argc, char* argv[])
{
  if (argc < 3)
  {
    fmt::print("Uporaba: <slovnica.xsd> <xml1> [xml2 ...]\n", *argv);
    return 1;
  }

  try
  {
    xercesc::XMLPlatformUtils::Initialize();
  }
  catch (const xercesc::XMLException& e)
  {
    fmt::print("Napaka pri inicializaciji Xerces: {}\n", xercesc::XMLString::transcode(e.getMessage()));
    return 1;
  }

  // Uporabimo shared_ptr za varno deljenje med nitmi
  auto gp = std::make_shared<xercesc::XMLGrammarPoolImpl>();

  std::latch        grammarLatch(1);
  std::atomic<bool> grammarLoaded{false};

  std::string              xsdFile = argv[1]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  std::vector<std::string> xmlFiles;
  for (int i = 2; i < argc; ++i) xmlFiles.emplace_back(argv[i]); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)

  std::jthread loader(load_grammar, gp, std::ref(grammarLatch), std::ref(grammarLoaded), xsdFile);
  std::jthread validator(validate_xml, gp, std::ref(grammarLatch), std::ref(grammarLoaded), std::cref(xmlFiles));
  // Počakamo, da se obe niti zaključita
  loader.join();
  validator.join();
  gp.reset();
  xercesc::XMLPlatformUtils::Terminate();
  return 0;
}
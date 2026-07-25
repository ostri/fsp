#include "validate.hpp"
#include "x_str.hpp"
#include "xerces_mgr.hpp"
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <memory>

namespace fsp
{
  EH::EH(bool quiet)
  : quietMode(quiet)
  {
  }

  void EH::warning(const xercesc::SAXParseException& exc) { handleError("Warning", exc); }
  void EH::error(const xercesc::SAXParseException& exc)
  {
    hasErrors = true;
    handleError("Error", exc);
  }

  void EH::fatalError(const xercesc::SAXParseException& exc)
  {
    hasErrors = true;
    handleError("Fatal error", exc);
  }

  void                             EH::resetErrors() { hasErrors = false; }
  [[nodiscard]] bool               EH::hasValidationErrors() const { return hasErrors; }
  void                             EH::handleError(const char* type, const xercesc::SAXParseException& exc)
  {
    // Convert from UTF-16 to UTF-8 for console output
    auto system_id = fsp::x_str(exc.getSystemId()).to_string_view();
    auto message   = fsp::x_str(exc.getMessage()).to_string_view();

    lastErrorLocation = fsp::x_str(system_id).to_string_view();
    lastErrorMessage  = fsp::x_str(message).to_string_view();

    if (! quietMode)
    {
      std::cerr << type << " in file " << system_id << " at line " << exc.getLineNumber() << ", column " << exc.getColumnNumber() << ": '"
                << lastErrorMessage << "' xerces err:'" << lastErrorLocation << "'\n";
    }
  }

  // Function to validate XML against XSD
  bool validateXML(const std::string& xmlFile, const std::string& xsdFile, bool quietMode)
  {
    // Check if files exist using std::filesystem
    if (! std::filesystem::exists(xmlFile))
    {
      if (! quietMode) { std::cerr << "Error: XML file '" << xmlFile << "' does not exist." << "\n"; }
      return false;
    }

    if (! std::filesystem::exists(xsdFile))
    {
      if (! quietMode) { std::cerr << "Error: XSD file '" << xsdFile << "' does not exist." << "\n"; }
      return false;
    }

    // Create parser
    auto parser = std::make_unique<xercesc::XercesDOMParser>(); // TODO: bug 1 - use SAX parser
    // Configure schema validation
    parser->setValidationScheme(xercesc::XercesDOMParser::Val_Always);
    parser->setDoNamespaces(true);
    parser->setDoSchema(true);
    parser->setValidationSchemaFullChecking(true);
    parser->setHandleMultipleImports(true);
    parser->setLoadExternalDTD(false);
    parser->useCachedGrammarInParse(true);
    try
    {
      parser->loadGrammar(xsdFile.c_str(), xercesc::Grammar::SchemaGrammarType,
                          true); // true = naloži tudi imported sheme
    }
    catch (const xercesc::XMLException& e)
    {
      auto msg = fsp::x_str(e.getMessage()).to_string_view();
      if (! quietMode) std::cerr << "Failed to load grammar: '" << msg << "\n";
      return false;
    }
    // Create error handler
    EH errorHandler(quietMode);
    parser->setErrorHandler(&errorHandler);
    // Load and validate XML file
    try
    {
      parser->parse(xmlFile.c_str());
      return ! errorHandler.hasValidationErrors();
    }
    catch (const xercesc::XMLException& e)
    {
      auto msg = fsp::x_str(e.getMessage()).to_string_view();
      if (! quietMode) { std::cerr << "XML Exception: '" << msg << "'\n"; }
      return false;
    }
    catch (const xercesc::SAXParseException& e)
    {
      auto msg       = fsp::x_str(e.getMessage()).to_string_view();
      auto system_id = fsp::x_str(e.getSystemId()).to_string_view();
      if (! quietMode)
      {
        std::cerr << "Parse error in '" << system_id << "' line: " << e.getLineNumber() << " column: " << e.getColumnNumber() << "-> '"
                  << msg << "'\n";
      }
      return false;
    }
    catch (const xercesc::SAXException& e)
    {
      auto msg = fsp::x_str(e.getMessage()).to_string_view();
      if (! quietMode) { std::cerr << "SAX Exception: '" << msg << "'\n"; }
      return false;
    }
    catch (...)
    {
      if (! quietMode) { std::cerr << "Unknown exception during validation.\n"; }
      return false;
    }
  }

  // Print usage information
  void printUsage(const char* programName)
  {
    std::cerr << "Usage: " << programName << " <xml_file> <xsd_file> [-q|--quiet]" << "\n";
    std::cerr << "\n";
    std::cerr << "Options:" << "\n";
    std::cerr << "  -q, --quiet    Quiet mode (no output to stdout/stderr)" << "\n";
    std::cerr << "\n";
    std::cerr << "Return codes:" << "\n";
    std::cerr << "  0 - XML file is valid according to XSD schema" << "\n";
    std::cerr << "  1 - XML file is invalid or an error occurred" << "\n";
  }
}; // namespace fsp
int main(int argc, char* argv[])
{
  // Validate argument count
  if (argc < 3 || argc > 4)
  {
    fsp::printUsage(argv[0]); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return 1;
  }

  std::string xmlFile   = argv[1]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  std::string xsdFile   = argv[2]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  bool        quietMode = false;

  // Check optional mode parameter
  if (argc == 4)
  {
    std::string_view mode = argv[3]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (mode == "-q" || mode == "--quiet") { quietMode = true; }
    else
    {
      std::cerr << "Error: Invalid mode parameter '" << mode << "'" << "\n";
      fsp::printUsage(argv[0]); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      return 1;
    }
  }

  // Initialize Xerces
  try
  {
    fsp::xerces_mgr initializer;

    // Perform validation
    bool isValid = fsp::validateXML(xmlFile, xsdFile, quietMode);

    // Print result (except in quiet mode)
    if (! quietMode)
    {
      if (isValid) { std::cout << "valid" << "\n"; }
      else
      {
        std::cout << "invalid" << "\n";
      }
    }

    // Return appropriate exit code
    return isValid ? 0 : 1;
  }
  catch (const std::exception& e)
  {
    if (! quietMode) { std::cerr << "Error: " << e.what() << "\n"; }
    return 1;
  }
  catch (...)
  {
    if (! quietMode) { std::cerr << "Unknown error during initialization." << "\n"; }
    return 1;
  }
}

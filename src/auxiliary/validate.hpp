#pragma once

// #include <iostream>
#include <string>
// #include <string_view>
// #include <memory>

#include <xercesc/parsers/XercesDOMParser.hpp>
#include <xercesc/dom/DOMDocument.hpp>
#include <xercesc/framework/LocalFileInputSource.hpp>
#include <xercesc/framework/MemBufInputSource.hpp>
#include <xercesc/sax/SAXParseException.hpp>
#include <xercesc/validators/schema/SchemaGrammar.hpp>
#include <xercesc/framework/psvi/XSModel.hpp>
#include <xercesc/validators/common/GrammarResolver.hpp>
#include <xercesc/validators/schema/SchemaValidator.hpp>
#include <xercesc/util/PlatformUtils.hpp>
#include <xercesc/util/XMLString.hpp>
#include <xercesc/util/XMLUni.hpp>
#include <xercesc/sax/ErrorHandler.hpp>
#include <xercesc/util/TranscodingException.hpp>

namespace fsp
{
  // Error handler for validation with UTF-8 support
  class ValidationErrorHandler : public xercesc::ErrorHandler
  {
  private:
    bool        hasErrors{};
    bool        quietMode;
    std::string lastErrorLocation;
    std::string lastErrorMessage;
  public:
    explicit ValidationErrorHandler(bool quiet);

    void warning(const xercesc::SAXParseException& exc) override;

    void error(const xercesc::SAXParseException& exc) override;

    void fatalError(const xercesc::SAXParseException& exc) override;

    void resetErrors() override;

    [[nodiscard]] bool hasValidationErrors() const;

    [[nodiscard]] const std::string& getLastErrorLocation() const;

    [[nodiscard]] const std::string& getLastErrorMessage() const;
  private:
    void handleError(const char* type, const xercesc::SAXParseException& exc);
  };

  bool validateXML(const std::string& xmlFile, const std::string& xsdFile, bool quietMode);

  void printUsage(const char* programName);
}; // namespace fsp

// int main(int argc, char* argv[]);

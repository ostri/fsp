#pragma once

#include <spdlog/logger.h>
#include <string>
#include <vector>

// [DODANO] future/optional za sprejem validacijske napake iz validacijske niti
#include <future>
#include <optional>

#include <xercesc/dom/DOMLocator.hpp>
#include <xercesc/sax/Locator.hpp>
#include <xercesc/sax2/Attributes.hpp>
#include <xercesc/sax2/DefaultHandler.hpp>
#include <xercesc/sax2/SAX2XMLReader.hpp>

// #include "common.hpp"
#include "e_tag_wide.hpp"
#include "error_info.hpp"
#include "logger.hpp"
#include "parsing_util.hpp"
#include "queue.hpp"
#include "x_str.hpp"
// #include "x_str.hpp"

namespace fsp
{
  using cstr_t       = std::string_view;
  using cstr_XMLCh_t = std::basic_string_view<XMLCh>;
  using ns_def_t     = std::vector<std::pair<x_str, x_str>>;


  class Handler : public xercesc::DefaultHandler
  {
  public:
    Handler(proc_data&                    targets,
            segment_queue&                queue,
            const fsp_logger&             logger,
            const xercesc::SAX2XMLReader* parser,
            std::string_view              base_addr);

    // --- SAX2 ContentHandler ---
    void startPrefixMapping(const XMLCh* prefix, const XMLCh* uri) override;
    // void endPrefixMapping() override;
    void startElement(const XMLCh* uri, const XMLCh* localname, const XMLCh* qname, const xercesc::Attributes& attrs) override;
    void endElement( //
      [[maybe_unused]] const XMLCh* uri,
      [[maybe_unused]] const XMLCh* localname,
      [[maybe_unused]] const XMLCh* qname) override;
    void characters([[maybe_unused]] const XMLCh* chars, [[maybe_unused]] XMLSize_t length) override { };
    // --- SAX2 ErrorHandler ---
    void warning(const xercesc::SAXParseException& e) override;
    void error(const xercesc::SAXParseException& e) override;
    void fatalError(const xercesc::SAXParseException& e) override;

    [[nodiscard]] std::size_t      segments_found() const noexcept { return counter_; }
    [[nodiscard]] std::string_view base_addr() const;

    // [DODANO] Injicira shared_future iz xml_processor::process_from_buffer.
    // Handler ga polling preverja v startElement() in ob napaki vrže
    // SAXParseException, ki jo Xerces uporabi kot signal za prekinitev parsinga.
    // shared_future (ne future) ker get() ne sme biti destructive — handler ga
    // lahko preveri večkrat (polling), xml_processor pa pokliče get() na koncu.
    void set_validation_future(std::shared_future<std::optional<error_info>> f) { val_future_ = std::move(f); }
  private:
    // --- NS context stack ---
    // Vsak nivo je map prefix→uri za en XML element scope.
    // open_ns_scope() potisne nov nivo, close_ns_scope() ga odstrani.
    void               open_ns_scope();
    void               close_ns_scope();
    void               push_ns_mapping(const XMLCh* prefix, const XMLCh* uri);
    [[nodiscard]] bool is_capturing() const { return frag_depth_ != -1; }
    // Razreši prefix v URI. Prazen string če prefix ni znan.
    [[nodiscard]] x_str resolve_ns(const x_str& prefix) const noexcept;
    // Vrne snapshot vseh aktivnih NS preslikav (za vbrizganje v fragment).
    [[nodiscard]] ns_def_t active_ns() const; // FIXME ostri - should be vector
    // Razreši NS URI za e_tag (enkrat, ko je NS context zgrajen).
    [[nodiscard]] bool tag_matches(const e_tag_wide& tag, const XMLCh* local_name, const XMLCh* ns_uri) const noexcept;
    std::string        make_open_tag(const XMLCh* qname, const xercesc::Attributes& attrs);

    // // --- Xerces string helpers ---
    // static std::string to_str(const XMLCh* xstr);
    // static std::string to_str(const XMLCh* xstr, XMLSize_t len);
    /// prepare message to report exception
    std::string prepare_msg(const xercesc::SAXParseException& e);
    // --- subtree xpaths ---
    proc_data                 targets_;      // xpath rules
    std::vector<xpath_wide_t> targets_wide_; // xpath rules as XMLCh* strings

    // --- NS definicitons ---
    // stack of NS definitions. Whenever new set of ns definitions occur new push on stack
    // occurs. on endElement of the same tag this is removed.
    struct ns_level
    {
      int      depth;
      ns_def_t ns_vec;
    };
    std::vector<ns_level> ns_stack_;
    // the ns_pending_structure is a temporary buffer that transfers information between
    // methods startPrefixMapping and startElement
    ns_def_t ns_pending_;
    // --- Matching state ---
    std::vector<int> matched_;
    int              doc_depth_ = 0; // globina v dokumentu (1 = koreni elem.)
    // --- helper methods ---------
    void check_validation_status();
    void check_xpath_matches( //
      const XMLCh*               uri,
      const XMLCh*               localname,
      const XMLCh*               qname,
      const xercesc::Attributes& attrs);

    // --- Fragment akumulacija ---
    // bool capturing_  = false;
    int frag_depth_ = -1; // depth inside the fragment
    int active_idx_ = -1; // which subtry type we are processing
    // std::string fragment_;              // akumuliran XML fragment
    std::size_t frag_start_offset_ = 0; // byte offset of start of the fragment

    // --- Output ---
    [[maybe_unused]] segment_queue& queue_;
    std::size_t                     counter_ = 0;

    const xercesc::SAX2XMLReader* parser_; // reference to parser; for getSrcOffs
    // std::shared_ptr<spdlog::logger> logger_;    /// logger
    const fsp_logger& log_;       // logger
    std::string_view  base_addr_; // address of the start of the document
    // TODO: ostri - ostri check if prefix_ is really related to handler
    std::string prefix_; // opening tag with inherited ns, ns and attributes

    // [DODANO] Shared future na katerega validacijska nit postavi napako (ali nullopt).
    // Handler ga polling preverja v startElement() brez blokiranja.
    // Inicializiran kot neveljaven (valid() == false) — brez validacije se ne
    // preveri nikoli in ne povzroča overhead-a.
    std::shared_future<std::optional<error_info>> val_future_;

    // [DODANO] Števec za redčenje polling preverjanj validacijskega future-a.
    // Preverjanje se izvede vsakih 2^10 (1024) elementov z bitno masko,
    // kar je zanemarljiva cena pri 1M transakcijah (~1000 preverjanj skupaj).
    std::size_t element_counter_ = 0;
    std::string buf_; // space for "make_open_tag"
  };

} // namespace fsp

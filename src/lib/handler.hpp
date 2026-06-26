#pragma once

// #include <spdlog/logger.h>
#include <string>
#include <vector>
#include <string_view>

// [DODANO] future/optional za sprejem validacijske napake iz validacijske niti
#include <future>
#include <optional>

#include <xercesc/dom/DOMLocator.hpp>
#include <xercesc/sax/Locator.hpp>
#include <xercesc/sax2/Attributes.hpp>
#include <xercesc/sax2/DefaultHandler.hpp>
#include <xercesc/sax2/SAX2XMLReader.hpp>

#include "e_tag_wide.hpp"
#include "error_info.hpp"
#include "logger.hpp"
#include "parsing_util.hpp"
#include "queue.hpp"
#include "x_str.hpp"

namespace fsp
{
  using cstr_t       = std::string_view;
  using str_t        = std::string;
  using cstr_XMLCh_t = std::basic_string_view<XMLCh>;
  using ns_def_t     = std::vector<std::pair<x_str, x_str>>;
  using RuleMask     = uint64_t;

  class Handler : public xercesc::DefaultHandler
  {
  public:
    Handler(proc_data&                    targets,
            segment_queue&                queue,
            const fsp_logger&             log,
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
    // void characters([[maybe_unused]] const XMLCh* chars, [[maybe_unused]] XMLSize_t length) override { };
    //  --- SAX2 ErrorHandler ---
    void warning(const xercesc::SAXParseException& e) override;
    void error(const xercesc::SAXParseException& e) override;
    void fatalError(const xercesc::SAXParseException& e) override;

    [[nodiscard]] std::size_t segments_found() const noexcept;
    [[nodiscard]] cstr_t      base_addr() const;
    // [DODANO] Injicira shared_future iz xml_processor::process_from_buffer.
    // Handler ga polling preverja v startElement() in ob napaki vrže
    // SAXParseException, ki jo Xerces uporabi kot signal za prekinitev parsinga.
    // shared_future (ne future) ker get() ne sme biti destructive — handler ga
    // lahko preveri večkrat (polling), xml_processor pa pokliče get() na koncu.
    void set_validation_future(std::shared_future<std::optional<error_info>> f);
  private: // methods
    [[noreturn]] void logic_error(const char* msg) const;
    // --- helper methods ---------
    void check_validation_status();
    void check_xpath_matches( //
      const XMLCh*               uri,
      const XMLCh*               localname,
      const XMLCh*               qname,
      const xercesc::Attributes& attrs);
    // --- NS context stack ---
    // Vsak nivo je map prefix→uri za en XML element scope.
    // open_ns_scope() potisne nov nivo, close_ns_scope() ga odstrani.
    void               open_ns_scope();
    void               close_ns_scope();
    void               push_ns_mapping(const XMLCh* prefix, const XMLCh* uri);
    [[nodiscard]] bool is_capturing() const { return frag_depth_ != -1; }
    // Translate prefix to uri. Empty string if prefix is not defined
    [[nodiscard]] x_str resolve_ns(const x_str& prefix) const noexcept;
    // Razreši NS URI za e_tag (enkrat, ko je NS context zgrajen).
    [[nodiscard]] bool tag_matches(const e_tag_wide& tag, const XMLCh* local_name, const XMLCh* ns_uri) const noexcept;
    std::string        make_open_tag(const XMLCh* qname, const xercesc::Attributes& attrs);
    /// prepare message to report exception
    std::string prepare_msg(const xercesc::SAXParseException& e);
    void        rebuild_ns_decl_for_current_level();
  private: /// members
    // --- subtree xpaths ---
    proc_data                 targets_;      // xpath rules
    std::vector<xpath_wide_t> targets_wide_; // xpath rules as XMLCh* strings

    std::vector<RuleMask>    active_mask_stack_;  //< stack of active rules
    std::vector<std::size_t> rule_lengths_;       //< length of the xpaths /rules
    RuleMask                 all_rules_mask_ = 0; //< current rule mask

    // --- NS definicitons ---
    // stack of NS definitions. Whenever new set of ns definitions occur new push on stack
    // occurs. on endElement of the same tag this is removed.
    struct ns_level
    {
      int         depth;          // depth in the tree where these ns are valid
      ns_def_t    ns_vec;         // list of ns that are defined at this level
      std::string ns_decl_string; //< namespaces as string
      // public:
      //   ns_level(int d = -1, ns_def_t v = {}, std::string s = {})
      //   : depth(d)
      //   , ns_vec(std::move(v))
      //   , ns_decl_string(std::move(s))
      //   {
      //   }
    };
    std::vector<ns_level> ns_stack_;
    // the ns_pending_structure is a temporary buffer that transfers information between
    // methods startPrefixMapping and startElement
    ns_def_t ns_pending_;
    // // --- Matching state ---
    // std::vector<int> matched_;
    int doc_depth_ = 0; // depth in the document (1 = koreni elem.)
    // --- Fragment akumulacija ---
    int         frag_depth_        = -1; // depth inside the fragment
    int         target_type_       = -1; // which subtry type we are processing
    std::size_t frag_start_offset_ = 0;  // byte offset of start of the fragment

    // --- Output ---
    [[maybe_unused]] segment_queue& queue_; // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
    std::size_t                     counter_ = 0;

    const xercesc::SAX2XMLReader* parser_;    // reference to parser; for getSrcOffs
    const fsp_logger&             log_;       // logger NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
    std::string_view              base_addr_; // address of the start of the document
    std::string                   prefix_;    // opening tag with inherited ns, ns and attributes

    // [DODANO] Shared future na katerega validacijska nit postavi napako (ali nullopt).
    // Handler ga polling preverja v startElement() brez blokiranja.
    // Inicializiran kot neveljaven (valid() == false) — brez validacije se ne
    // preveri nikoli in ne povzroča overhead-a.
    std::shared_future<std::optional<error_info>> val_future_;
    std::size_t                                   element_counter_ = 0; // pooling counter check also "every"
    std::string                                   buf_;                 // space for "make_open_tag"
    bool                                          log_trace_       = false;
    bool                                          log_debug_       = false;
    bool                                          log_info_        = false;
    bool                                          log_warn_        = false;
    bool                                          log_err_         = false;
    bool                                          log_crit_        = false;
    int                                           max_xpath_depth_ = 0;
  };

  inline std::size_t Handler::segments_found() const noexcept { return counter_; }
  inline cstr_t      Handler::base_addr() const { return base_addr_; }
  inline void        Handler::set_validation_future(std::shared_future<std::optional<error_info>> f) { val_future_ = std::move(f); }

} // namespace fsp

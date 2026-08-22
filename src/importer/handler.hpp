#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <string_view>

#include <xercesc/sax2/Attributes.hpp>
#include <xercesc/sax2/DefaultHandler.hpp>
#include <xercesc/sax2/SAX2XMLReader.hpp>

#include "doc_set_dscr.hpp"
#include "e_tag_wide.hpp"
#include <logger/logger.hpp>
#include "parsing_util.hpp"
#include "x_str.hpp"
#include "xml_segment.hpp"
#include "segment_pool.hpp"

namespace fsp
{
  using cstr_t       = std::string_view;
  using str_t        = std::string;
  using cstr_XMLCh_t = std::basic_string_view<XMLCh>;
  using ns_def_t     = std::vector<std::pair<x_str, x_str>>;
  using RuleMask     = uint64_t;

  // Which xercesc::ErrorHandler callback most recently reported a problem -- error() means a
  // schema/validity constraint violation, fatalError() means a well-formedness violation (see
  // xercesc::ErrorHandler's own doc comments on each). Both callbacks throw the SAME
  // xercesc::SAXParseException type (see Handler::error()/fatalError() below), so this is the
  // ONLY signal that actually distinguishes syntax from schema at the doc_cutter::cut() catch
  // site -- point 15/16 of the design discussion this implements.
  enum class sax_error_source : std::uint8_t
  {
    none,
    validity,   // reported via error()
    well_formed // reported via fatalError()
  };

  class Handler : public xercesc::DefaultHandler
  {
  public:
    Handler(const proc_data&              targets, //
            const logger::Logger&         log,
            const xercesc::SAX2XMLReader* parser,
            segment_pool&                 pool,
            const doc_set_dscr&           ds_dscr,
            std::vector<bool>             is_header_seg_type = {});
    // --- SAX2 ContentHandler ---
    void startPrefixMapping(const XMLCh* prefix, const XMLCh* uri) override;
    void startElement(const XMLCh* uri, const XMLCh* localname, const XMLCh* qname, const xercesc::Attributes& attrs) override;
    void endElement( //
      [[maybe_unused]] const XMLCh* uri,
      [[maybe_unused]] const XMLCh* localname,
      [[maybe_unused]] const XMLCh* qname) override;
    //  --- SAX2 ErrorHandler ---
    void warning(const xercesc::SAXParseException& e) override;
    void error(const xercesc::SAXParseException& e) override;
    void fatalError(const xercesc::SAXParseException& e) override;

    [[nodiscard]] std::size_t segments_found() const noexcept;
    void                      set_doc(cstr_t doc);
    [[nodiscard]] cstr_t      doc() const;
    [[nodiscard]] int         doc_ndx() const;
    void                      set_doc_ndx(int doc_ndx);
    // When true, error()/fatalError() throw the SAXParseException right back out of parse()
    // instead of only logging -- used when this Handler's parser was set up with schema
    // validation enabled (see doc_cutter::setup_parser_with_validation()), so an invalid
    // document short-circuits cutting immediately instead of paying for the rest of it.
    void set_validating(bool validating) { validating_ = validating; }
    // Which of error()/fatalError() most recently reported a problem -- see sax_error_source's
    // own doc comment. Read by doc_cutter::cut() from its catch block, after the fact (this
    // Handler instance stays alive across the throw, owned by doc_cutter).
    [[nodiscard]] sax_error_source last_error_source() const noexcept { return last_error_source_; }
  private: // methods
    [[noreturn]] void logic_error(const char* msg) const;
    // --- helper methods ---------
    void check_xpath_matches(const XMLCh* uri,
                             const XMLCh* localname,
                             //                             const XMLCh*               qname, // it is not used to make it faster
                             const xercesc::Attributes& attrs);
    // --- NS context stack ---
    // Each level is a prefix→uri map for one XML element scope.
    // open_ns_scope() pushes a new level, close_ns_scope() removes it.
    void               open_ns_scope();
    void               close_ns_scope();
    void               push_ns_mapping(const XMLCh* prefix, const XMLCh* uri);
    [[nodiscard]] bool is_capturing() const { return frag_depth_ != -1; }
    // Resolves the NS URI for e_tag (once the NS context has been built).
    [[nodiscard]] bool tag_matches(const e_tag_wide& tag, const XMLCh* local_name, const XMLCh* ns_uri) const noexcept;
    str_t              make_open_tag(const XMLCh* qname, const xercesc::Attributes& attrs);
    str_XMLCh_t        attr_values_str(const xercesc::Attributes& attrs);
    /// prepare message to report exception
    str_t prepare_msg(const xercesc::SAXParseException& e);
    void  rebuild_ns_decl_for_current_level();
  private:                      /// members
    const logger::Logger& log_; // must be first logger NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
    // --- subtree xpaths ---
    proc_data                 targets_;      // xpath rules
    std::vector<xpath_wide_t> targets_wide_; // xpath rules as XMLCh* strings

    std::vector<RuleMask>    active_mask_stack_; //< stack of active rules
    std::vector<std::size_t> rule_lengths_;      //< length of the xpaths /rules

    // --- NS definicitons ---
    // stack of NS definitions. Whenever new set of ns definitions occur new push on stack
    // occurs. on endElement of the same tag this is removed.
    struct ns_level
    {
      int      depth;      //< depth in the tree where these ns are valid
      ns_def_t ns_vec;     //< list of ns that are defined at this level
                           //      str_t    ns_decl_string; //< namespaces as string
      str_XMLCh_t ns_decl; //< namespaces as XMLCh
    };
    std::vector<ns_level> ns_stack_; // stack of namespaces associated with xml tags
                                     // It is pushed on startElement tag where ns are provided
                                     // It is popped on endElement tag

    ns_def_t ns_pending_; // the ns_pending_structure is a temporary buffer that transfers information between
                          // methods startPrefixMapping and startElement. It is cleared after startElement
    int doc_depth_ = 0;   // depth in the document (1 = root elem.)
    // --- segment acumutate sdata ---
    int         frag_depth_        = -1;                //< depth inside the fragment
    int         seg_type_          = -1;                //< type/structure of the segment. document is split into segments.
    std::size_t frag_start_offset_ = 0;                 //< byte offset of start of the fragment
    std::size_t counter_           = 0;                 //< counter to obtain unique segment id within the file
                                                        //< (reset per document in set_doc_ndx() -- this Handler
                                                        //< is owned by one worker thread and reused across every
                                                        //< document that thread cuts, not one Handler per document)
    const xercesc::SAX2XMLReader* parser_;              //< pointer to related parser; only for getSrcOffs, not owner
    cstr_t                        doc_;                 //< xml document mapped as string view over mmap file
    const doc_set_dscr&           ds_dscr_;             //< structure of all documents to be processed
    int                           doc_ndx_ = -1;        //< index of the document within the ds_dscr global structure
    str_XMLCh_t                   ns_;                  //< current and inherited namespaces as string (for current segment)
    str_XMLCh_t                   attr_;                //< current tag attributes as a string (for current segment)
    std::size_t                   element_counter_ = 0; //< pooling counter check also "every"
    str_XMLCh_t                   buf_;                 //< space for "make_open_tag" as XMLCh
    segment_pool&                 pool_;                //< segment pool
    // Indexed by seg_type() (== subtree_type()) -- true means that schema class derives from
    // fsp::hdr_seg_schema (see reflection.hpp and proc_data::is_header's own doc comments), so
    // endElement() routes segments of that type into pool_.push_ready_header() instead of
    // pool_.push_ready() (see its own doc comment). Empty when the reflected namespace declares no
    // hdr_seg_schema-derived class at all, in which case index-out-of-range never happens because
    // endElement() only ever indexes this when it's non-empty -- see its own check.
    std::vector<bool> is_header_seg_type_;
    bool              validating_        = false;                  //< see set_validating()
    sax_error_source  last_error_source_ = sax_error_source::none; //< see last_error_source()
    const bool        log_trace_         = log_.active(logger::level::trace);
    const bool        log_debug_         = log_.active(logger::level::debug);
    const bool        log_info_          = log_.active(logger::level::info);
    const bool        log_warn_          = log_.active(logger::level::warn);
    const bool        log_err_           = log_.active(logger::level::error);
    const bool        log_crit_          = log_.active(logger::level::critical);
    int               max_xpath_depth_   = 0; //< max depth of all xpaths
  };
  /////////////////////////////////////////////////////////////////////////////////////////////////
  inline std::size_t Handler::segments_found() const noexcept { return counter_; }
  inline void        Handler::set_doc(cstr_t doc) { doc_ = doc; }
  inline cstr_t      Handler::doc() const { return doc_; }
  inline int         Handler::doc_ndx() const { return doc_ndx_; }
  inline void        Handler::set_doc_ndx(int doc_ndx)
  {
    doc_ndx_ = doc_ndx;
    counter_ = 0;
  }
} // namespace fsp

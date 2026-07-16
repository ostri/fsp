#include "xml_attr.hpp"
#include "xpath_set.hpp"
#include <libxml/parser.h>
#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>
#include <string>
#include <span>
#include <bit>
#include <cstdint>

namespace fsp
{

  struct xml_path_el
  { // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::string_view uri;
    std::string_view tag;
    bool             operator==(const xml_path_el& other) const { return tag == other.tag && uri == other.uri; }
    // NOLINTEND(misc-non-private-member-variables-in-classes)
  };

  struct sax_ctx
  {                                                // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::shared_ptr<xpath_set>            targets; // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
    std::vector<xml_path_el>              path_stack;
    std::vector<std::vector<std::string>> results;
    std::uint64_t                         remaining_mask    = 0;
    bool                                  stop_parsing      = false;
    int                                   active_target_idx = -1;
    xmlParserCtxtPtr                      ctxt              = nullptr; // to stop the parser
    std::string                           current_buffer;              // for temporary tag values
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    explicit sax_ctx();
    void reset_for_reuse(std::shared_ptr<xpath_set> t);
  };

  class segment_sax
  {
  public:
    using result_t = std::vector<std::vector<std::string>>;
    segment_sax();
    ~segment_sax();
    segment_sax(segment_sax&&)                 = delete;
    segment_sax& operator=(segment_sax&&)      = delete;
    segment_sax(const segment_sax&)            = delete;
    segment_sax& operator=(const segment_sax&) = delete;

    segment_sax::result_t exec(std::string_view xml_data, std::shared_ptr<xpath_set> targets);
  private:
    template <typename F>
    static void for_each_set_bit(std::uint64_t bits, F&& func); //< oteration over bits set
    static void process_attributes(sax_ctx* ctx, int nb_attributes, const xmlChar** attributes, std::uint64_t attr_bits);
    static void process_elements(sax_ctx* ctx, std::uint64_t elem_bits);
    static void on_start(void*                            user_data,
                         const xmlChar*                   localname,
                         [[maybe_unused]] const xmlChar*  prefix,
                         const xmlChar*                   URI,
                         [[maybe_unused]] int             nb_namespaces,
                         [[maybe_unused]] const xmlChar** namespaces,
                         [[maybe_unused]] int             nb_attributes,
                         [[maybe_unused]] int             nb_defaulted,
                         [[maybe_unused]] const xmlChar** attributes);
    static void on_chars(void* user_data, const xmlChar* ch, int len);
    static void on_end(void* user_data, //
                       const xmlChar* /*localname*/,
                       const xmlChar* /*prefix*/,
                       const xmlChar* /*URI*/);
    static void check_stop_condition(sax_ctx* ctx);
    static bool is_path_match(const std::vector<xml_path_el>& stack, const xml_attr& attr);
  private:
    xmlSAXHandler    handler_{};      // sax parser handler
    xmlParserCtxtPtr ctxt_ = nullptr; // sax parser context
    sax_ctx          ctx_;            // user data associated with the parsing
  };

  inline sax_ctx::sax_ctx()
  {
    static const int buf_size = 1024;
    current_buffer.reserve(buf_size);
  }

  inline void sax_ctx::reset_for_reuse(std::shared_ptr<xpath_set> t)
  {
    targets = std::move(t);
    results.resize(targets->size());
    path_stack.reserve(targets->max_xpath_size());
    for (auto& r : results) r.clear();
    remaining_mask    = targets->full_mask();
    stop_parsing      = false;
    active_target_idx = -1;
    path_stack.clear();
    current_buffer.clear();
  }

  inline segment_sax::segment_sax()
  {
    handler_.initialized    = XML_SAX2_MAGIC;
    handler_.startElementNs = &on_start;
    handler_.endElementNs   = &on_end;
    handler_.characters     = &on_chars;

    ctxt_ = xmlCreatePushParserCtxt(&handler_, &ctx_, nullptr, 0, nullptr);
    // NOLINTNEXTLINE(hicpp-signed-bitwise)
    xmlCtxtUseOptions(ctxt_, XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_NONET);
    ctx_.ctxt = ctxt_;
  }
  /**
   * @brief Destroy the segment sax::segment sax object
   *
   */
  inline segment_sax::~segment_sax()
  {
    if (ctxt_ != nullptr) xmlFreeParserCtxt(ctxt_);
  }
  /**
   * @brief activate the sax parser with new xml document to extract the provided target values
   * @param xml_data xml document
   * @return segment_sax::result_t values of the targets found in xml document
   */
  inline segment_sax::result_t segment_sax::exec(std::string_view xml_data, std::shared_ptr<xpath_set> targets)
  {
    ctx_.reset_for_reuse(std::move(targets));
    // xmlCtxtResetPush perserves allocated internal structures (dict, ns tabele ...)
    // and only set new xml document to be parsed.
    xmlCtxtResetPush(ctxt_, xml_data.data(), static_cast<int>(xml_data.size()), nullptr, nullptr);
    xmlCtxtUseOptions(ctxt_, XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_NONET); // NOLINT(hicpp-signed-bitwise)
    ctx_.ctxt = ctxt_; // xmlCtxtResetPush lahko premakne notranje kazalce, ponovno poveži
    xmlParseChunk(ctxt_, xml_data.data(), static_cast<int>(xml_data.size()), 1);
    return ctx_.results; // brez move — caller mora podatke porabiti/kopirati pred naslednjim exec(
  }

  // Pomožna funkcija za iteracijo čez nastavljene bite
  template <typename F>
  inline void segment_sax::for_each_set_bit(std::uint64_t bits, F&& func)
  {
    while (bits != 0)
    {
      const int t = std::countr_zero(bits);
      std::forward<F>(func)(t);
      bits &= (bits - 1); // Pobriši najnižji nastavljen bit
    }
  }

  inline void segment_sax::process_attributes(sax_ctx* ctx, int nb_attributes, const xmlChar** attributes, std::uint64_t attr_bits)
  {
    // Uporaba vaše predlagane for_each_set_bit za čistočo
    for (int i = 0; i < nb_attributes; ++i)
    { // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic, readability-magic-numbers)
      const xmlChar* attr_localname = attributes[static_cast<ptrdiff_t>(i * 5)];
      const xmlChar* attr_uri       = attributes[i * 5 + 2];
      const xmlChar* val_ptr        = attributes[i * 5 + 3];
      const xmlChar* val_end        = attributes[i * 5 + 4];
      // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic, readability-magic-numbers)

      for_each_set_bit( //
        attr_bits & ctx->remaining_mask,
        [&](int t)
        {
          const auto& target = (*ctx->targets)[static_cast<std::size_t>(t)];
          if (is_path_match(ctx->path_stack, target) && target.attr_name() == reinterpret_cast<const char*>(attr_localname) &&
              target.attr_uri() == (nullptr != attr_uri ? reinterpret_cast<const char*>(attr_uri) : ""))
          {
            ctx->results[t].emplace_back(reinterpret_cast<const char*>(val_ptr), val_end - val_ptr);
            if ((ctx->targets->array_mask() & (std::uint64_t{1} << static_cast<std::size_t>(t))) == 0)
              ctx->remaining_mask &= ~(std::uint64_t{1} << static_cast<std::size_t>(t));
          }
        });
    }
  }
  /**
   * @brief assign index if xpath and current tree stack matches
   * If the bit is set and the corresponding tree stack and xpath matches we need to store
   * the index of the xpath, so that we know where to store corresponding tag value.
   * there can be only one xpath that matches the current tree stack
   * @param ctx data context
   * @param elem_bits set of xpaths (only bits set) that should be checked
   */
  inline void segment_sax::process_elements(sax_ctx* ctx, std::uint64_t elem_bits)
  {
    for_each_set_bit(elem_bits & ctx->remaining_mask,
                     [&](int t)
                     {
                       if (is_path_match(ctx->path_stack, (*ctx->targets)[static_cast<std::size_t>(t)])) //
                         ctx->active_target_idx = t;
                     });
  }

  inline void segment_sax::on_start(void*                            user_data,
                                    const xmlChar*                   localname,
                                    [[maybe_unused]] const xmlChar*  prefix,
                                    const xmlChar*                   URI,
                                    [[maybe_unused]] int             nb_namespaces,
                                    [[maybe_unused]] const xmlChar** namespaces,
                                    [[maybe_unused]] int             nb_attributes,
                                    [[maybe_unused]] int             nb_defaulted,
                                    [[maybe_unused]] const xmlChar** attributes)
  {
    auto* ctx = static_cast<sax_ctx*>(user_data);
    if (ctx->stop_parsing) [[unlikely]]
      return;
    const auto* safe_tag = reinterpret_cast<const char*>(localname);
    const auto* safe_uri = URI != nullptr ? reinterpret_cast<const char*>(URI) : "";
    ctx->path_stack.push_back({.uri = safe_uri, .tag = safe_tag});

    const auto depth = ctx->path_stack.size();
    if (depth > ctx->targets->max_xpath_size()) return; // skip if we are too deep
    process_attributes(ctx, nb_attributes, attributes, ctx->targets->attr_mask(depth));
    process_elements(ctx, ctx->targets->elem_mask(depth));
  }
  /**
   * @brief accumulate tag value until everything is collected
   * There can be several calls for on_chars in order to get whole tag value.
   * Final accumularo is string in the data context (ctx->current buffer)
   * @param user_data  context data
   * @param ch start of the (partial) value
   * @param len length of the provided value in characters
   */
  inline void segment_sax::on_chars(void* user_data, const xmlChar* ch, int len)
  {
    auto* ctx = static_cast<sax_ctx*>(user_data);
    if (ctx->active_target_idx != -1) ctx->current_buffer.append(reinterpret_cast<const char*>(ch), len);
  }
  /**
   * @brief callback for the moment when sax parser reached the end of tag
   *
   * @param user_data context data
   * @param localname localname of the node
   * @param prefix prefix of the tag
   * @param URI universal resource indicator for the node
   */
  inline void segment_sax::on_end(void*                           user_data, // context data
                                  [[maybe_unused]] const xmlChar* localname,
                                  [[maybe_unused]] const xmlChar* prefix,
                                  [[maybe_unused]] const xmlChar* URI)
  {
    auto* ctx = static_cast<sax_ctx*>(user_data);
    if (ctx->active_target_idx != -1)
    {
      auto idx = ctx->active_target_idx;
      ctx->results[idx].push_back(std::move(ctx->current_buffer));
      ctx->current_buffer.clear();
      if ((ctx->targets->array_mask() & (std::uint64_t{1} << static_cast<unsigned int>(idx))) == 0U)
        ctx->remaining_mask &= ~(std::uint64_t{1} << static_cast<unsigned int>(idx));
      ctx->active_target_idx = -1;
    }
    ctx->path_stack.pop_back();
    check_stop_condition(ctx);
  }
  /**
   * @brief set exit flag if no values are needed to be searched for
   *
   * @param ctx
   */
  inline void segment_sax::check_stop_condition(sax_ctx* ctx)
  {
    if (ctx->remaining_mask == 0)
    {
      ctx->stop_parsing = true;
      if (ctx->ctxt != nullptr) xmlStopParser(ctx->ctxt);
    }
  }
  /**
   * @brief compare curent tree position and a specific xpath if they match
   *
   * Comparison between the stack nodes and xpath nodes are made in oposite direction,
   * since it is more probable to be different in some leaf nodes than in the root. All xpaths
   * at least have the same root node.
   * @param stack current position in the xml tree, denoted by a stack of values
   * @param attr  xpath description
   * @return true current tree position and path matches
   * @return false current tree position and path does not match
   */
  inline bool segment_sax::is_path_match(const std::vector<xml_path_el>& stack, const xml_attr& attr)
  {
    auto target_span = attr.xpath();
    if (stack.size() != target_span.size()) return false;
    for (size_t i = stack.size(); i > 0; --i)
    {
      const auto& s = stack[i - 1];
      const auto& t = target_span[i - 1];
      if (s.tag != t.tag || s.uri != t.ns) return false;
    }
    return true;
  }
} // namespace fsp
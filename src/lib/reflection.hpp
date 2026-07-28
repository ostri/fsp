#pragma once
#include "compile_error.hpp"
#include "parsing_util.hpp"
#include "xml_attr.hpp"
#include <array>
#include <meta>
#include <string_view>
#include <type_traits>
#include <vector>

// convenience aliases used by schema classes (e.g. work.hpp) to type their
// placeholder fields -- the annotation text is what actually gets extracted.
using str_t  = std::string;
using cstr_t = std::string_view;

namespace fsp
{
  using str_t  = std::string;
  using cstr_t = std::string_view;

  /**
   * @brief Marker base for schema classes (see work.hpp): a class in a
   * reflected namespace is treated as a segment cut point only if it derives
   * from this (or from whatever Base is passed to classes_of()/proc_data_of()).
   * Other types living in the same namespace (e.g. helper structs, the ns
   * table) are ignored instead of having to be excluded by name/kind.
   */
  struct seg_schema
  {
  };

  /**
   * @brief Parse a single "name=path" annotation string into a raw_attr. Used
   * for class-level (target/segment) annotations, where the desired name
   * (e.g. "header") legitimately differs from the C++ class identifier (e.g.
   * pacs8_header). path is passed through as-is -- any leading cardinality
   * marker (?, * or +) or embedded '@' in it is understood natively by
   * xml_attr's own parsing (see xml_attr.hpp), nothing to do here.
   *
   * @param s annotation payload, e.g. "header=/x:Document/.../GrpHdr"
   * @return constexpr raw_attr with name/path filled in
   */
  consteval raw_attr parse_raw_attr(cstr_t s)
  {
    auto eq = s.find('=');
    if (eq == cstr_t::npos) throw compile_error("annotation is missing '=' between name and path");
    return raw_attr{.name = s.substr(0, eq), .path = s.substr(eq + 1)};
  }

  /**
   * @brief Read the first [[= "..."]] annotation attached to a reflected entity
   * (namespace, class or non-static data member) as its raw quoted text.
   *
   * @tparam Item reflection of the annotated entity
   * @return cstr_t annotation text with static storage duration
   *
   * NOTE: std::meta::extract<T>() is not usable here -- on this GCC branch it
   * fails ("reflect_constant failed") for any annotation payload that carries a
   * pointer (which includes plain string-literal annotations, since they decay
   * to const char*). display_string_of() is the reliable path: it renders the
   * annotation back as source text (e.g. [[=(const char*)"name=path"]]), and we
   * pull the quoted part out of that -- this works fine at compile time,
   * including inside static_assert, as long as the reflection range read by
   * `template for` is bound to a `static constexpr` variable.
   */
  template <std::meta::info Item>
  consteval cstr_t annotation_text()
  {
    static constexpr auto anns = std::define_static_array(std::meta::annotations_of(Item));
    static_assert(! anns.empty(), "reflected item is missing a [[= \"name=path\"]] annotation");
    static constexpr auto disp_chars = std::define_static_array(std::meta::display_string_of(anns[0]));
    constexpr cstr_t      disp(disp_chars.data(), disp_chars.size());
    constexpr auto        first  = disp.find('"');
    constexpr auto        second = disp.find('"', first + 1);
    static_assert(first != cstr_t::npos && second != cstr_t::npos, "annotation is not a quoted string literal");
    return disp.substr(first + 1, second - first - 1);
  }

  template <std::meta::info Item>
  consteval raw_attr attr_of()
  { return parse_raw_attr(annotation_text<Item>()); }

  /**
   * @brief Read a field's [[= "path"]] annotation into a raw_attr, taking the
   * name from the field's own identifier instead of requiring it spelled out
   * again in the annotation string. path is passed through as-is -- any
   * leading cardinality marker (?, * or +) or embedded '@' in it is understood
   * natively by xml_attr's own parsing (see the raw_attr comment in
   * xml_attr.hpp for the exact syntax), nothing to do here.
   *
   * @tparam Item reflection of the annotated non-static data member
   */
  template <std::meta::info Item>
  consteval raw_attr field_attr_of()
  { return raw_attr{.name = std::meta::identifier_of(Item), .path = annotation_text<Item>()}; }

  /**
   * @brief Build the raw_attr table for one xpath schema class by walking its
   * non-static data members and reading each member's [[= "path"]] annotation
   * (name comes from the member's own identifier). Field declaration order is
   * preserved (matches GCC's reflection member order), so the resulting table
   * lines up 1:1 with the equivalent hand-written array.
   *
   * @tparam Class reflection of the annotated schema class (e.g. ^^work::pacs8_header)
   */
  template <std::meta::info Class>
  consteval auto fields_of()
  {
    static constexpr auto                ctx     = std::meta::access_context::unchecked();
    static constexpr auto                members = std::define_static_array(std::meta::nonstatic_data_members_of(Class, ctx));
    std::array<raw_attr, members.size()> out{};
    std::size_t                          i             = 0;
    template for (constexpr auto m : members) out[i++] = field_attr_of<m>();
    return out;
  }

  // fixed-capacity (namespace member count) list of raw_attr with a used-count,
  // since the number of schema classes in a namespace isn't known up front.
  template <std::size_t Capacity>
  struct raw_attr_list
  {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::array<raw_attr, Capacity> data{};
    std::size_t                    count = 0;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    [[nodiscard]] constexpr raw_inputs span() const { return {data.data(), count}; }
  };

  /**
   * @brief Build the target (segment cut point) table for a namespace by
   * collecting the [[= "name=path"]] annotation of every class declared in it
   * that derives from Base, in declaration order.
   *
   * @tparam Namespace reflection of the namespace (e.g. ^^work)
   * @tparam Base      marker base class identifying segment-schema classes
   */
  template <std::meta::info Namespace, typename Base = seg_schema>
  consteval auto classes_of()
  {
    static constexpr auto         ctx     = std::meta::access_context::unchecked();
    static constexpr auto         members = std::define_static_array(std::meta::members_of(Namespace, ctx));
    raw_attr_list<members.size()> out{};
    template for (constexpr auto m : members)
    {
      // NOTE: the Base check must sit in its own nested `if constexpr` --
      // `typename[:m:]` is elaborated for the whole condition even when
      // is_type(m) is false, since `&&` short-circuits the *value*, not the
      // type-checking of a splice. GCC rejects it ("not usable in a splice
      // type") for non-type members if it's combined into one expression.
      if constexpr (std::meta::is_type(m) && std::meta::has_identifier(m))
      {
        if constexpr (std::is_base_of_v<Base, typename[:m:]>) out.data[out.count++] = attr_of<m>();
      }
    }
    return out;
  }

  /**
   * @brief Find the namespace's own "ns" member (the prefix -> URI table used
   * to resolve xpath prefixes, e.g. work.hpp's `static constexpr auto ns = ...`)
   * and return a reflection of it.
   *
   * NOTE: this is a name-based lookup, not std::meta::extract<T>() -- it just
   * splices to the existing variable ([:find_ns_member<Namespace>():] as an
   * expression) rather than trying to materialize a new compile-time constant
   * from it, so it works fine even though fsp::ns holds cstr_t (pointer)
   * members, which is exactly what makes extract<T>() fail elsewhere in this
   * file (see annotation_text()'s note).
   *
   * @tparam Namespace reflection of the namespace expected to declare "ns"
   */
  template <std::meta::info Namespace>
  consteval std::meta::info find_ns_member()
  {
    static constexpr auto ctx     = std::meta::access_context::unchecked();
    static constexpr auto members = std::define_static_array(std::meta::members_of(Namespace, ctx));
    template for (constexpr auto m : members)
    {
      if constexpr (std::meta::has_identifier(m))
        if (std::meta::identifier_of(m) == "ns") return m;
    }
    throw compile_error("namespace is missing a 'ns' member (an array of fsp::ns used to resolve xpath prefixes)");
  }

  /**
   * @brief Build the full fsp::proc_data (targets + one xpath_set per segment
   * class) for a namespace in one call: every class declared in Namespace that
   * derives from Base contributes one target row (its own annotation) and one
   * xpath_set (its annotated non-static data members), in declaration order.
   * The prefix -> URI table is read from the namespace's own "ns" member (see
   * find_ns_member()) instead of being passed in -- Namespace already has
   * everything needed, so there's nothing left for the caller to keep in sync.
   *
   * Usage: static const auto all = fsp::proc_data_of<^^work>();
   *
   * @tparam Namespace reflection of the namespace holding the schema classes
   * @tparam Base      marker base class identifying segment-schema classes
   */
  template <std::meta::info Namespace, typename Base = seg_schema>
  auto proc_data_of() -> proc_data
  {
    static constexpr auto ctx         = std::meta::access_context::unchecked();
    static constexpr auto members     = std::define_static_array(std::meta::members_of(Namespace, ctx));
    static constexpr auto targets_raw = classes_of<Namespace, Base>();
    static constexpr auto ns_member   = find_ns_member<Namespace>();
    constexpr auto&       ns_arr      = [:ns_member:];

    std::vector<xpath_set> xpaths;
    xpaths.reserve(targets_raw.count);
    template for (constexpr auto m : members)
    {
      if constexpr (std::meta::is_type(m) && std::meta::has_identifier(m))
      {
        if constexpr (std::is_base_of_v<Base, typename[:m:]>) xpaths.push_back(fsp::build(fields_of<m>(), ns_arr));
      }
    }
    return proc_data{.targets = fsp::build(targets_raw.span(), ns_arr), .xpaths = std::move(xpaths)};
  }

}; // namespace fsp
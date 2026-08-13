#pragma once
#include "compile_error.hpp"
#include "error_info.hpp"
#include "parsing_util.hpp"
#include "result_values.hpp"
#include "segment_result.hpp"
#include "xml_attr.hpp"
#include <array>
#include <charconv>
#include <expected>
#include <meta>
#include <optional>
#include <string_view>
#include <type_traits>
#include <variant>
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
   * annotation back as source text (e.g. [[=(const char*)"path"]]), and we
   * pull the quoted part out of that -- this works fine at compile time,
   * including inside static_assert, as long as the reflection range read by
   * `template for` is bound to a `static constexpr` variable.
   */
  template <std::meta::info Item>
  consteval cstr_t annotation_text()
  {
    static constexpr auto anns = std::define_static_array(std::meta::annotations_of(Item));
    static_assert(! anns.empty(), "reflected item is missing a [[= \"path\"]] annotation");
    static constexpr auto disp_chars = std::define_static_array(std::meta::display_string_of(anns[0]));
    constexpr cstr_t      disp(disp_chars.data(), disp_chars.size());
    constexpr auto        first  = disp.find('"');
    constexpr auto        second = disp.find('"', first + 1);
    static_assert(first != cstr_t::npos && second != cstr_t::npos, "annotation is not a quoted string literal");
    return disp.substr(first + 1, second - first - 1);
  }

  /**
   * @brief Read a class-level (target/segment) [[= "path"]] annotation into a raw_attr, taking
   * the name from the class's own identifier (e.g. pacs8_txn) instead of requiring it spelled
   * out again in the annotation string -- mirrors field_attr_of() below, which does the same for
   * non-static data members. Class-level annotations have no cardinality of their own (is_opt/
   * is_array default to false), unlike field_attr_of().
   *
   * @tparam Item reflection of the annotated class
   */
  template <std::meta::info Item>
  consteval raw_attr attr_of()
  { return raw_attr{.name = std::meta::identifier_of(Item), .path = annotation_text<Item>()}; }

  /** @brief True iff FieldType is a std::optional<X> -- an o_*-named schema field type. */
  template <typename>
  struct is_optional : std::false_type
  {
  };
  template <typename X>
  struct is_optional<std::optional<X>> : std::true_type
  {
  };
  template <typename FieldType>
  inline constexpr bool is_optional_v = is_optional<FieldType>::value;

  /** @brief True iff FieldType is a std::array<X, N> -- an m_*-named schema field type. */
  template <typename>
  struct is_fixed_array : std::false_type
  {
  };
  template <typename X, std::size_t N>
  struct is_fixed_array<std::array<X, N>> : std::true_type
  {
  };
  template <typename FieldType>
  inline constexpr bool is_fixed_array_v = is_fixed_array<FieldType>::value;

  /** @brief True iff FieldType is a std::vector<X> -- the older, dynamically-growing array schema field type. */
  template <typename>
  struct is_vector : std::false_type
  {
  };
  template <typename X>
  struct is_vector<std::vector<X>> : std::true_type
  {
  };
  template <typename FieldType>
  inline constexpr bool is_vector_v = is_vector<FieldType>::value;

  /**
   * @brief Field type for a validated scalar: X on success, or an index into the owning
   * segment_result::errors() on failure (see materialize<T>() below).
   * @details Deliberately std::expected<X, int> rather than std::expected<X, error_info> --
   * error_info carries two owning std::string members (~90 bytes), so embedding it inline would
   * make every validated field pay that size, and lose trivial-copyability, even on the (common)
   * success path where no error ever occurs. An int index keeps a validated field exactly as
   * small/trivial as X + int, at the cost of the field alone not being interpretable without its
   * owning segment_result.
   * @tparam X the wrapped, self-validating field type -- must provide
   * `static std::expected<X, error_info> parse(cstr_t)` (see is_validated_v below and e.g.
   * fsp::ach::iban_t in ach/utility.hpp)
   */
  template <typename X>
  using validated_t = std::expected<X, int>;

  /** @brief True iff FieldType is a validated_t<X> (i.e. std::expected<X, int>) for some X. */
  template <typename>
  struct is_validated : std::false_type
  {
  };
  template <typename X>
  struct is_validated<std::expected<X, int>> : std::true_type
  {
  };
  template <typename FieldType>
  inline constexpr bool is_validated_v = is_validated<FieldType>::value;

  /**
   * @brief o_- and m_-prefixed validated_t<X> pairs, matching the naming convention used for
   * o_str_t and m_str_t in parsing_util.hpp: o_validated_t<X> is an optional validated field (0
   * or 1 occurrences), m_validated_t<X> is a fixed-capacity (max_values) array of
   * independently-validated occurrences -- each element gets its own parse call and its own
   * error_info, see convert_scalar()'s is_validated_v branch. Purely naming convenience:
   * materialize<T>() already supports std::optional<validated_t<X>> and
   * std::array<validated_t<X>, N> directly, via the same is_optional_v and is_fixed_array_v
   * dispatch every other field type uses.
   */
  template <typename X>
  using o_validated_t = std::optional<validated_t<X>>;
  template <typename X>
  using m_validated_t = std::array<validated_t<X>, max_values>;

  /**
   * @brief Read a field's [[= "path"]] annotation into a raw_attr, taking the name from the
   * field's own identifier instead of requiring it spelled out again in the annotation
   * string. Cardinality comes ONLY from the field's own C++ type -- there is no marker
   * character in path anymore (see the raw_attr comment in xml_attr.hpp): an o_*-named field
   * type (std::optional<X>, see parsing_util.hpp) sets is_opt, and an m_*-named field type
   * (std::array<X, max_values>) or a std::vector<X> field sets is_array.
   *
   * @tparam Item reflection of the annotated non-static data member
   */
  template <std::meta::info Item>
  consteval raw_attr field_attr_of()
  {
    using field_t = typename[:std::meta::type_of(Item):];
    return raw_attr{
      .name     = std::meta::identifier_of(Item),
      .path     = annotation_text<Item>(),
      .is_opt   = is_optional_v<field_t>,
      .is_array = is_fixed_array_v<field_t> || is_vector_v<field_t>,
    };
  }

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
   * collecting the [[= "path"]] annotation of every class declared in it
   * that derives from Base, in declaration order (see attr_of()).
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

  /**
   * @brief Converts one extracted string value into a schema class field's own (scalar)
   * C++ type.
   * @details Materialize's only supported scalar element types for now are str_t (passed
   * through as-is), big_int_t, int_t and small_int_t (std::uint64_t/std::int32_t/std::int16_t,
   * parsed via std::from_chars), ts_t (an ISO 20022 ISODateTime, parsed via
   * parse_iso_datetime()), date_t (an ISO 20022 ISODate, parsed via parse_iso_date()) and
   * amount_t (an ISO 20022 decimal amount, parsed via parse_iso_amount() -- see
   * parsing_util.hpp), plus any o_*-named std::optional<X> of one of those (see
   * is_optional_v above -- unwraps to X, then the result converts back to std::optional<X> at
   * the return statement). Other C++ types are rejected at compile time -- support is added
   * incrementally as materialize()/materialize_variant() grow to cover more of a schema
   * class's field types.
   * @tparam FieldType the target field's own declared C++ type (or a vector/array field's own
   * value_type)
   * @param errors the owning segment_result's error list -- a validated_t<X> field that fails
   * X::parse() appends its error_info here and stores the resulting index instead (see
   * is_validated_v/validated_t above)
   */
  template <typename FieldType>
  FieldType convert_scalar(cstr_t s, std::vector<error_info>& errors)
  {
    if constexpr (std::is_same_v<FieldType, str_t>) return str_t(s);
    else if constexpr (std::is_same_v<FieldType, big_int_t>)
    {
      big_int_t v = 0;
      std::from_chars(s.data(), s.data() + s.size(), v);
      return v;
    }
    else if constexpr (std::is_same_v<FieldType, int_t>)
    {
      int_t v = 0;
      std::from_chars(s.data(), s.data() + s.size(), v);
      return v;
    }
    else if constexpr (std::is_same_v<FieldType, small_int_t>)
    {
      small_int_t v = 0;
      std::from_chars(s.data(), s.data() + s.size(), v);
      return v;
    }
    else if constexpr (std::is_same_v<FieldType, ts_t>) return parse_iso_datetime(s);
    else if constexpr (std::is_same_v<FieldType, date_t>) return parse_iso_date(s);
    else if constexpr (std::is_same_v<FieldType, amount_t>) return parse_iso_amount(s);
    else if constexpr (is_optional_v<FieldType>) return convert_scalar<typename FieldType::value_type>(s, errors);
    else if constexpr (is_validated_v<FieldType>)
    {
      using X   = typename FieldType::value_type;
      auto pars = X::parse(s);
      if (pars) return FieldType{*std::move(pars)};
      errors.push_back(std::move(pars.error()));
      return FieldType{std::unexpect, static_cast<int>(errors.size() - 1)};
    }
    else static_assert(
      sizeof(FieldType) == 0,
      "materialize: unsupported field type (only str_t, big_int_t, int_t, small_int_t, ts_t, date_t, amount_t, validated_t<X> and "
      "std::optional<> of those so far)");
  }

  /**
   * @brief Reflectively fills a schema class instance from one segment's extracted values.
   * @details Walks T's own non-static data members and, for each one found() in values,
   * converts the string value(s) into that member's own C++ type (see convert_scalar()) and
   * assigns it. A member not found() in values is left at its default-constructed value.
   * Field names are matched by the member's own identifier, the same name field_attr_of()
   * used to build the raw_attr the value was extracted under.
   *
   * A std::vector<X> field is treated as an optional-or-repeated-cardinality schema field and
   * filled via values.values(name) -- REQUIRED: T's own C++ field type must agree with the
   * schema's own cardinality for that xpath (vector<> for an optional or repeated array
   * xpath, scalar for a single-valued one). result_values::value()/values() don't cross-check
   * this themselves (see result_values.hpp), so a scalar field wrongly declared for an array
   * xpath reads out of bounds instead of failing cleanly.
   *
   * An m_*-named std::array<X, max_values> field (see parsing_util.hpp) is filled the same way
   * as std::vector<X>, capped at max_values -- extra values beyond max_values are silently
   * dropped (m_* trades unbounded growth for a fixed, allocation-free capacity; the schema
   * would need more than max_values repeats of the same xpath to hit this). Slots beyond the
   * found count keep their default-constructed value, same as any other not-found field.
   * @tparam T a schema class (see work.hpp) whose fields are all currently either str_t,
   * big_int_t, int_t, small_int_t, ts_t, date_t, amount_t, validated_t<X>, an o_*-named
   * std::optional<> of one of those, or a std::vector<>/m_*-named std::array<> of one of those
   * @param seg the segment_result to materialize from -- values() supplies the extracted
   * strings, errors() receives any validated_t<X> field's error_info (see convert_scalar())
   */
  template <typename T>
  T materialize(segment_result& seg)
  {
    const result_values&  values  = seg.values();
    T                     out{};
    static constexpr auto ctx     = std::meta::access_context::unchecked();
    static constexpr auto members = std::define_static_array(std::meta::nonstatic_data_members_of(^^T, ctx));
    template for (constexpr auto m : members)
    {
      constexpr auto name = std::meta::identifier_of(m);
      if (values.found(name))
      {
        using field_t = typename[:std::meta::type_of(m):];
        if constexpr (is_vector_v<field_t>)
        {
          for (const auto& s : values.values(name)) out.[:m:].push_back(convert_scalar<typename field_t::value_type>(s, seg.errors()));
        }
        else if constexpr (is_fixed_array_v<field_t>)
        {
          constexpr std::size_t capacity = std::tuple_size_v<field_t>;
          std::size_t           i        = 0;
          for (const auto& s : values.values(name))
          {
            if (i >= capacity) break;
            out.[:m:][i++] = convert_scalar<typename field_t::value_type>(s, seg.errors());
          }
        }
        else out.[:m:] = convert_scalar<field_t>(values.value(name), seg.errors());
      }
    }
    return out;
  }

  /**
   * @brief Collects reflections of every class in Namespace that derives from Base, in
   * declaration order -- the same set classes_of()/proc_data_of() walk, but returning the
   * classes' own std::meta::info instead of their annotation, for variant_of_t() to turn
   * into a parameter pack.
   * @tparam Namespace reflection of the namespace holding the schema classes
   * @tparam Base      marker base class identifying segment-schema classes
   */
  template <std::meta::info Namespace, typename Base = seg_schema>
  consteval auto seg_schema_infos_of()
  {
    static constexpr auto        ctx     = std::meta::access_context::unchecked();
    static constexpr auto        members = std::define_static_array(std::meta::members_of(Namespace, ctx));
    std::vector<std::meta::info> out;
    template for (constexpr auto m : members)
    {
      if constexpr (std::meta::is_type(m) && std::meta::has_identifier(m))
      {
        if constexpr (std::is_base_of_v<Base, typename[:m:]>) out.push_back(m);
      }
    }
    return out;
  }

  /**
   * @brief The union of every one of Namespace's schema classes (in declaration order) as a
   * single std::variant -- e.g. variant_of_t<^^work> is std::variant<pacs8_header, pacs8_txn>.
   * @tparam Namespace reflection of the namespace holding the schema classes
   * @tparam Base      marker base class identifying segment-schema classes
   */
  template <std::meta::info Namespace, typename Base = seg_schema>
  using variant_of_t = typename[:std::meta::substitute(^^std::variant, std::define_static_array(seg_schema_infos_of<Namespace, Base>())):];

  /** @brief Pack-deduction helper: recovers Ts... from a variant_of_t<Namespace, Base> pointer. */
  template <typename... Ts>
  std::variant<Ts...> materialize_variant_impl(int seg_type, segment_result& seg, std::variant<Ts...>*)
  {
    std::variant<Ts...> out;
    int                 i        = 0;
    bool                assigned = false;
    ((i++ == seg_type ? (out = materialize<Ts>(seg), assigned = true) : false), ...);
    if (! assigned) throw compile_error("materialize_variant: seg_type out of range for Namespace's schema classes");
    return out;
  }

  /**
   * @brief Materializes one segment's values directly into the developer's own schema class,
   * wrapped in the union of every schema class Namespace declares -- what a caller's
   * on_semantic_check() would naturally want instead of the generic, name-indexed result_values.
   * @details seg_type must be the same declaration-order index proc_data_of<Namespace>()
   * assigned to the segment's own schema class (i.e. segment_result::seg_type()).
   * @tparam Namespace reflection of the namespace holding the schema classes
   * @tparam Base      marker base class identifying segment-schema classes
   * @param seg_type declaration-order index of the segment's own schema class within Namespace
   * @param seg      the segment_result to materialize from (see materialize<T>() above)
   */
  template <std::meta::info Namespace, typename Base = seg_schema>
  variant_of_t<Namespace, Base> materialize_variant(int seg_type, segment_result& seg)
  { return materialize_variant_impl(seg_type, seg, static_cast<variant_of_t<Namespace, Base>*>(nullptr)); }

}; // namespace fsp
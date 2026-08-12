#include "work.hpp"
#include <array>
#include <iostream>
#include <meta>
#include <string_view>
#include <type_traits>

// ---- Data structure into which we store the xpath annotation result ----

struct class_info
{
  fsp::raw_attr                 target;
  std::array<fsp::raw_attr, 32> fields{};
  std::size_t                   field_count = 0;
};

// Builds a list of classes in the given namespace that derive from
// fsp::seg_schema, storing each class's target raw_attr and its raw_attr
// field table -- the same data fsp::proc_data_of() uses to build
// fsp::proc_data (see reflection.hpp), here just for printing.
template <std::meta::info Ns>
consteval auto get_classes()
{
  constexpr auto        ctx     = std::meta::access_context::unchecked();
  static constexpr auto members = std::define_static_array(std::meta::members_of(Ns, ctx));

  std::array<class_info, 32> result{};
  std::size_t                count = 0;

  template for (constexpr auto m : members)
  {
    if constexpr (std::meta::is_type(m) && std::meta::has_identifier(m))
    {
      // Nested if constexpr: typename[:m:] is checked for the WHOLE if-constexpr
      // condition even when is_type(m) is not satisfied, so it must stay separate
      // (see the same note on fsp::classes_of() in reflection.hpp).
      if constexpr (std::is_base_of_v<fsp::seg_schema, typename[:m:]>)
      {
        class_info ci{};
        ci.target = fsp::attr_of<m>();

        constexpr auto raw_fields = fsp::fields_of<m>();
        for (std::size_t i = 0; i < raw_fields.size(); ++i) ci.fields[ci.field_count++] = raw_fields[i];

        result[count++] = ci;
      }
    }
  }

  return result;
}

namespace
{
  // "1" / "0..1" / "1..*" / "0..*" -- based on is_opt()/is_array(), which
  // fsp::xml_attr derives from the same raw_attr the real pipeline code
  // uses (see fsp::field_attr_of() in reflection.hpp for the meaning of the ?, *, + markers).
  std::string_view cardinality(const fsp::xml_attr& a)
  {
    if (a.is_array()) return a.is_opt() ? "0..*" : "1..*";
    return a.is_opt() ? "0..1" : "1";
  }

  void print_row(std::string_view indent, const fsp::raw_attr& raw)
  {
    fsp::xml_attr a(0, raw, fsp::work::ns);
    std::cout << indent << raw.name << "  ->  " << a.full_xpath_with_uri() << "  [" << (a.is_attr() ? "attr" : "tag") << ", "
              << cardinality(a) << "]\n";
  }
} // namespace

int main()
{
  constexpr auto classes = get_classes<^^fsp::work>();

  std::cout << "Classes with xpath annotation:\n";
  for (const auto& ci : classes)
  {
    if (ci.target.name.empty()) break;

    print_row("", ci.target);
    for (std::size_t i = 0; i < ci.field_count; ++i) print_row("    ", ci.fields[i]);
  }
}

#include "work.hpp"
#include <meta>
#include <array>
#include <string_view>
#include <iostream>

// ---- Podatkovna struktura, v katero shranimo rezultat xpath anotacije ----

struct field_info
{
  cstr_t name;
  cstr_t xpath; // surov display_string_of niz anotacije, npr. [[=(const char*)"x:GrpHdr/MsgId"]]
};

struct class_info
{
  cstr_t                     name;
  cstr_t                     xpath; // surov display_string_of niz anotacije razreda
  std::array<field_info, 32> fields{};
  std::size_t                field_count = 0;
};

// Prebere prvo anotacijo danega elementa (razred ali polje) kot surov
// display_string_of niz. std::meta::extract<T>(...) na tej (eksperimentalni)
// reflection veji GCC ne deluje zanesljivo za noben kazalčni tip, zato
// uporabimo dokazano delujoč meta::display_string_of() (glej reflection.hpp).
// POZOR: razčlenitev narekovajev (iskanje ") NE sme potekati tukaj, znotraj
// consteval funkcije - cstr_t::find() na tem posebnem
// "reflect_constant" nizu v prevajalnem času ni podprt ('not a constant
// expression'). Zato tu vrnemo samo surov niz; razčlenitev naredimo kasneje,
// v main(), kjer teče kot navadna izvajalna (runtime) koda.
template <std::meta::info Item>
consteval cstr_t read_xpath()
{
  constexpr auto anns = std::define_static_array(std::meta::annotations_of(Item));
  if constexpr (anns.empty()) { return ""; }
  else
  {
    // display_string_of vrne str_t; z define_static_array mu damo
    // statično dobo trajanja, da lahko iz njega varno vrnemo string_view.
    constexpr auto disp_chars = std::define_static_array(std::meta::display_string_of(anns[0]));
    return cstr_t(disp_chars.data(), disp_chars.size());
  }
}

// Zgradi seznam polj razreda skupaj z njihovimi xpath potmi
template <std::meta::info Class>
consteval auto get_fields_with_xpath()
{
  constexpr auto        ctx     = std::meta::access_context::unchecked();
  static constexpr auto members = std::define_static_array(std::meta::nonstatic_data_members_of(Class, ctx));

  std::array<field_info, 32> result{};
  std::size_t                count = 0;

  template for (constexpr auto m : members) { result[count++] = field_info{std::meta::identifier_of(m), read_xpath<m>()}; }

  return result;
}

// Zgradi seznam razredov v podanem imenskem prostoru, za vsak razred pa
// shrani njegov xpath in seznam polj z njihovimi xpath potmi
template <std::meta::info Ns>
consteval auto get_classes_with_xpath()
{
  constexpr auto        ctx     = std::meta::access_context::unchecked();
  static constexpr auto members = std::define_static_array(std::meta::members_of(Ns, ctx));

  std::array<class_info, 32> result{};
  std::size_t                count = 0;

  template for (constexpr auto m : members)
  {
    if constexpr (std::meta::is_type(m) && std::meta::has_identifier(m))
    {
      class_info ci{};
      ci.name  = std::meta::identifier_of(m);
      ci.xpath = read_xpath<m>();

      constexpr auto fields = get_fields_with_xpath<m>();
      for (std::size_t i = 0; i < fields.size() && ! fields[i].name.empty(); ++i) { ci.fields[ci.field_count++] = fields[i]; }

      result[count++] = ci;
    }
  }

  return result;
}

// Iz surovega display_string_of niza (npr. [[=(const char*)"x:GrpHdr/MsgId"]])
// izlušči del med prvim parom narekovajev. Teče kot navadna (runtime) koda -
// namenoma NI consteval/constexpr klicana v prevajalnem času.
cstr_t extract_quoted(cstr_t disp)
{
  auto first = disp.find('"');
  if (first == cstr_t::npos) { return disp; }
  auto second = disp.find('"', first + 1);
  if (second == cstr_t::npos) { return disp; }
  return disp.substr(first + 1, second - first - 1);
}
template <std::meta::info Ns>
consteval auto get_ns_annotation()
{
  static constexpr auto            anns = std::define_static_array(std::meta::annotations_of(Ns));
  std::size_t                      size = 0;
  std::array<fsp::ns, anns.size()> res;
  template for (constexpr auto& el : anns) res[size++] = std::meta::constant_of(el);
  return res;
}
int main()
{
  constexpr auto classes = get_classes_with_xpath<^^work>();

  std::cout << "Razredi z xpath anotacijo:\n";
  for (const auto& ci : classes)
  {
    if (ci.name.empty()) break;

    std::cout << ci.name;
    if (! ci.xpath.empty()) { std::cout << "  ->  " << extract_quoted(ci.xpath); }
    std::cout << '\n';

    for (std::size_t i = 0; i < ci.field_count; ++i)
    {
      const auto& f = ci.fields[i];
      std::cout << "    " << f.name;
      if (! f.xpath.empty()) { std::cout << "  ->  " << extract_quoted(f.xpath); }
      std::cout << '\n';
    }
  }
}
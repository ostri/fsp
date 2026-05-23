#include "test_pars.hpp"
#include <fmt/format.h>
// #include <iostream>

namespace fsp
{
  // clang-format off
  constexpr auto NS = std::to_array<ns>({
    {.prefix = "",   .uri = "en:dolg:uri"}, // default namespace
    {.prefix = "xy", .uri = "krneki"},      // explicitly defined namespace and prefix
  });

  constexpr auto ct_tran_attr = std::to_array<attr_attr>
  ({
//    MA({.name = "txn_id",          .path = ""}),
    MA({.name = "txn_id",          .path = "/a/txn"}, NS),
    MA({.name = "debtor_.iban_",   .path = "a/xy:debtor/iban"}, NS),
    MA({.name = "debtor_.bic_",    .path = "a/xy:debtor/bic"}, NS),
    MA({.name = "creditor_.iban_", .path = "a/creditor/iban"}, NS),
    MA({.name = "creditor_.bic_",  .path = "a/creditor/bic"}, NS),
    MA({.name = "amount_",         .path = "a/amount"}, NS),
    MA({.name = "currency_",       .path = "a/amount/ccy",    .is_attr = true, .is_opt = true}, NS),
    MA({.name = "value_date_",     .path = "a/date",                           .is_opt = true}, NS),
  });
  // clang-format on

  // NOLINTBEGIN(readability-magic-numbers)
  // Testi zastavic v času prevajanja
  static_assert(! ct_tran_attr[0].is_attr);
  static_assert(! ct_tran_attr[0].is_opt);

  // Test za "currency_", ki ima is_attr = true in is_opt = true
  static_assert(ct_tran_attr[6].is_attr);
  static_assert(ct_tran_attr[6].is_opt);
  static_assert(ct_tran_attr[6].xpath[1].tag == "amount");

  // Test za "value_date_", ki ima is_opt = true
  static_assert(! ct_tran_attr[7].is_attr);
  static_assert(ct_tran_attr[7].is_opt);
  // NOLINTEND(readability-magic-numbers)
}; // namespace fsp
int main()
{
  // Izpis tabele z vsemi atributi za kontrolo med izvajanjem
  fmt::print("{:<25} | {:<20} | {:<7} | {:<8} | {:<7} | {}\n", "Name", "Path", "Is_Attr", "Is_Array", "Is_Opt", "XPath");
  fmt::print("{:-<90}\n", "");

  for (const auto& attr : fsp::ct_tran_attr)
  {
    std::string xpath_str;
    for (size_t i = 0; i < attr.xpath_size; ++i)
    {
      xpath_str += fmt::format("[{}{}]", //
                               attr.xpath.at(i).ns.empty() ? "" : std::string(attr.xpath.at(i).ns) + "×",
                               attr.xpath.at(i).tag);
    }

    fmt::print("{:<25} | {:<20} | {:<7} | {:<8} | {:<7} | {}\n", attr.name, attr.path, attr.is_attr, attr.is_array, attr.is_opt, xpath_str);
  }
  return 0;
}
#include "test_pars.hpp"
#include <fmt/format.h>

namespace fsp
{
  // clang-format off
  constexpr auto NS = std::to_array<ns>({
    {.prefix = "",   .uri = "urn:iso:std:iso:20022:tech:xsd:pacs.008.001.10"}, // default namespace
    {.prefix = "xy", .uri = "krneki"},      // explicitly defined namespace and prefix
  });

//  constexpr const std::array raw_inputs = std::to_array<raw_attr>({
  constexpr auto raw = std::to_array<raw_attr>({
    {.name="txn_id",          .path="CdtTrfTxInf/PmtId/TxId"},
    {.name="debtor_.iban_",   .path="CdtTrfTxInf/DbtrAcct/Id/IBAN"},
    {.name="debtor_.bic_",    .path="CdtTrfTxInf/DbtrAgt/FinInstnId/BICFI"},
    {.name="creditor_.iban_", .path="CdtTrfTxInf/CdtrAcct/Id/IBAN"},
    {.name="creditor_.bic_",  .path="CdtTrfTxInf/CdtrAgt/FinInstnId/BICFI"},
    {.name="amount_",         .path="CdtTrfTxInf/IntrBkSttlmAmt"},
    {.name="currency_",       .path="CdtTrfTxInf/IntrBkSttlmAmt/@Ccy",        .is_opt=true},
    {.name="value_date_",     .path="CdtTrfTxInf/IntrBkSttlmDt",              .is_opt=true},
    {.name="remmitance_",     .path="CdtTrfTxInf/RmtInf/Strd/RfrdDocInf/*Nb", .is_opt=true},
  });
  // clang-format on
  // NOLINTNEXTLINE(cert-err58-cpp)
  const auto xtn = build(raw, NS); // xml tree node(s)

  // NOLINTBEGIN(readability-magic-numbers)
  // Testi zastavic v času prevajanja
  // static_assert(xtn["txn_id"].name() == "txn_id");
  // static_assert(! xtn["txn_id"].is_attr());
  // static_assert(! xtn["txn_id"].is_opt());

  // // Test za "currency_", ki ima is_attr = true in is_opt = true
  // static_assert(xtn["currency_"].name() == "currency_");
  // static_assert(xtn["currency_"].is_attr());
  // static_assert(xtn["currency_"].is_opt());
  // static_assert(xtn["currency_"].attr_name() == "Ccy");

  // // Test za "value_date_", ki ima is_opt = true
  // static_assert(! xtn["value_date_"].is_attr());
  // static_assert(xtn["value_date_"].is_opt());
  // // Test za "remittance_", ki ima is_attr = true in is_opt = true
  // static_assert(xtn["remmitance_"].name() == "remmitance_");
  // static_assert(! xtn["remmitance_"].is_attr());
  // static_assert(xtn["remmitance_"].is_opt());
  // static_assert(xtn["remmitance_"].last().tag == "Nb");
  //  NOLINTEND(readability-magic-numbers)
}; // namespace fsp

int main()
{
  try
  {
    constexpr const auto line_len = 120U;
    fmt::print("\n");
    for (const auto& el : fsp::NS) { fmt::print("prefix:'{:10}' uri: '{}'\n", el.prefix, el.uri); }
    fmt::print("\nNamespace(s):\n");
    // Izpis tabele z vsemi atributi za kontrolo med izvajanjem
    //    fmt::print("Calculated xpath depth: {}\n", fsp::get_xpaths_max_depth(fsp::raw));
    fmt::print("Calculated xpath depth: {}\n", fsp::xtn.max_xpath_size());
    fmt::print("{:<15} | {:<40} | {:<7} | {:<8} | {:<7} | {}\n", "Name", "Path", "Is_Attr", "Is_Array", "Is_Opt", "XPath");
    fmt::print("{:-<{}}\n", "", line_len);

    for (const auto& attr : fsp::xtn)
    {
      std::string xpath_str = attr.full_xpath();
      fmt::print("{:<15} | {:<40} | {:<7} | {:<8} | {:<7} | {}\n",
                 attr.name(),
                 attr.path(),
                 attr.is_attr() ? "yes" : " ",
                 attr.is_array() ? "yes" : " ",
                 attr.is_opt() ? "yes" : " ",
                 xpath_str);
    }

    fmt::print("currency: '{}'\n", fsp::xtn["currency_"].xpath()[0].tag);
    fmt::print("currency: '{}'\n", fsp::xtn["currency_"].xpath()[1].tag);
    fmt::print("currency: '{}'\n", fsp::xtn["currency_"].xpath()[2].tag);
    fmt::print("currency: '{}'\n", fsp::xtn["currency_"].last().tag);
    fmt::print("currency: '{}'\n", fsp::xtn["currency_"].attr_name());
    fmt::print("currency: '{}'\n", fsp::xtn["currency_"].full_xpath());
    fmt::print("currency: '{}'\n", fsp::xtn["currency_"].full_xpath_with_uri());
    fmt::print("remmitance: '{}'\n", fsp::xtn["remmitance_"].xpath()[0].tag);
    fmt::print("remmitance: '{}'\n", fsp::xtn["remmitance_"].xpath()[1].tag);
    fmt::print("remmitance: '{}'\n", fsp::xtn["remmitance_"].xpath()[2].tag);
    fmt::print("remmitance: '{}'\n", fsp::xtn["remmitance_"].xpath()[3].tag);
    fmt::print("remmitance: '{}'\n", fsp::xtn["remmitance_"].xpath()[4].tag);
    fmt::print("remmitance: '{}'\n", fsp::xtn["remmitance_"].last().tag);
    fmt::print("remmitance: '{}'\n", fsp::xtn["remmitance_"].attr_name());
    fmt::print("remmitance: '{}'\n", fsp::xtn["remmitance_"].full_xpath());
    fmt::print("remmitance: '{}'\n", fsp::xtn["remmitance_"].full_xpath_with_uri());
    fmt::print("lowest and the biggest tag name value by depth\n");
    for (auto ndx = 0U; ndx < fsp::xtn.max_xpath_size(); ndx++) //
    {
      fmt::print("depth: {:3}: min: {:15} max: {:15}\n", +ndx, fsp::xtn.first(ndx), fsp::xtn.last(ndx));
    }
    // static_assert(fsp::xtn.first_xpath_tag_name(0) == "CdtTrfTxInf");
    // static_assert(fsp::xtn.last_xpath_tag_name(0) == "CdtTrfTxInf");
    // static_assert(fsp::xtn.first_xpath_tag_name(1) == "CdtrAcct");
    // static_assert(fsp::xtn.last_xpath_tag_name(1) == "RmtInf");
    // static_assert(fsp::xtn.first_xpath_tag_name(2) == "FinInstnId");
    // static_assert(fsp::xtn.last_xpath_tag_name(2) == "TxId");
    // static_assert(fsp::xtn.first_xpath_tag_name(3) == "BICFI");
    // static_assert(fsp::xtn.last_xpath_tag_name(3) == "RfrdDocInf");
    // static_assert(fsp::xtn.first_xpath_tag_name(4) == "Nb");
    // static_assert(fsp::xtn.last_xpath_tag_name(4) == "Nb");
  }
  catch (...)
  {
    fmt::print("unknown exception");
  }
  return 0;
}
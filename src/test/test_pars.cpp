#include "test_pars.hpp"
#include <fmt/format.h>

namespace fsp
{
  // clang-format off
  constexpr auto NS = std::to_array<ns>({
    {.prefix = "",   .uri = "urn:iso:std:iso:20022:tech:xsd:pacs.008.001.10"}, // default namespace
    {.prefix = "xy", .uri = "krneki"},      // explicitly defined namespace and prefix
  });

  constexpr const std::array raw_inputs = std::to_array<raw_attr>({
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

  // 2. Eksplicitno podamo znane dimenzije konstruktorju
  // constexpr attr_tbl<raw_inputs.size(), fsp::get_max_depth(raw_inputs)> ct_tran_attr(raw_inputs, NS);
  constexpr path_node_struct<100, raw_inputs.size(), get_max_depth(raw_inputs)> ct_tran_attr(raw_inputs, NS);

  // NOLINTBEGIN(readability-magic-numbers)
  // Testi zastavic v času prevajanja
  static_assert(ct_tran_attr["txn_id"].name == "txn_id");
  static_assert(! ct_tran_attr["txn_id"].is_attr);
  static_assert(! ct_tran_attr["txn_id"].is_opt);

  // Test za "currency_", ki ima is_attr = true in is_opt = true
  static_assert(ct_tran_attr["currency_"].name == "currency_");
  static_assert(ct_tran_attr["currency_"].is_attr);
  static_assert(ct_tran_attr["currency_"].is_opt);
  static_assert(ct_tran_attr["currency_"].last().tag == "Ccy");

  // Test za "value_date_", ki ima is_opt = true
  static_assert(! ct_tran_attr["value_date_"].is_attr);
  static_assert(ct_tran_attr["value_date_"].is_opt);
  // Test za "remittance_", ki ima is_attr = true in is_opt = true
  static_assert(ct_tran_attr["remmitance_"].name == "remmitance_");
  static_assert(! ct_tran_attr["remmitance_"].is_attr);
  static_assert(ct_tran_attr["remmitance_"].is_opt);
  static_assert(ct_tran_attr["remmitance_"].last().tag == "Nb");
  // NOLINTEND(readability-magic-numbers)
}; // namespace fsp

int main()
{
  fmt::print("\n");
  for (const auto& el : fsp::NS) { fmt::print("prefix:'{:10}' uri: '{}'\n", el.prefix, el.uri); }
  fmt::print("\nNamespace(s):");
  // Izpis tabele z vsemi atributi za kontrolo med izvajanjem
  fmt::print("Calculated array depth: {}\n", fsp::get_max_depth(fsp::raw_inputs));
  fmt::print("{:<15} | {:<40} | {:<7} | {:<8} | {:<7} | {}\n", "Name", "Path", "Is_Attr", "Is_Array", "Is_Opt", "XPath");
  fmt::print("{:-<90}\n", "");

  for (const auto& attr : fsp::ct_tran_attr)
  {
    std::string xpath_str;
    for (size_t i = 0; i < attr.xpath_size_; ++i)
    {
      xpath_str += fmt::format("[{}]", //
                                       //  attr.xpath.at(i).ns.empty() ? "def" : std::string(attr.xpath.at(i).ns) + "×",
                                       //  attr.xpath.at(i).tag);
                               attr.xpath.at(i).tag);
    }

    fmt::print("{:<15} | {:<40} | {:<7} | {:<8} | {:<7} | {}\n", attr.name, attr.path, attr.is_attr, attr.is_array, attr.is_opt, xpath_str);
  }

  // fmt::print("currency: '{}'\n", fsp::ct_tran_attr["currency_"].xpath[1].tag);
  // fmt::print("currency: '{}'\n", fsp::ct_tran_attr["currency_"].xpath[0].tag);
  // fmt::print("currency: '{}'\n", fsp::ct_tran_attr["currency_"].xpath[2].tag);
  // fmt::print("currency: '{}'\n", fsp::ct_tran_attr["currency_"].last().tag);
  // fmt::print("remmitance: '{}'\n", fsp::ct_tran_attr["remmitance_"].xpath[0].tag);
  // fmt::print("remmitance: '{}'\n", fsp::ct_tran_attr["remmitance_"].xpath[1].tag);
  // fmt::print("remmitance: '{}'\n", fsp::ct_tran_attr["remmitance_"].xpath[2].tag);
  // fmt::print("remmitance: '{}'\n", fsp::ct_tran_attr["remmitance_"].xpath[3].tag);
  // fmt::print("remmitance: '{}'\n", fsp::ct_tran_attr["remmitance_"].xpath[4].tag);
  // fmt::print("remmitance: '{}'\n", fsp::ct_tran_attr["remmitance_"].last().tag);
  return 0;
}
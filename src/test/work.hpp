#pragma once

#include "reflection.hpp"

/**
 * @brief pacs.008 segment schema, expressed as reflectable annotations instead
 * of hand-written fsp::raw_attr arrays.
 *
 * Each schema class stands for one segment cut point, annotated with its own
 * [[= "name=path"]] (name differs from the C++ class identifier, e.g. "header"
 * vs. pacs8_header, so it's spelled out). Each non-static data member stands
 * for one xpath to extract from that segment, annotated with just [[= "path"]]
 * -- its raw_attr name comes from the member's own identifier, no need to
 * repeat it. Cardinality comes ONLY from the field's own C++ type (see
 * fsp::field_attr_of() in reflection.hpp) -- there is no marker character in
 * path anymore:
 *
 *   str_t/big_int_t/int_t/small_int_t/date_t/ts_t/amount_t   required, exactly one value
 *   o_str_t/o_big_int_t/... (std::optional<X>)               optional, zero or one value
 *   m_str_t/m_big_int_t/... (std::array<X, max_values>)       zero to max_values values
 *   std::vector<X>                                            zero or more values
 *
 * proc_data_of()'s own extraction reads only the annotation text, never the member's C++
 * type -- but fsp::materialize<T>()/materialize_variant() (see reflection.hpp) DO read it, to
 * fill a real instance of this class from a segment's extracted values, and
 * fsp::field_attr_of() reads it too, to derive is_opt/is_array for the extraction engine
 * itself (xpath_set/xml_attr). Currently supported scalar element types are str_t, big_int_t,
 * int_t, small_int_t, ts_t, date_t and amount_t; more are added incrementally (see
 * materialize<T>()'s own doc comment).
 *
 * Classes deriving from fsp::seg_schema are the segment cut points; see
 * fsp::proc_data_of() in reflection.hpp for how this whole namespace is turned
 * into the same fsp::proc_data pacs8.cpp used to build by hand.
 */
namespace fsp::work
{
  // clang-format off
  static constexpr auto ns = std::to_array<fsp::ns>({
    {.prefix = "",   .uri = "urn:iso:std:iso:20022:tech:xsd:pacs.008.001.08"}, // default namespace
    {.prefix = "x",  .uri = "urn:iso:std:iso:20022:tech:xsd:pacs.008.001.08"}, // default namespace
    {.prefix = "xy", .uri = "krneki"},                                        // explicitly defined namespace and prefix
  });
  // clang-format on

  class[[= "header=/x:Document/FIToFICstmrCdtTrf/x:GrpHdr"]] pacs8_header : public fsp::seg_schema
  {
  public:
    // clang-format off
    [[= "x:GrpHdr/MsgId"]]                str_t     msg_id;
    [[= "GrpHdr/NbOfTxs"]]                big_int_t no_of_txn;
    [[= "GrpHdr/TtlIntrBkSttlmAmt"]]      amount_t  amount_sum;
    [[= "GrpHdr/TtlIntrBkSttlmAmt/@Ccy"]] o_str_t   currency;
    [[= "x:GrpHdr/CreDtTm"]]              o_ts_t    msg_ts;
    [[= "GrpHdr/IntrBkSttlmDt"]]          o_date_t  value_date;
    // clang-format on
  };

  class[[= "transaction=/Document/x:FIToFICstmrCdtTrf/x:CdtTrfTxInf"]] pacs8_txn : public fsp::seg_schema
  {
  public:
    // clang-format off
    [[= "CdtTrfTxInf/PmtId/TxId"]]                str_t    txn_id;
    [[= "CdtTrfTxInf/DbtrAcct/Id/IBAN"]]          str_t    debtor_iban;
    [[= "CdtTrfTxInf/DbtrAgt/FinInstnId/BICFI"]]  str_t    debtor_bic;
    [[= "CdtTrfTxInf/CdtrAcct/Id/IBAN"]]          str_t    creditor_iban;
    [[= "CdtTrfTxInf/CdtrAgt/FinInstnId/BICFI"]]  str_t    creditor_bic;
    [[= "CdtTrfTxInf/IntrBkSttlmAmt"]]            amount_t amount;
    [[= "CdtTrfTxInf/IntrBkSttlmAmt/@Ccy"]]       o_str_t  currency;
    [[= "CdtTrfTxInf/InstgAgt/FinInstnId/BICFI"]] m_str_t  instr_agent;
    // clang-format on
  };
} // namespace fsp::work
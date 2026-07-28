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
 * repeat it. An optional leading cardinality marker on the path controls
 * raw_attr::is_opt/is_array (see fsp::field_attr_of() in reflection.hpp):
 *
 *   "path"    required, exactly one value
 *   "?path"   optional, zero or one value
 *   "*path"   optional, zero or more values
 *   "+path"   required, one or more values
 *
 * Field types are placeholders -- the class exists to hang annotations on,
 * extraction reads only the annotation text, not the member's C++ type.
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
    [[= "x:GrpHdr/MsgId"]]                  str_t   msg_id;
    [[= "GrpHdr/TtlIntrBkSttlmAmt"]]        int64_t amount_sum;
    [[= "?GrpHdr/TtlIntrBkSttlmAmt/@Ccy"]]  str_t   currency;
    [[= "?x:GrpHdr/CreDtTm"]]               str_t   msg_ts;
    [[= "?GrpHdr/IntrBkSttlmDt"]]           str_t   value_date;
    // clang-format on
  };

  class[[= "transaction=/Document/x:FIToFICstmrCdtTrf/x:CdtTrfTxInf"]] pacs8_txn : public fsp::seg_schema
  {
  public:
    // clang-format off
    [[= "CdtTrfTxInf/PmtId/TxId"]]                       str_t   txn_id;
    [[= "CdtTrfTxInf/DbtrAcct/Id/IBAN"]]                 int64_t debtor_iban;
    [[= "CdtTrfTxInf/DbtrAgt/FinInstnId/BICFI"]]         str_t   debtor_bic;
    [[= "CdtTrfTxInf/CdtrAcct/Id/IBAN"]]                 int64_t creditor_iban;
    [[= "CdtTrfTxInf/CdtrAgt/FinInstnId/BICFI"]]         str_t   creditor_bic;
    [[= "CdtTrfTxInf/IntrBkSttlmAmt"]]                   str_t   amount;
    [[= "?CdtTrfTxInf/IntrBkSttlmAmt/@Ccy"]]             str_t   currency;
    [[= "*CdtTrfTxInf/InstgAgt/FinInstnId/BICFI"]]       str_t   instr_agent;
    // clang-format on
  };
} // namespace fsp::work
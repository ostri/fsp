#pragma once

#include "reflection.hpp"
#include "ach/utility.hpp"
#include "user_types.hpp"

namespace fsp::work
{
  // clang-format off
  static constexpr auto ns = std::to_array<fsp::ns>({
    {.prefix = "",   .uri = "urn:iso:std:iso:20022:tech:xsd:pacs.008.001.08"}, // default namespace
    {.prefix = "x",  .uri = "urn:iso:std:iso:20022:tech:xsd:pacs.008.001.08"}, // explicitly defined namespace and prefix
    {.prefix = "xy", .uri = "krneki"},
  });
  // clang-format on

  // BIC codes of every agent in ach's own dic_agents reference table (see
  // ach/config/dic_agents.conf) -- kept here as a plain, semicolon-delimited literal instead of
  // reading that JSON file at runtime, since fsp only vendors ach/utility.hpp/.cpp (see
  // src/ach/), not ach's own DB-backed dic_agents loader. usr::bic_code_t::init() (see below)
  // needs exactly this: a caller-owned, stable buffer plus a delimiter, nothing more.
  // clang-format off
  static constexpr fsp::cstr_t known_agent_bics =
    "HAABSI22;BAKOSI2X;KSPKSI22;SZKBSI2X;GORESI2X;LJBASI2X;KBMASI2X;"
    "SIDRSI22;BACXSI22;HDELSI22;HLONSI22;HKVISI22;BFKKSI22;BSLJSI2X";
  // clang-format on

  class[[= "/x:Document/FIToFICstmrCdtTrf/x:GrpHdr"]] pacs8_hdr : public fsp::seg_schema
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

  class[[= "/Document/x:FIToFICstmrCdtTrf/x:CdtTrfTxInf"]] pacs8_txn : public fsp::seg_schema
  {
  public:
    // clang-format off
    [[= "CdtTrfTxInf/PmtId/TxId"]]                str_t                                         txn_id;
    [[= "CdtTrfTxInf/DbtrAcct/Id/IBAN"]]           validated_t<fsp::ach::iban_t>                 debtor_iban;
    [[= "CdtTrfTxInf/DbtrAgt/FinInstnId/BICFI"]]   validated_t<usr::bic_code_t>                  debtor_bic;
    [[= "CdtTrfTxInf/CdtrAcct/Id/IBAN"]]           validated_t<fsp::ach::iban_t>                 creditor_iban;
    [[= "CdtTrfTxInf/CdtrAgt/FinInstnId/BICFI"]]   validated_t<usr::bic_code_t>                  creditor_bic;
    [[= "CdtTrfTxInf/IntrBkSttlmAmt"]]             validated_t<usr::bounded_amount_t<1, 50000>>  amount; // NOLINT(readability-magic-numbers)
    [[= "CdtTrfTxInf/IntrBkSttlmAmt/@Ccy"]]        o_str_t                                       currency;
    [[= "CdtTrfTxInf/InstgAgt/FinInstnId/BICFI"]]  m_str_t                                       instr_agent;
    // clang-format on
  };
} // namespace fsp::work
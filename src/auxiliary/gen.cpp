#include "gen.hpp"

#include <iostream>
#include <string>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <random>
#include <ctime>

// Function to get current date in ISO format (YYYY-MM-DD)
str_t getCurrentDate()
{
  auto        now      = std::chrono::system_clock::now();
  std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  std::tm     now_tm   = {};
#ifdef _WIN32
  localtime_s(&now_tm, &now_time);
#else
  localtime_r(&now_time, &now_tm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&now_tm, "%Y-%m-%d");
  return oss.str();
}

// Function to get current date time in ISO format (YYYY-MM-DDThh:mm:ss.sssZ)
str_t getCurrentDateTime()
{
  auto now    = std::chrono::system_clock::now();
  auto now_ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
  auto value  = now_ms.time_since_epoch();
  // NOLINTNEXTLINE(readability-magic-numbers)
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(value) % 1000;

  std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  std::tm     now_tm   = {};
#ifdef _WIN32
  gmtime_s(&now_tm, &now_time);
#else
  gmtime_r(&now_time, &now_tm);
#endif

  std::ostringstream oss;
  oss << std::put_time(&now_tm, "%Y-%m-%dT%H:%M:%S");
  oss << "." << std::setfill('0') << std::setw(3) << ms.count() << "Z";
  return oss.str();
}

// Function to generate a random numeric string of given length
str_t randomNumericString(size_t length)
{
  static const char                      digits[] = "0123456789"; // NOLINT(hicpp-avoid-c-arrays)
  static std::random_device              rd;
  static std::mt19937                    gen(rd());
  static std::uniform_int_distribution<> dis(0, sizeof(digits) - 2);

  str_t result;
  result.reserve(length);
  for (size_t i = 0; i < length; ++i) { result += digits[dis(gen)]; } // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
  return result;
}

// Function to generate a random alphanumeric string of given length
str_t randomString(size_t length)
{
  // NOLINTNEXTLINE(hicpp-avoid-c-arrays)
  static const char                      alphanum[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  static std::random_device              rd;
  static std::mt19937                    gen(rd());
  static std::uniform_int_distribution<> dis(0, sizeof(alphanum) - 2);

  str_t result;
  result.reserve(length);
  for (size_t i = 0; i < length; ++i) { result += alphanum[dis(gen)]; } // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
  return result;
}

// Function to generate a random IBAN (starts with EE for Estonia)
str_t randomIBAN()
{
  // Generate a random 18-digit IBAN (EE + 2 check digits + 16 digits)
  // NOLINTNEXTLINE(readability-magic-numbers)
  str_t iban = "EE" + randomNumericString(2) + randomNumericString(16);
  return iban;
}

// Function to generate a random BICFI (8 or 11 characters)
str_t randomBICFI()
{
  static std::random_device rd;
  static std::mt19937       gen(rd());

  str_t                           bic;
  std::uniform_int_distribution<> letter_dist(0, 25); // NOLINT(readability-magic-numbers)
  std::uniform_int_distribution<> branch_dist(0, 1);

  // First 4 chars: bank code (letters)
  for (int i = 0; i < 4; ++i) { bic += static_cast<char>('A' + letter_dist(gen)); }
  // Next 2 chars: country code (letters)
  for (int i = 0; i < 2; ++i) { bic += static_cast<char>('A' + letter_dist(gen)); }

  // Next 2 chars: location code (alphanumeric)
  static const char               alnum[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"; // NOLINT(hicpp-avoid-c-arrays)
  std::uniform_int_distribution<> alnum_dist(0, static_cast<int>(sizeof(alnum)) - 2);
  for (int i = 0; i < 2; ++i) { bic += alnum[alnum_dist(gen)]; } // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)

  // Optional branch code (3 chars) - sometimes present (50% chance)
  if (branch_dist(gen) == 0)
  {
    for (int i = 0; i < 3; ++i) { bic += static_cast<char>('A' + letter_dist(gen)); }
  }
  return bic;
}

// Function to generate a random amount between min and max with 2 decimal places
str_t randomAmount(double min, double max)
{
  static std::random_device        rd;
  static std::mt19937              gen(rd());
  std::uniform_real_distribution<> dis(min, max);

  double             amount = dis(gen);
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(2) << amount;
  return oss.str();
}

// Function to generate a random RF Creditor Reference (ISO 11649)
str_t randomRFCreditorReference()
{
  // RF + 2 check digits + up to 21 alphanumeric characters
  // NOLINTNEXTLINE(readability-magic-numbers)
  str_t rf = "RF" + randomNumericString(2) + randomString(21);
  return rf;
}

// Generate the complete XML document
void generateXML(int numTransactions)
{
  // NOLINTNEXTLINE(readability-magic-numbers)
  str_t msgId            = "MSG" + randomNumericString(10);
  str_t creationDateTime = getCurrentDateTime();
  str_t nbOfTxs          = std::to_string(numTransactions);
  str_t intrBkSttlmDt    = getCurrentDate();
  // NOLINTNEXTLINE(readability-magic-numbers)
  str_t ttlIntrBkSttlmAmt = std::to_string(numTransactions); // randomAmount(0.01, 999999999999999.99);
  str_t instgAgtBIC       = randomBICFI();
  str_t instdAgtBIC       = randomBICFI();

  // XML header
  std::cout << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  std::cout << "<Document xmlns=\"urn:iso:std:iso:20022:tech:xsd:pacs.008.001.08\"\n";
  std::cout << "          xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\n";
  std::cout << "  <FIToFICstmrCdtTrf>\n";

  // Group Header
  std::cout << "    <GrpHdr>\n";
  std::cout << "      <MsgId>" << msgId << "</MsgId>\n";
  std::cout << "      <CreDtTm>" << creationDateTime << "</CreDtTm>\n";
  std::cout << "      <NbOfTxs>" << nbOfTxs << "</NbOfTxs>\n";
  std::cout << "      <TtlIntrBkSttlmAmt Ccy=\"EUR\">" << ttlIntrBkSttlmAmt << "</TtlIntrBkSttlmAmt>\n";
  std::cout << "      <IntrBkSttlmDt>" << intrBkSttlmDt << "</IntrBkSttlmDt>\n";
  std::cout << "      <SttlmInf>\n";
  std::cout << "        <SttlmMtd>CLRG</SttlmMtd>\n";
  std::cout << "      </SttlmInf>\n";
  std::cout << "      <PmtTpInf>\n";
  std::cout << "        <SvcLvl>\n";
  std::cout << "          <Cd>SEPA</Cd>\n";
  std::cout << "        </SvcLvl>\n";
  std::cout << "        <CtgyPurp>\n";
  std::cout << "          <Cd>FCIN</Cd>\n";
  std::cout << "        </CtgyPurp>\n";
  std::cout << "      </PmtTpInf>\n";
  std::cout << "      <InstgAgt>\n";
  std::cout << "        <FinInstnId>\n";
  std::cout << "          <BICFI>" << instgAgtBIC << "</BICFI>\n";
  std::cout << "        </FinInstnId>\n";
  std::cout << "      </InstgAgt>\n";
  std::cout << "      <InstdAgt>\n";
  std::cout << "        <FinInstnId>\n";
  std::cout << "          <BICFI>" << instdAgtBIC << "</BICFI>\n";
  std::cout << "        </FinInstnId>\n";
  std::cout << "      </InstdAgt>\n";
  std::cout << "    </GrpHdr>\n";

  // Transactions
  for (int i = 0; i < numTransactions; ++i)
  {
    // NOLINTNEXTLINE(readability-magic-numbers)
    str_t endToEndId = "E2E" + randomString(15);
    // NOLINTNEXTLINE(readability-magic-numbers)
    str_t txId = "TXN" + randomNumericString(15);
    // NOLINTNEXTLINE(readability-magic-numbers)
    str_t intrBkSttlmAmtTx = "1.00";        // randomAmount(0.01, 999999999.99);
    str_t dbtrNm           = randomBICFI(); // Originator PSP BIC
    str_t dbtrId           = randomBICFI(); // Originator PSP AnyBIC
    str_t dbtrAcct         = randomIBAN();
    str_t dbtrAgtBIC       = randomBICFI();
    str_t cdtrAgtBIC       = randomBICFI();
    str_t cdtrNm           = randomBICFI(); // Beneficiary PSP BIC
    str_t cdtrId           = randomBICFI(); // Beneficiary PSP AnyBIC
    str_t cdtrAcct         = randomIBAN();
    str_t creditorRef      = randomRFCreditorReference();
    str_t issuer           = "ISO";

    std::cout << "    <CdtTrfTxInf>\n";
    std::cout << "      <PmtId>\n";
    std::cout << "        <EndToEndId>" << endToEndId << "</EndToEndId>\n";
    std::cout << "        <TxId>" << txId << "</TxId>\n";
    std::cout << "      </PmtId>\n";
    std::cout << "      <PmtTpInf>\n";
    std::cout << "        <SvcLvl>\n";
    std::cout << "          <Cd>SEPA</Cd>\n";
    std::cout << "        </SvcLvl>\n";
    std::cout << "        <CtgyPurp>\n";
    std::cout << "          <Cd>FCIN</Cd>\n";
    std::cout << "        </CtgyPurp>\n";
    std::cout << "      </PmtTpInf>\n";
    std::cout << "      <IntrBkSttlmAmt Ccy=\"EUR\">" << intrBkSttlmAmtTx << "</IntrBkSttlmAmt>\n";
    std::cout << "      <ChrgBr>SLEV</ChrgBr>\n";
    std::cout << "      <InstgAgt>\n";
    std::cout << "        <FinInstnId>\n";
    std::cout << "          <BICFI>" << instgAgtBIC << "</BICFI>\n";
    std::cout << "        </FinInstnId>\n";
    std::cout << "      </InstgAgt>\n";
    std::cout << "      <InstdAgt>\n";
    std::cout << "        <FinInstnId>\n";
    std::cout << "          <BICFI>" << instdAgtBIC << "</BICFI>\n";
    std::cout << "        </FinInstnId>\n";
    std::cout << "      </InstdAgt>\n";
    std::cout << "      <Dbtr>\n";
    std::cout << "        <Nm>" << dbtrNm << "</Nm>\n";
    std::cout << "        <Id>\n";
    std::cout << "          <OrgId>\n";
    std::cout << "            <AnyBIC>" << dbtrId << "</AnyBIC>\n";
    std::cout << "          </OrgId>\n";
    std::cout << "        </Id>\n";
    std::cout << "      </Dbtr>\n";
    std::cout << "      <DbtrAcct>\n";
    std::cout << "        <Id>\n";
    std::cout << "          <IBAN>" << dbtrAcct << "</IBAN>\n";
    std::cout << "        </Id>\n";
    std::cout << "      </DbtrAcct>\n";
    std::cout << "      <DbtrAgt>\n";
    std::cout << "        <FinInstnId>\n";
    std::cout << "          <BICFI>" << dbtrAgtBIC << "</BICFI>\n";
    std::cout << "        </FinInstnId>\n";
    std::cout << "      </DbtrAgt>\n";
    std::cout << "      <CdtrAgt>\n";
    std::cout << "        <FinInstnId>\n";
    std::cout << "          <BICFI>" << cdtrAgtBIC << "</BICFI>\n";
    std::cout << "        </FinInstnId>\n";
    std::cout << "      </CdtrAgt>\n";
    std::cout << "      <Cdtr>\n";
    std::cout << "        <Nm>" << cdtrNm << "</Nm>\n";
    std::cout << "        <Id>\n";
    std::cout << "          <OrgId>\n";
    std::cout << "            <AnyBIC>" << cdtrId << "</AnyBIC>\n";
    std::cout << "          </OrgId>\n";
    std::cout << "        </Id>\n";
    std::cout << "      </Cdtr>\n";
    std::cout << "      <CdtrAcct>\n";
    std::cout << "        <Id>\n";
    std::cout << "          <IBAN>" << cdtrAcct << "</IBAN>\n";
    std::cout << "        </Id>\n";
    std::cout << "      </CdtrAcct>\n";
    std::cout << "      <RmtInf>\n";
    std::cout << "        <Strd>\n";
    std::cout << "          <CdtrRefInf>\n";
    std::cout << "            <Tp>\n";
    std::cout << "              <CdOrPrtry>\n";
    std::cout << "                <Cd>SCOR</Cd>\n";
    std::cout << "              </CdOrPrtry>\n";
    std::cout << "              <Issr>" << issuer << "</Issr>\n";
    std::cout << "            </Tp>\n";
    std::cout << "            <Ref>" << creditorRef << "</Ref>\n";
    std::cout << "          </CdtrRefInf>\n";
    std::cout << "        </Strd>\n";
    std::cout << "      </RmtInf>\n";
    std::cout << "    </CdtTrfTxInf>\n";
  }

  std::cout << "  </FIToFICstmrCdtTrf>\n";
  std::cout << "</Document>\n";
}

int main(int argc, char* argv[])
{
  std::vector<str_t> args(argv, argv + argc); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  if (argc != 2)
  {
    std::cerr << "Usage: " << (argc > 0 ? args[0] : "xml_generator") << " <number_of_transactions>\n";
    std::cerr << "Description: Generates an ISO 20022 pacs.008 XML message with the specified number of transactions.\n";
    std::cerr << "Parameter: number_of_transactions - A positive integer (1 or more)\n";
    return 1;
  }

  try
  {
    int numTransactions = std::stoi(args[1]);
    if (numTransactions <= 0)
    {
      std::cerr << "Error: Number of transactions must be a positive integer.\n";
      std::cerr << "Usage: " << (argc > 0 ? args[0] : "xml_generator") << " <number_of_transactions>\n";
      return 1;
    }

    generateXML(numTransactions);
  }
  catch (const std::invalid_argument&)
  {
    std::cerr << "Error: Invalid argument. Please provide a valid integer.\n";
    std::cerr << "Usage: " << (argc > 0 ? args[0] : "xml_generator") << " <number_of_transactions>\n";
    return 1;
  }
  catch (const std::out_of_range&)
  {
    std::cerr << "Error: Number out of range.\n";
    std::cerr << "Usage: " << (argc > 0 ? args[0] : "xml_generator") << " <number_of_transactions>\n";
    return 1;
  }

  return 0;
}
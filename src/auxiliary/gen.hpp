#pragma once
#include <string>
// Function to get current date in ISO format (YYYY-MM-DD)
std::string getCurrentDate();
// Function to get current date time in ISO format (YYYY-MM-DDThh:mm:ss.sssZ)
std::string getCurrentDateTime();
// Function to generate a random numeric string of given length
std::string randomNumericString(size_t length);
// Function to generate a random alphanumeric string of given length
std::string randomString(size_t length);
// Function to generate a random IBAN (starts with EE for Estonia)
std::string randomIBAN();
// Function to generate a random BICFI (8 or 11 characters)
std::string randomBICFI();
// Function to generate a random amount between min and max with 2 decimal places
std::string randomAmount(double min, double max);
// Function to generate a random RF Creditor Reference (ISO 11649)
std::string randomRFCreditorReference();
// Generate the complete XML document
void generateXML(int numTransactions);

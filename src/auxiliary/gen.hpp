#pragma once
#include <string>
using str_t = std::string;
// Function to get current date in ISO format (YYYY-MM-DD)
str_t getCurrentDate();
// Function to get current date time in ISO format (YYYY-MM-DDThh:mm:ss.sssZ)
str_t getCurrentDateTime();
// Function to generate a random numeric string of given length
str_t randomNumericString(size_t length);
// Function to generate a random alphanumeric string of given length
str_t randomString(size_t length);
// Function to generate a random IBAN (starts with EE for Estonia)
str_t randomIBAN();
// Function to generate a random BICFI (8 or 11 characters)
str_t randomBICFI();
// Function to generate a random amount between min and max with 2 decimal places
str_t randomAmount(double min, double max);
// Function to generate a random RF Creditor Reference (ISO 11649)
str_t randomRFCreditorReference();
// Generate the complete XML document
void generateXML(int numTransactions);

#pragma once

#include "openbooks/book.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ob {

// RFC 4180-style CSV parsing (quoted fields, doubled quotes, embedded newlines).
std::vector<std::vector<std::string>> parseCsv(const std::string& text);

struct CsvImportOptions {
    int dateColumn = 1;  // 1-based
    int descriptionColumn = 2;
    int amountColumn = 3;
    bool hasHeader = true;
    bool negate = false;                 // flip signs for banks that export money-in as negative
    std::optional<int> offsetAccountId;  // default: Uncategorized Income / Uncategorized Expense
};

struct CsvImportResult {
    int imported = 0;
    int skipped = 0;  // duplicates and zero-amount rows
    std::vector<int> transactionIds;
};

// Imports a bank or card statement into `accountId`. Positive amounts are money into the
// account. Rows already present (same date, description and amount) are skipped. Either every
// row is imported or, on error, the book is left unchanged.
CsvImportResult importCsv(Book& book, int accountId, std::string text, const CsvImportOptions& options);

}  // namespace ob

#pragma once

#include "openbooks/book.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ob {

enum class RowStyle { Normal, Section, Total, Spacer };

struct Row {
    std::vector<std::string> cells;
    RowStyle style = RowStyle::Normal;
    int indent = 0;
};

// A rendered-agnostic report: the same Table prints as aligned text or as CSV.
struct Table {
    std::string title;
    std::vector<std::string> subtitles;
    std::vector<std::string> headers;
    std::vector<bool> numeric;  // right-align these columns
    std::vector<Row> rows;

    void add(std::vector<std::string> cells, RowStyle style = RowStyle::Normal, int indent = 0);
    void spacer();
    // First row whose first cell equals `label`, or nullptr.
    const Row* find(std::string_view label) const;
};

std::string renderText(const Table& t);
std::string renderCsv(const Table& t);

std::string accountLabel(const Account& a);
std::string describePeriod(const Period& p);

// ---- Lists
Table accountList(const Book& b, bool includeInactive);
Table contactList(const Book& b, ContactKind kind, bool includeInactive);
Table itemList(const Book& b, bool includeInactive);
Table documentList(const Book& b, DocKind kind, bool openOnly, int contactId, Date today);
Table paymentList(const Book& b, std::optional<PaymentKind> kind, int contactId);
Table transactionList(const Book& b, const Period& period, int accountId);
Table transactionDetail(const Book& b, int txnId);
Table accountRegister(const Book& b, int accountId, const Period& period);
Table reconciliationWorksheet(const Book& b, int accountId, Date through);

// ---- Financial reports
Table trialBalance(const Book& b, Date asOf);
Table balanceSheet(const Book& b, Date asOf);
Table profitAndLoss(const Book& b, Date from, Date to);
Table agingReport(const Book& b, DocKind kind, Date asOf);
Table journalReport(const Book& b, const Period& period);

// Printable invoice or bill.
std::string renderDocument(const Book& b, int documentId);

}  // namespace ob

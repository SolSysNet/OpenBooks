#include "openbooks/import.hpp"

#include "openbooks/util.hpp"

#include <algorithm>

namespace ob {
namespace {

bool isDuplicate(const Book& b, int accountId, Date date, Money amount, const std::string& payee) {
    for (const auto& t : b.transactions()) {
        if (t.voided || t.date != date || t.payee != payee) continue;
        for (const auto& s : t.splits) {
            if (s.accountId == accountId && s.amount == amount) return true;
        }
    }
    return false;
}

int findByName(const Book& b, const char* name) {
    for (const auto& a : b.accounts()) {
        if (iequals(a.name, name)) return a.id;
    }
    return 0;
}

}  // namespace

std::vector<std::vector<std::string>> parseCsv(const std::string& text) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool inQuotes = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field += '"';
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                field += c;
            }
        } else if (c == '"') {
            inQuotes = true;
        } else if (c == ',') {
            row.push_back(field);
            field.clear();
        } else if (c == '\n') {
            row.push_back(field);
            field.clear();
            rows.push_back(std::move(row));
            row.clear();
        } else if (c != '\r') {
            field += c;
        }
    }
    if (!field.empty() || !row.empty()) {
        row.push_back(field);
        rows.push_back(std::move(row));
    }
    return rows;
}

CsvImportResult importCsv(Book& book, int accountId, std::string text, const CsvImportOptions& options) {
    if (startsWith(text, "\xEF\xBB\xBF")) text.erase(0, 3);
    const auto rows = parseCsv(text);
    book.account(accountId);  // must exist

    int inflowOffset = 0;
    int outflowOffset = 0;
    if (options.offsetAccountId) {
        inflowOffset = outflowOffset = *options.offsetAccountId;
    } else {
        inflowOffset = findByName(book, "Uncategorized Income");
        outflowOffset = findByName(book, "Uncategorized Expense");
    }

    const int maxColumn = std::max({options.dateColumn, options.descriptionColumn, options.amountColumn});
    if (std::min({options.dateColumn, options.descriptionColumn, options.amountColumn}) < 1)
        throw Error("CSV column numbers start at 1");
    const auto needed = static_cast<std::size_t>(maxColumn);
    auto column = [](const std::vector<std::string>& r, int col) -> const std::string& {
        return r[static_cast<std::size_t>(col - 1)];
    };

    Book draft = book;  // all-or-nothing
    CsvImportResult result;
    for (std::size_t i = options.hasHeader ? 1 : 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        if (r.empty() || (r.size() == 1 && trim(r[0]).empty())) continue;
        const std::string rowLabel = "CSV row " + std::to_string(i + 1);
        if (r.size() < needed) throw Error(rowLabel + " has only " + std::to_string(r.size()) + " columns");
        const auto date = Date::parse(column(r, options.dateColumn));
        if (!date) throw Error(rowLabel + ": invalid date '" + column(r, options.dateColumn) + "'");
        const auto parsed = Money::parse(column(r, options.amountColumn));
        if (!parsed) throw Error(rowLabel + ": invalid amount '" + column(r, options.amountColumn) + "'");
        const Money amount = options.negate ? -*parsed : *parsed;
        const std::string payee = trim(column(r, options.descriptionColumn));
        if (amount.isZero() || isDuplicate(draft, accountId, *date, amount, payee)) {
            ++result.skipped;
            continue;
        }
        const int offset = amount > Money() ? inflowOffset : outflowOffset;
        if (offset == 0)
            throw Error(std::string("there is no '") + (amount > Money() ? "Uncategorized Income" : "Uncategorized Expense") +
                        "' account; choose an offset account");
        Transaction t;
        t.date = *date;
        t.kind = TxnKind::Import;
        t.payee = payee;
        t.splits = {Split{accountId, amount, "", SplitState::Uncleared}, Split{offset, -amount, "", SplitState::Uncleared}};
        result.transactionIds.push_back(draft.postTransaction(std::move(t)));
        ++result.imported;
    }
    book = std::move(draft);
    return result;
}

}  // namespace ob

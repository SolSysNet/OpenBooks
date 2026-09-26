#include "openbooks/reports.hpp"

#include "openbooks/util.hpp"

#include <algorithm>
#include <cctype>
#include <array>
#include <map>
#include <set>
#include <sstream>

namespace ob {
namespace {

Money lookup(const std::map<int, Money>& m, int id) {
    const auto it = m.find(id);
    return it == m.end() ? Money() : it->second;
}

std::vector<const Account*> sortedAccounts(const Book& b) {
    std::vector<const Account*> v;
    for (const auto& a : b.accounts()) v.push_back(&a);
    std::stable_sort(v.begin(), v.end(), [](const Account* x, const Account* y) {
        if (x->type != y->type) return static_cast<int>(x->type) < static_cast<int>(y->type);
        if (x->number != y->number) {
            if (x->number.empty()) return false;
            if (y->number.empty()) return true;
            return x->number < y->number;
        }
        return toLower(x->name) < toLower(y->name);
    });
    return v;
}

std::vector<const Transaction*> sortedTransactions(const Book& b) {
    std::vector<const Transaction*> v;
    for (const auto& t : b.transactions()) v.push_back(&t);
    std::stable_sort(v.begin(), v.end(), [](const Transaction* x, const Transaction* y) {
        return x->date != y->date ? x->date < y->date : x->id < y->id;
    });
    return v;
}

// Net income is the credit balance of income and expense accounts combined.
Money netIncome(const Book& b, const std::map<int, Money>& balances) {
    Money raw;
    for (const auto& a : b.accounts()) {
        if (a.type == AccountType::Income || a.type == AccountType::Expense) raw += lookup(balances, a.id);
    }
    return -raw;
}

const char* clearedMark(SplitState s) {
    switch (s) {
        case SplitState::Cleared: return "c";
        case SplitState::Reconciled: return "R";
        default: return "";
    }
}

std::string offsetAccounts(const Book& b, const Transaction& t, int accountId) {
    std::set<int> others;
    for (const auto& s : t.splits) {
        if (s.accountId != accountId) others.insert(s.accountId);
    }
    if (others.empty()) return "";
    if (others.size() > 1) return "-split-";
    return b.account(*others.begin()).name;
}

std::string payeeAndMemo(const std::string& payee, const std::string& memo) {
    if (payee.empty()) return memo;
    if (memo.empty()) return payee;
    return payee + " - " + memo;
}

std::string csvQuote(const std::string& s) {
    if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + "\"";
}

}  // namespace

// ------------------------------------------------------------------ tables

void Table::add(std::vector<std::string> cells, RowStyle style, int indent) {
    rows.push_back(Row{std::move(cells), style, indent});
}

void Table::spacer() { rows.push_back(Row{{}, RowStyle::Spacer, 0}); }

const Row* Table::find(std::string_view label) const {
    for (const auto& r : rows) {
        if (!r.cells.empty() && r.cells[0] == label) return &r;
    }
    return nullptr;
}

std::string renderText(const Table& t) {
    const std::size_t cols = t.headers.size();
    auto isNumeric = [&](std::size_t c) { return c < t.numeric.size() && t.numeric[c]; };
    auto cellText = [&](const Row& r, std::size_t c) {
        std::string s = c < r.cells.size() ? r.cells[c] : std::string();
        if (c == 0 && r.indent > 0) s.insert(0, static_cast<std::size_t>(r.indent) * 2, ' ');
        return s;
    };

    std::vector<std::size_t> width(cols, 0);
    for (std::size_t c = 0; c < cols; ++c) width[c] = displayWidth(t.headers[c]);
    for (const auto& r : t.rows) {
        if (r.style == RowStyle::Section) continue;  // section headings may overhang
        for (std::size_t c = 0; c < cols; ++c) width[c] = std::max(width[c], displayWidth(cellText(r, c)));
    }

    std::ostringstream out;
    auto printLine = [&](const std::vector<std::string>& cells) {
        std::string line;
        for (std::size_t c = 0; c < cols; ++c) {
            const std::string s = c < cells.size() ? cells[c] : std::string();
            const std::size_t w = displayWidth(s);
            const std::size_t pad = width[c] > w ? width[c] - w : 0;
            if (c) line += "  ";
            if (isNumeric(c)) line += std::string(pad, ' ') + s;
            else line += s + std::string(pad, ' ');
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out << line << '\n';
    };

    std::size_t total = 0;
    for (std::size_t c = 0; c < cols; ++c) total += width[c] + (c ? 2 : 0);

    if (!t.title.empty()) out << t.title << '\n';
    for (const auto& s : t.subtitles) out << s << '\n';
    if (!t.title.empty() || !t.subtitles.empty()) out << '\n';
    printLine(t.headers);
    out << std::string(total, '-') << '\n';
    for (const auto& r : t.rows) {
        std::vector<std::string> cells;
        for (std::size_t c = 0; c < cols; ++c) cells.push_back(cellText(r, c));
        switch (r.style) {
            case RowStyle::Spacer:
                out << '\n';
                break;
            case RowStyle::Section:
                out << cells[0] << '\n';
                break;
            case RowStyle::Total: {
                std::vector<std::string> rule;
                for (std::size_t c = 0; c < cols; ++c) {
                    const bool underline = isNumeric(c) && !cells[c].empty();
                    rule.push_back(underline ? std::string(width[c], '-') : "");
                }
                printLine(rule);
                printLine(cells);
                break;
            }
            case RowStyle::Normal:
                printLine(cells);
                break;
        }
    }
    if (t.rows.empty()) out << "(nothing to show)\n";
    return out.str();
}

std::string renderCsv(const Table& t) {
    std::ostringstream out;
    auto printLine = [&](const std::vector<std::string>& cells) {
        for (std::size_t c = 0; c < t.headers.size(); ++c) {
            if (c) out << ',';
            std::string cell = c < cells.size() ? cells[c] : std::string();
            if (c < t.numeric.size() && t.numeric[c] && &cells != &t.headers)  // plain numbers for spreadsheets
                cell.erase(std::remove(cell.begin(), cell.end(), ','), cell.end());
            out << csvQuote(cell);
        }
        out << '\n';
    };
    printLine(t.headers);
    for (const auto& r : t.rows) {
        if (r.style != RowStyle::Spacer) printLine(r.cells);
    }
    return out.str();
}

std::string accountLabel(const Account& a) { return a.number.empty() ? a.name : a.number + " " + a.name; }

std::string describePeriod(const Period& p) {
    if (p.from && p.to) return p.from->str() + " through " + p.to->str();
    if (p.to) return "Through " + p.to->str();
    if (p.from) return "From " + p.from->str();
    return "All dates";
}

// ------------------------------------------------------------------- lists

Table accountList(const Book& b, bool includeInactive) {
    Table t;
    t.title = "Chart of Accounts";
    t.headers = {"Number", "Name", "Type", "Balance", "Notes"};
    t.numeric = {false, false, false, true, false};
    const auto bal = b.balances(Period{});
    const Company& co = b.company;
    for (const Account* a : sortedAccounts(b)) {
        if (!a->active && !includeInactive) continue;
        std::string note;
        if (a->id == co.receivablesAccountId) note = "A/R";
        else if (a->id == co.payablesAccountId) note = "A/P";
        else if (a->id == co.salesTaxAccountId) note = "sales tax";
        else if (a->id == co.retainedEarningsAccountId) note = "retained earnings";
        if (!a->active) note = note.empty() ? "inactive" : note + ", inactive";
        t.add({a->number, a->name, toString(a->type), naturalSign(a->type, lookup(bal, a->id)).formatted(), note});
    }
    return t;
}

Table contactList(const Book& b, ContactKind kind, bool includeInactive) {
    Table t;
    t.title = kind == ContactKind::Customer ? "Customers" : "Vendors";
    t.headers = {"ID", "Name", "Email", "Phone", "Terms", "Open Balance"};
    t.numeric = {true, false, false, false, false, true};
    Money total;
    for (const auto& c : b.contacts()) {
        if (c.kind != kind || (!c.active && !includeInactive)) continue;
        const Money bal = b.contactBalance(c.id);
        total += bal;
        const std::string terms = c.termsDays < 0 ? "default" : "Net " + std::to_string(c.termsDays);
        t.add({"#" + std::to_string(c.id), c.name + (c.active ? "" : " (inactive)"), c.email, c.phone, terms,
               bal.formatted()});
    }
    t.add({"Total", "", "", "", "", total.formatted()}, RowStyle::Total);
    return t;
}

Table itemList(const Book& b, bool includeInactive) {
    Table t;
    t.title = "Products & Services";
    t.headers = {"ID", "Name", "Description", "Price", "Income Account", "Expense Account", "Taxable"};
    t.numeric = {true, false, false, true, false, false, false};
    for (const auto& i : b.items()) {
        if (!i.active && !includeInactive) continue;
        t.add({"#" + std::to_string(i.id), i.name + (i.active ? "" : " (inactive)"), i.description,
               i.price.formatted(), i.incomeAccountId ? b.account(i.incomeAccountId).name : "",
               i.expenseAccountId ? b.account(i.expenseAccountId).name : "", i.taxable ? "yes" : "no"});
    }
    return t;
}

std::string documentStatus(const Book& b, const Document& d, Date today) {
    if (d.voided) return "Void";
    const Money balance = b.documentBalance(d.id);
    switch (d.kind) {
        case DocKind::Estimate: {
            const EstimateStatus s = b.estimateStatus(d.id);
            if (s == EstimateStatus::Pending && d.dueDate < today) return "Expired";
            return toString(s);
        }
        case DocKind::SalesReceipt: return "Paid";
        case DocKind::CreditMemo:
            if (balance.isZero()) return "Applied";
            return balance < d.total() ? "Partly applied" : "Unapplied";
        default:
            if (balance.isZero()) return "Paid";
            if (d.dueDate < today) return "Overdue";
            return balance < d.total() ? "Partial" : "Open";
    }
}

bool documentIsOpen(const Book& b, const Document& d) {
    if (d.voided) return false;
    if (d.kind == DocKind::Estimate) {
        const EstimateStatus s = b.estimateStatus(d.id);
        return s == EstimateStatus::Pending || s == EstimateStatus::Accepted;
    }
    return !b.documentBalance(d.id).isZero();
}

const char* documentListTitle(DocKind kind) {
    switch (kind) {
        case DocKind::Invoice: return "Invoices";
        case DocKind::Bill: return "Bills";
        case DocKind::Estimate: return "Estimates";
        case DocKind::CreditMemo: return "Credit Memos";
        case DocKind::SalesReceipt: return "Sales Receipts";
    }
    return "Documents";
}

Table documentList(const Book& b, DocKind kind, bool openOnly, int contactId, Date today) {
    Table t;
    t.title = documentListTitle(kind);
    if (openOnly) t.title += " (open)";
    const char* dueHeader = kind == DocKind::Estimate ? "Expires" : docHasDueDate(kind) ? "Due" : "";
    const char* balanceHeader = kind == DocKind::CreditMemo ? "Credit Left" : "Balance";
    t.headers = {"Number", "Date", dueHeader, partyKind(kind) == ContactKind::Customer ? "Customer" : "Vendor",
                 "Total", balanceHeader, "Status"};
    t.numeric = {false, false, false, false, true, true, false};
    Money total;
    Money open;
    for (const auto& d : b.documents()) {
        if (d.kind != kind || (contactId && d.contactId != contactId)) continue;
        if (openOnly && !documentIsOpen(b, d)) continue;
        const Money balance = b.documentBalance(d.id);
        const Money docTotal = d.voided ? Money() : d.total();
        total += docTotal;
        open += balance;
        t.add({d.number, d.date.str(), docHasDueDate(kind) ? d.dueDate.str() : "", b.contact(d.contactId).name,
               docTotal.formatted(), balance.formatted(), documentStatus(b, d, today)});
    }
    t.add({"Total", "", "", "", total.formatted(), open.formatted(), ""}, RowStyle::Total);
    return t;
}

Table recurringList(const Book& b, Date today) {
    Table t;
    t.title = "Recurring Invoices";
    t.headers = {"Name", "Customer", "Schedule", "Next", "Created", "Amount", "Status"};
    t.numeric = {false, false, false, false, true, true, false};
    for (const auto& r : b.recurringInvoices()) {
        std::string status = !r.active ? "Paused" : r.finished() ? "Finished" : r.nextDate() <= today ? "Due" : "Active";
        t.add({r.name, b.contact(r.contactId).name, r.scheduleText(), r.finished() ? "" : r.nextDate().str(),
               std::to_string(r.occurrencesCreated), r.total().formatted(), status});
    }
    return t;
}

Table paymentList(const Book& b, std::optional<PaymentKind> kind, int contactId) {
    Table t;
    t.title = "Payments";
    t.headers = {"ID", "Date", "Type", "Name", "Account", "Amount", "Unapplied", "Ref", "Applied To"};
    t.numeric = {true, false, false, false, false, true, true, false, false};
    for (const auto& p : b.payments()) {
        if ((kind && p.kind != *kind) || (contactId && p.contactId != contactId)) continue;
        std::vector<std::string> applied;
        for (const auto& a : p.applications) {
            applied.push_back(b.document(a.documentId).number + " (" + a.amount.formatted() + ")");
        }
        t.add({"#" + std::to_string(p.id), p.date.str(),
               std::string(p.kind == PaymentKind::Received ? "Received" : "Paid") + (p.voided ? " (void)" : ""),
               b.contact(p.contactId).name, b.account(p.accountId).name, p.amount.formatted(),
               p.voided ? "" : p.unapplied().formatted(), p.ref, join(applied, ", ")});
    }
    return t;
}

Table transactionList(const Book& b, const Period& period, int accountId) {
    Table t;
    t.title = "Transactions";
    t.subtitles = {describePeriod(period)};
    if (accountId) t.subtitles.push_back("Account: " + accountLabel(b.account(accountId)));
    t.headers = {"Txn", "Date", "Type", "Ref", "Payee", "Memo", "Amount", "Status"};
    t.numeric = {true, false, false, false, false, false, true, false};
    for (const Transaction* tx : sortedTransactions(b)) {
        if (!period.contains(tx->date)) continue;
        if (accountId && std::none_of(tx->splits.begin(), tx->splits.end(),
                                      [&](const Split& s) { return s.accountId == accountId; }))
            continue;
        t.add({"#" + std::to_string(tx->id), tx->date.str(), toString(tx->kind), tx->ref, tx->payee, tx->memo,
               tx->debitTotal().formatted(), tx->voided ? "VOID" : ""});
    }
    return t;
}

Table transactionDetail(const Book& b, int txnId) {
    const Transaction& tx = b.transaction(txnId);
    Table t;
    t.title = "Transaction #" + std::to_string(tx.id) + (tx.voided ? "  ** VOID **" : "");
    t.subtitles.push_back("Date:  " + tx.date.str());
    t.subtitles.push_back(std::string("Type:  ") + toString(tx.kind));
    if (!tx.ref.empty()) t.subtitles.push_back("Ref:   " + tx.ref);
    if (!tx.payee.empty()) t.subtitles.push_back("Payee: " + tx.payee);
    if (!tx.memo.empty()) t.subtitles.push_back("Memo:  " + tx.memo);
    t.headers = {"Account", "Debit", "Credit", "Memo", "Clr"};
    t.numeric = {false, true, true, false, false};
    Money debits;
    Money credits;
    for (const auto& s : tx.splits) {
        const bool debit = s.amount > Money();
        if (debit) debits += s.amount;
        else credits -= s.amount;
        t.add({accountLabel(b.account(s.accountId)), debit ? s.amount.formatted() : "",
               debit ? "" : (-s.amount).formatted(), s.memo, clearedMark(s.state)});
    }
    t.add({"Total", debits.formatted(), credits.formatted(), "", ""}, RowStyle::Total);
    return t;
}

Table accountRegister(const Book& b, int accountId, const Period& period) {
    const Account& a = b.account(accountId);
    Table t;
    t.title = b.company.name;
    t.subtitles = {"Register: " + accountLabel(a), describePeriod(period)};
    t.headers = {"Date", "Txn", "Type", "Ref", "Payee / Memo", "Offset Account", "Amount", "Balance", "Clr"};
    t.numeric = {false, true, false, false, false, false, true, true, false};
    Money running;
    if (period.from) {
        running = naturalSign(a.type, b.balance(accountId, Period{std::nullopt, period.from->addDays(-1)}));
        t.add({period.from->str(), "", "", "", "Opening balance", "", "", running.formatted(), ""});
    }
    for (const Transaction* tx : sortedTransactions(b)) {
        if (tx->voided || !period.contains(tx->date)) continue;
        for (const auto& s : tx->splits) {
            if (s.accountId != accountId) continue;
            const Money amount = naturalSign(a.type, s.amount);
            running += amount;
            t.add({tx->date.str(), "#" + std::to_string(tx->id), toString(tx->kind), tx->ref,
                   payeeAndMemo(tx->payee, s.memo.empty() ? tx->memo : s.memo), offsetAccounts(b, *tx, accountId),
                   amount.formatted(), running.formatted(), clearedMark(s.state)});
        }
    }
    t.add({"Ending balance", "", "", "", "", "", "", running.formatted(), ""}, RowStyle::Total);
    return t;
}

Table reconciliationWorksheet(const Book& b, int accountId, Date through) {
    const Account& a = b.account(accountId);
    Table t;
    t.title = "Reconcile: " + accountLabel(a);
    t.subtitles = {"Unreconciled activity through " + through.str() + "  (c = cleared)"};
    t.headers = {"Clr", "Txn", "Date", "Type", "Payee / Memo", "Amount"};
    t.numeric = {false, true, false, false, false, true};
    for (const Transaction* tx : sortedTransactions(b)) {
        if (tx->voided || tx->date > through) continue;
        for (const auto& s : tx->splits) {
            if (s.accountId != accountId || s.state == SplitState::Reconciled) continue;
            t.add({clearedMark(s.state), "#" + std::to_string(tx->id), tx->date.str(), toString(tx->kind),
                   payeeAndMemo(tx->payee, tx->memo), naturalSign(a.type, s.amount).formatted()});
        }
    }
    return t;
}

// -------------------------------------------------------- financial reports

Table trialBalance(const Book& b, Date asOf) {
    Table t;
    t.title = b.company.name;
    t.subtitles = {"Trial Balance", "As of " + asOf.str()};
    t.headers = {"Account", "Debit", "Credit"};
    t.numeric = {false, true, true};
    const auto bal = b.balances(Period{std::nullopt, asOf});
    Money debits;
    Money credits;
    for (const Account* a : sortedAccounts(b)) {
        const Money m = lookup(bal, a->id);
        if (m.isZero()) continue;
        if (m > Money()) {
            t.add({accountLabel(*a), m.formatted(), ""});
            debits += m;
        } else {
            t.add({accountLabel(*a), "", (-m).formatted()});
            credits -= m;
        }
    }
    t.add({"Total", debits.formatted(), credits.formatted()}, RowStyle::Total);
    return t;
}

Table balanceSheet(const Book& b, Date asOf) {
    Table t;
    t.title = b.company.name;
    t.subtitles = {"Balance Sheet", "As of " + asOf.str()};
    t.headers = {"Account", "Balance"};
    t.numeric = {false, true};

    const auto bal = b.balances(Period{std::nullopt, asOf});
    const Date yearStart = b.fiscalYearStart(asOf);
    const Money priorEarnings = netIncome(b, b.balances(Period{std::nullopt, yearStart.addDays(-1)}));
    const Money currentEarnings = netIncome(b, bal) - priorEarnings;
    const auto accounts = sortedAccounts(b);
    const int reId = b.company.retainedEarningsAccountId;

    auto section = [&](AccountType type, const char* heading) {
        t.add({heading}, RowStyle::Section);
        Money total;
        for (const Account* a : accounts) {
            if (a->type != type) continue;
            Money m = naturalSign(type, lookup(bal, a->id));
            if (a->id == reId) m += priorEarnings;  // income from closed years rolls into retained earnings
            if (m.isZero()) continue;
            t.add({accountLabel(*a), m.formatted()}, RowStyle::Normal, 1);
            total += m;
        }
        return total;
    };

    const Money assets = section(AccountType::Asset, "Assets");
    t.add({"Total Assets", assets.formatted()}, RowStyle::Total);
    t.spacer();
    const Money liabilities = section(AccountType::Liability, "Liabilities");
    t.add({"Total Liabilities", liabilities.formatted()}, RowStyle::Total);
    t.spacer();
    Money equity = section(AccountType::Equity, "Equity");
    if (reId == 0 && !priorEarnings.isZero()) {
        t.add({"Retained Earnings", priorEarnings.formatted()}, RowStyle::Normal, 1);
        equity += priorEarnings;
    }
    if (!currentEarnings.isZero()) {
        t.add({"Net Income", currentEarnings.formatted()}, RowStyle::Normal, 1);
        equity += currentEarnings;
    }
    t.add({"Total Equity", equity.formatted()}, RowStyle::Total);
    t.spacer();
    t.add({"Total Liabilities & Equity", (liabilities + equity).formatted()}, RowStyle::Total);
    if (assets != liabilities + equity)
        t.subtitles.push_back("WARNING: out of balance by " + (assets - liabilities - equity).formatted());
    return t;
}

Table profitAndLoss(const Book& b, Date from, Date to) {
    Table t;
    t.title = b.company.name;
    t.subtitles = {"Profit and Loss", describePeriod(Period{from, to})};
    t.headers = {"Account", "Amount"};
    t.numeric = {false, true};
    const auto bal = b.balances(Period{from, to});
    const auto accounts = sortedAccounts(b);

    auto section = [&](AccountType type, const char* heading) {
        t.add({heading}, RowStyle::Section);
        Money total;
        for (const Account* a : accounts) {
            if (a->type != type) continue;
            const Money m = naturalSign(type, lookup(bal, a->id));
            if (m.isZero()) continue;
            t.add({accountLabel(*a), m.formatted()}, RowStyle::Normal, 1);
            total += m;
        }
        return total;
    };

    const Money income = section(AccountType::Income, "Income");
    t.add({"Total Income", income.formatted()}, RowStyle::Total);
    t.spacer();
    const Money expenses = section(AccountType::Expense, "Expenses");
    t.add({"Total Expenses", expenses.formatted()}, RowStyle::Total);
    t.spacer();
    t.add({"Net Income", (income - expenses).formatted()}, RowStyle::Total);
    return t;
}

Table agingReport(const Book& b, DocKind kind, Date asOf) {
    const bool invoice = kind == DocKind::Invoice;
    Table t;
    t.title = b.company.name;
    t.subtitles = {invoice ? "Accounts Receivable Aging" : "Accounts Payable Aging", "As of " + asOf.str()};
    t.headers = {invoice ? "Customer" : "Vendor", "Current", "1-30", "31-60", "61-90", "Over 90", "Total"};
    t.numeric = {false, true, true, true, true, true, true};

    using Buckets = std::array<Money, 6>;
    std::map<int, Buckets> byContact;
    for (const auto& d : b.documents()) {
        if (d.kind != kind || d.voided || d.date > asOf) continue;
        const Money due = b.documentBalance(d.id, asOf);
        if (due.isZero()) continue;
        const int late = asOf - d.dueDate;
        const std::size_t bucket = late <= 0 ? 0 : late <= 30 ? 1 : late <= 60 ? 2 : late <= 90 ? 3 : 4;
        Buckets& row = byContact[d.contactId];
        row[bucket] += due;
        row[5] += due;
    }
    const PaymentKind payKind = invoice ? PaymentKind::Received : PaymentKind::Paid;
    for (const auto& p : b.payments()) {
        if (p.kind != payKind || p.voided || p.date > asOf) continue;
        const Money credit = p.unapplied();
        if (credit.isZero()) continue;
        Buckets& row = byContact[p.contactId];
        row[0] -= credit;  // unapplied payments are shown as credits in the current column
        row[5] -= credit;
    }
    if (invoice) {  // so are credit memos not yet applied to an invoice
        for (const auto& cm : b.documents()) {
            if (cm.kind != DocKind::CreditMemo || cm.voided || cm.date > asOf) continue;
            const Money credit = b.documentBalance(cm.id);
            if (credit.isZero()) continue;
            Buckets& row = byContact[cm.contactId];
            row[0] -= credit;
            row[5] -= credit;
        }
    }

    std::vector<std::pair<std::string, Buckets>> rows;
    for (const auto& [contactId, buckets] : byContact) rows.emplace_back(b.contact(contactId).name, buckets);
    std::sort(rows.begin(), rows.end(), [](const auto& x, const auto& y) { return toLower(x.first) < toLower(y.first); });

    Buckets totals{};
    for (const auto& [name, buckets] : rows) {
        std::vector<std::string> cells{name};
        for (std::size_t i = 0; i < buckets.size(); ++i) {
            cells.push_back(buckets[i].formatted());
            totals[i] += buckets[i];
        }
        t.add(std::move(cells));
    }
    std::vector<std::string> cells{"Total"};
    for (const Money& m : totals) cells.push_back(m.formatted());
    t.add(std::move(cells), RowStyle::Total);
    return t;
}

Table journalReport(const Book& b, const Period& period) {
    Table t;
    t.title = b.company.name;
    t.subtitles = {"Journal", describePeriod(period)};
    t.headers = {"Date", "Txn", "Type", "Account / Description", "Debit", "Credit"};
    t.numeric = {false, true, false, false, true, true};
    Money debits;
    Money credits;
    for (const Transaction* tx : sortedTransactions(b)) {
        if (!period.contains(tx->date)) continue;
        std::string description = payeeAndMemo(tx->payee, tx->memo);
        if (!tx->ref.empty()) description = tx->ref + (description.empty() ? "" : "  " + description);
        if (tx->voided) description += "  ** VOID **";
        t.add({tx->date.str(), "#" + std::to_string(tx->id), toString(tx->kind), description, "", ""});
        if (tx->voided) continue;
        for (const auto& s : tx->splits) {
            const bool debit = s.amount > Money();
            if (debit) debits += s.amount;
            else credits -= s.amount;
            std::string label = "  " + accountLabel(b.account(s.accountId));
            if (!s.memo.empty()) label += " (" + s.memo + ")";
            t.add({"", "", "", label, debit ? s.amount.formatted() : "", debit ? "" : (-s.amount).formatted()});
        }
    }
    t.add({"Total", "", "", "", debits.formatted(), credits.formatted()}, RowStyle::Total);
    return t;
}

// --------------------------------------------------------------- documents

std::string renderDocument(const Book& b, int documentId) {
    const Document& d = b.document(documentId);
    const Contact& c = b.contact(d.contactId);
    std::ostringstream out;

    out << b.company.name << '\n';
    if (!b.company.address.empty()) out << b.company.address << '\n';
    out << '\n' << toUpper(docTitle(d.kind)) << " " << d.number << (d.voided ? "   ** VOID **" : "") << "\n\n";
    out << (d.kind == DocKind::Bill ? "Vendor:   " : "Customer: ") << c.name << '\n';
    if (!c.address.empty()) {
        for (const auto& line : split(c.address, '\n')) out << "          " << line << '\n';
    }
    if (!c.email.empty()) out << "          " << c.email << '\n';
    out << "Date:     " << d.date << '\n';
    if (d.kind == DocKind::Estimate) out << "Expires:  " << d.dueDate << "\nStatus:   " << documentStatus(b, d, Date::today()) << '\n';
    else if (docHasDueDate(d.kind)) out << "Due:      " << d.dueDate << '\n';
    if (d.kind == DocKind::SalesReceipt && d.depositAccountId) out << "Paid to:  " << b.account(d.depositAccountId).name << '\n';
    if (!d.memo.empty()) out << "Memo:     " << d.memo << '\n';
    out << '\n';

    Table t;
    t.headers = {"Description", "Account", "Qty", "Rate", "Amount"};
    t.numeric = {false, false, true, true, true};
    for (const auto& l : d.lines) {
        t.add({l.description, b.account(l.accountId).name, l.quantity.str(), l.rate.formatted(),
               l.amount.formatted()});
    }
    t.add({"Subtotal", "", "", "", d.subtotal().formatted()}, RowStyle::Total);
    if (!d.tax().isZero()) t.add({"Sales tax (" + d.taxRate.str() + "%)", "", "", "", d.tax().formatted()});
    t.add({"Total", "", "", "", d.total().formatted()});
    if (d.kind == DocKind::Invoice || d.kind == DocKind::Bill) {
        const Money paid = b.documentPaid(documentId);
        const Money credited = b.documentCredited(documentId);
        if (!paid.isZero()) t.add({"Payments applied", "", "", "", (-paid).formatted()});
        if (!credited.isZero()) t.add({"Credits applied", "", "", "", (-credited).formatted()});
        t.add({"Balance due", "", "", "", b.documentBalance(documentId).formatted()}, RowStyle::Total);
    } else if (d.kind == DocKind::CreditMemo) {
        for (const auto& a : d.applications)
            t.add({"Applied to invoice " + b.document(a.documentId).number, "", "", "", (-a.amount).formatted()});
        t.add({"Credit remaining", "", "", "", b.documentBalance(documentId).formatted()}, RowStyle::Total);
    }
    out << renderText(t);
    return out.str();
}

std::string toUpper(std::string_view s) {
    std::string out(s);
    for (char& ch : out) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return out;
}

}  // namespace ob

#include "openbooks/model.hpp"

#include "openbooks/util.hpp"

namespace ob {

const char* toString(AccountType t) {
    switch (t) {
        case AccountType::Asset: return "Asset";
        case AccountType::Liability: return "Liability";
        case AccountType::Equity: return "Equity";
        case AccountType::Income: return "Income";
        case AccountType::Expense: return "Expense";
    }
    return "?";
}

std::optional<AccountType> parseAccountType(std::string_view s) {
    const std::string l = toLower(trim(s));
    if (l == "asset") return AccountType::Asset;
    if (l == "liability") return AccountType::Liability;
    if (l == "equity") return AccountType::Equity;
    if (l == "income" || l == "revenue") return AccountType::Income;
    if (l == "expense") return AccountType::Expense;
    return std::nullopt;
}

bool isDebitNormal(AccountType t) { return t == AccountType::Asset || t == AccountType::Expense; }

Money naturalSign(AccountType t, Money raw) { return isDebitNormal(t) ? raw : -raw; }

const char* toString(TxnKind k) {
    switch (k) {
        case TxnKind::Journal: return "Journal";
        case TxnKind::Expense: return "Expense";
        case TxnKind::Deposit: return "Deposit";
        case TxnKind::Transfer: return "Transfer";
        case TxnKind::Import: return "Import";
        case TxnKind::Invoice: return "Invoice";
        case TxnKind::Bill: return "Bill";
        case TxnKind::CustomerPayment: return "Payment";
        case TxnKind::VendorPayment: return "BillPayment";
        case TxnKind::CreditMemo: return "CreditMemo";
        case TxnKind::SalesReceipt: return "SalesReceipt";
    }
    return "?";
}

std::optional<TxnKind> parseTxnKind(std::string_view s) {
    for (TxnKind k : {TxnKind::Journal, TxnKind::Expense, TxnKind::Deposit, TxnKind::Transfer, TxnKind::Import,
                      TxnKind::Invoice, TxnKind::Bill, TxnKind::CustomerPayment, TxnKind::VendorPayment,
                      TxnKind::CreditMemo, TxnKind::SalesReceipt}) {
        if (iequals(s, toString(k))) return k;
    }
    return std::nullopt;
}

bool isManualKind(TxnKind k) {
    switch (k) {
        case TxnKind::Journal:
        case TxnKind::Expense:
        case TxnKind::Deposit:
        case TxnKind::Transfer:
        case TxnKind::Import:
            return true;
        default:
            return false;
    }
}

Money Transaction::debitTotal() const {
    Money total;
    for (const auto& s : splits) {
        if (s.amount > Money()) total += s.amount;
    }
    return total;
}

const char* toString(ContactKind k) { return k == ContactKind::Customer ? "Customer" : "Vendor"; }

std::optional<ContactKind> parseContactKind(std::string_view s) {
    if (iequals(s, "customer")) return ContactKind::Customer;
    if (iequals(s, "vendor")) return ContactKind::Vendor;
    return std::nullopt;
}

const char* toString(DocKind k) {
    switch (k) {
        case DocKind::Invoice: return "Invoice";
        case DocKind::Bill: return "Bill";
        case DocKind::Estimate: return "Estimate";
        case DocKind::CreditMemo: return "CreditMemo";
        case DocKind::SalesReceipt: return "SalesReceipt";
    }
    return "?";
}

std::optional<DocKind> parseDocKind(std::string_view s) {
    for (DocKind k : {DocKind::Invoice, DocKind::Bill, DocKind::Estimate, DocKind::CreditMemo, DocKind::SalesReceipt}) {
        if (iequals(s, toString(k))) return k;
    }
    return std::nullopt;
}

const char* docTitle(DocKind k) {
    switch (k) {
        case DocKind::Invoice: return "Invoice";
        case DocKind::Bill: return "Bill";
        case DocKind::Estimate: return "Estimate";
        case DocKind::CreditMemo: return "Credit Memo";
        case DocKind::SalesReceipt: return "Sales Receipt";
    }
    return "?";
}

const char* docNoun(DocKind k) {
    switch (k) {
        case DocKind::Invoice: return "invoice";
        case DocKind::Bill: return "bill";
        case DocKind::Estimate: return "estimate";
        case DocKind::CreditMemo: return "credit memo";
        case DocKind::SalesReceipt: return "sales receipt";
    }
    return "?";
}

ContactKind partyKind(DocKind k) { return k == DocKind::Bill ? ContactKind::Vendor : ContactKind::Customer; }

bool docPosts(DocKind k) { return k != DocKind::Estimate; }

bool docHasDueDate(DocKind k) { return k == DocKind::Invoice || k == DocKind::Bill || k == DocKind::Estimate; }

const char* toString(EstimateStatus s) {
    switch (s) {
        case EstimateStatus::Pending: return "Pending";
        case EstimateStatus::Accepted: return "Accepted";
        case EstimateStatus::Declined: return "Declined";
        case EstimateStatus::Converted: return "Converted";
    }
    return "?";
}

std::optional<EstimateStatus> parseEstimateStatus(std::string_view s) {
    for (EstimateStatus v :
         {EstimateStatus::Pending, EstimateStatus::Accepted, EstimateStatus::Declined, EstimateStatus::Converted}) {
        if (iequals(s, toString(v))) return v;
    }
    return std::nullopt;
}

const char* toString(Frequency f) {
    switch (f) {
        case Frequency::Weekly: return "Weekly";
        case Frequency::Monthly: return "Monthly";
        case Frequency::Yearly: return "Yearly";
    }
    return "?";
}

std::optional<Frequency> parseFrequency(std::string_view s) {
    const std::string l = toLower(trim(s));
    if (l == "weekly" || l == "week" || l == "weeks") return Frequency::Weekly;
    if (l == "monthly" || l == "month" || l == "months") return Frequency::Monthly;
    if (l == "yearly" || l == "year" || l == "years" || l == "annually") return Frequency::Yearly;
    return std::nullopt;
}

const char* toString(PaymentKind k) { return k == PaymentKind::Received ? "Received" : "Paid"; }

std::optional<PaymentKind> parsePaymentKind(std::string_view s) {
    if (iequals(s, "received")) return PaymentKind::Received;
    if (iequals(s, "paid")) return PaymentKind::Paid;
    return std::nullopt;
}

Money Document::subtotal() const {
    Money s;
    for (const auto& l : lines) s += l.amount;
    return s;
}

Money Document::taxableSubtotal() const {
    Money s;
    for (const auto& l : lines) {
        if (l.taxable) s += l.amount;
    }
    return s;
}

Money Document::tax() const { return percentOf(taxableSubtotal(), taxRate); }

Money Document::total() const { return subtotal() + tax(); }

Money Document::applied() const {
    Money s;
    for (const auto& a : applications) s += a.amount;
    return s;
}

Date RecurringInvoice::occurrence(int k) const {
    switch (frequency) {
        case Frequency::Weekly: return startDate.addDays(7 * interval * k);
        case Frequency::Monthly: return startDate.addMonths(interval * k);
        case Frequency::Yearly: return startDate.addMonths(12 * interval * k);
    }
    return startDate;
}

std::string RecurringInvoice::scheduleText() const {
    const char* unit = frequency == Frequency::Weekly ? "week" : frequency == Frequency::Monthly ? "month" : "year";
    if (interval == 1) return std::string("Every ") + unit;
    return "Every " + std::to_string(interval) + " " + unit + "s";
}

Money RecurringInvoice::subtotal() const {
    Money s;
    for (const auto& l : lines) s += l.amount;
    return s;
}

Money RecurringInvoice::total() const {
    Money taxable;
    for (const auto& l : lines) {
        if (l.taxable) taxable += l.amount;
    }
    return subtotal() + percentOf(taxable, taxRate);
}

Money Payment::applied() const {
    Money s;
    for (const auto& a : applications) s += a.amount;
    return s;
}

Money Payment::unapplied() const { return amount - applied(); }

}  // namespace ob

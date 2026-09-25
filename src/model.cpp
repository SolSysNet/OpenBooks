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
    }
    return "?";
}

std::optional<TxnKind> parseTxnKind(std::string_view s) {
    for (TxnKind k : {TxnKind::Journal, TxnKind::Expense, TxnKind::Deposit, TxnKind::Transfer, TxnKind::Import,
                      TxnKind::Invoice, TxnKind::Bill, TxnKind::CustomerPayment, TxnKind::VendorPayment}) {
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

const char* toString(DocKind k) { return k == DocKind::Invoice ? "Invoice" : "Bill"; }

std::optional<DocKind> parseDocKind(std::string_view s) {
    if (iequals(s, "invoice")) return DocKind::Invoice;
    if (iequals(s, "bill")) return DocKind::Bill;
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

Money Payment::applied() const {
    Money s;
    for (const auto& a : applications) s += a.amount;
    return s;
}

Money Payment::unapplied() const { return amount - applied(); }

}  // namespace ob

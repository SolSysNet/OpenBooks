#pragma once

#include "openbooks/date.hpp"
#include "openbooks/money.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ob {

// ---------------------------------------------------------------- Accounts

enum class AccountType { Asset, Liability, Equity, Income, Expense };

const char* toString(AccountType t);
std::optional<AccountType> parseAccountType(std::string_view s);
// Assets and expenses increase with debits; everything else increases with credits.
bool isDebitNormal(AccountType t);
// Converts a raw (debit-positive) amount into the account type's natural sign.
Money naturalSign(AccountType t, Money raw);

struct Account {
    int id = 0;
    std::string number;  // optional, e.g. "1000"
    std::string name;
    AccountType type = AccountType::Asset;
    std::string description;
    bool active = true;
};

// ------------------------------------------------------------ Transactions

enum class SplitState { Uncleared, Cleared, Reconciled };

// One line of a journal transaction. Positive amounts are debits, negative are credits.
struct Split {
    int accountId = 0;
    Money amount;
    std::string memo;
    SplitState state = SplitState::Uncleared;
};

enum class TxnKind { Journal, Expense, Deposit, Transfer, Import, Invoice, Bill, CustomerPayment, VendorPayment };

const char* toString(TxnKind k);
std::optional<TxnKind> parseTxnKind(std::string_view s);
// Manual kinds are entered directly; the rest are owned by an invoice, bill or payment.
bool isManualKind(TxnKind k);

struct Transaction {
    int id = 0;
    Date date;
    TxnKind kind = TxnKind::Journal;
    std::string ref;
    std::string payee;
    std::string memo;
    std::vector<Split> splits;
    bool voided = false;

    Money debitTotal() const;
};

// ---------------------------------------------------------------- Contacts

enum class ContactKind { Customer, Vendor };

const char* toString(ContactKind k);
std::optional<ContactKind> parseContactKind(std::string_view s);

struct Contact {
    int id = 0;
    ContactKind kind = ContactKind::Customer;
    std::string name;
    std::string email;
    std::string phone;
    std::string address;
    int termsDays = -1;  // -1 = use the company default
    bool active = true;
};

// ------------------------------------------------------------------- Items

struct Item {
    int id = 0;
    std::string name;
    std::string description;
    Money price;
    int incomeAccountId = 0;   // used when the item is sold (invoices)
    int expenseAccountId = 0;  // used when the item is purchased (bills)
    bool taxable = true;
    bool active = true;
};

// --------------------------------------------------------- Invoices / Bills

enum class DocKind { Invoice, Bill };

const char* toString(DocKind k);
std::optional<DocKind> parseDocKind(std::string_view s);

struct DocLine {
    int itemId = 0;
    int accountId = 0;
    std::string description;
    Decimal quantity = Decimal::fromInt(1);
    Money rate;
    Money amount;  // computed: rate * quantity
    bool taxable = true;
};

struct Document {
    int id = 0;
    DocKind kind = DocKind::Invoice;
    std::string number;
    int contactId = 0;
    Date date;
    Date dueDate;
    std::vector<DocLine> lines;
    Decimal taxRate;  // percent, invoices only
    std::string memo;
    int txnId = 0;
    bool voided = false;

    Money subtotal() const;
    Money taxableSubtotal() const;
    Money tax() const;
    Money total() const;
};

// ---------------------------------------------------------------- Payments

enum class PaymentKind { Received, Paid };

const char* toString(PaymentKind k);
std::optional<PaymentKind> parsePaymentKind(std::string_view s);

struct Application {
    int documentId = 0;
    Money amount;
};

struct Payment {
    int id = 0;
    PaymentKind kind = PaymentKind::Received;
    int contactId = 0;
    Date date;
    int accountId = 0;  // bank / cash account the money moved through
    Money amount;
    std::string ref;
    std::string memo;
    std::vector<Application> applications;
    int txnId = 0;
    bool voided = false;

    Money applied() const;
    Money unapplied() const;
};

// ----------------------------------------------------------------- Company

struct Company {
    std::string name = "My Company";
    std::string address;
    int fiscalYearStartMonth = 1;
    int defaultTermsDays = 30;
    std::optional<Date> closedThrough;  // no changes allowed on or before this date
    int nextInvoiceNumber = 1001;
    int receivablesAccountId = 0;
    int payablesAccountId = 0;
    int salesTaxAccountId = 0;
    int retainedEarningsAccountId = 0;
};

}  // namespace ob

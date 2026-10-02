#pragma once

#include "openbooks/model.hpp"
#include "openplugin/store.hpp"

#include <iosfwd>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ob {

class FileKey;  // crypto.hpp

// Every validation failure in the ledger is reported as an ob::Error with a user-facing message.
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// An inclusive date range; a missing bound is open-ended.
struct Period {
    std::optional<Date> from;
    std::optional<Date> to;
    bool contains(Date d) const;
};

enum class SystemAccount { Receivables, Payables, SalesTax, RetainedEarnings };

// A company's complete set of books: chart of accounts, contacts, items, the general
// journal, and the invoices/bills/payments that post to it.
//
// Every mutation validates first and only then changes state, so a thrown ob::Error
// leaves the book untouched. Invoices, bills and payments own their journal entries;
// Accounts Receivable and Accounts Payable can only be changed through them, which keeps
// the sub-ledgers (open invoices / bills) in agreement with the general ledger.
class Book {
public:
    Company company;
    // Data that plugins keep in this file (PLUGIN records), saved and encrypted with it. Kept
    // as-is when the plugin isn't installed, so files round-trip without loss.
    opl::PluginStore plugins;

    // Adds a starter chart of accounts for a small service/retail business.
    void createDefaultChart();

    // ---- Collections (ordered by id)
    const std::vector<Account>& accounts() const { return accounts_; }
    const std::vector<Contact>& contacts() const { return contacts_; }
    const std::vector<Item>& items() const { return items_; }
    const std::vector<Transaction>& transactions() const { return transactions_; }
    const std::vector<Document>& documents() const { return documents_; }
    const std::vector<Payment>& payments() const { return payments_; }

    // ---- Lookup by id (throws if missing)
    const Account& account(int id) const;
    const Contact& contact(int id) const;
    const Item& item(int id) const;
    const Transaction& transaction(int id) const;
    const Document& document(int id) const;
    const Payment& payment(int id) const;

    // ---- Lookup by user reference: "#id", exact number, exact name (case-insensitive)
    //      or a unique name prefix. Throws if nothing (or more than one thing) matches.
    const Account& findAccount(std::string_view ref) const;
    const Contact& findContact(ContactKind kind, std::string_view ref) const;
    const Item& findItem(std::string_view ref) const;
    const Document& findDocument(DocKind kind, std::string_view ref) const;

    // ---- Setup
    int addAccount(Account a);
    void updateAccount(const Account& a);  // name, number, description, active (type only if unused)
    void setSystemAccount(SystemAccount role, int accountId);
    int addContact(Contact c);
    void updateContact(const Contact& c);
    int addItem(Item i);
    void setItemActive(int itemId, bool active);

    // ---- Journal
    int postTransaction(Transaction t);  // manual kinds only
    void voidTransaction(int txnId);     // manual kinds only
    void recategorize(int txnId, int fromAccountId, int toAccountId);

    // ---- Sales & purchases
    // Creates any document kind. Line amounts and the number are filled in. How it posts:
    //   Invoice       Dr A/R, Cr income lines, Cr sales tax
    //   Credit memo   Dr income lines, Dr sales tax, Cr A/R   (a customer credit)
    //   Sales receipt Dr deposit account, Cr income lines, Cr sales tax
    //   Bill          Dr expense lines, Cr A/P
    //   Estimate      nothing (not in the ledger until converted to an invoice)
    int createDocument(Document d);
    void voidDocument(int documentId);
    int recordPayment(Payment p);  // with no applications, pays the oldest open documents
    void voidPayment(int paymentId);

    // ---- Estimates
    int convertEstimate(int estimateId, Date invoiceDate);  // returns the new invoice id
    void setEstimateStatus(int estimateId, EstimateStatus status);  // Pending / Accepted / Declined

    // ---- Credit memos
    void applyCredit(int creditMemoId, int invoiceId, Money amount);
    void unapplyCredit(int creditMemoId, int invoiceId);

    // ---- Recurring invoices
    const std::vector<RecurringInvoice>& recurringInvoices() const { return recurring_; }
    const RecurringInvoice& recurringInvoice(int id) const;
    const RecurringInvoice& findRecurring(std::string_view ref) const;  // "#id" or name
    int addRecurring(RecurringInvoice r);
    void updateRecurring(const RecurringInvoice& r);  // keeps the count of invoices already created
    void deleteRecurring(int id);                     // invoices already created are kept
    // Creates every invoice due on or before `through`, oldest first. All or nothing.
    std::vector<int> createDueRecurringInvoices(Date through);
    int dueRecurringCount(Date through) const;

    // ---- Reconciliation
    void setSplitState(int txnId, int accountId, SplitState state);
    int finishReconciliation(int accountId);  // Cleared -> Reconciled, returns line count

    // ---- Queries
    std::map<int, Money> balances(const Period& period) const;  // raw, debit-positive
    Money balance(int accountId, const Period& period = {}) const;
    Money clearedBalance(int accountId) const;
    Money documentPaid(int documentId, std::optional<Date> asOf = std::nullopt) const;      // payments
    Money documentCredited(int documentId, std::optional<Date> asOf = std::nullopt) const;  // credit memos
    // Invoices/bills: amount still owed. Credit memos: credit not yet applied. Others: zero.
    Money documentBalance(int documentId, std::optional<Date> asOf = std::nullopt) const;
    Money contactBalance(int contactId) const;  // owed, less unapplied payments and credits
    // Estimate status, taking conversion into account.
    EstimateStatus estimateStatus(int estimateId) const;
    Date fiscalYearStart(Date d) const;
    bool isSubledgerAccount(int accountId) const;  // A/R or A/P
    bool isSystemAccount(int accountId) const;

    // ---- Persistence (storage.cpp). Paths are UTF-8.
    void write(std::ostream& out) const;
    static Book read(std::istream& in);
    // Atomic replace, keeping a .bak of the previous file. With a key the file is encrypted
    // (see crypto.hpp); the temporary file is encrypted too, so plaintext never touches disk.
    void save(const std::string& path, const FileKey* key = nullptr) const;
    // Encrypted files need the password: without one this throws PasswordRequiredError, with a
    // wrong one WrongPasswordError. `keyOut` receives the key for later saves.
    static Book load(const std::string& path, std::string_view password = {},
                     std::unique_ptr<FileKey>* keyOut = nullptr);
    // Reopens a file with a key obtained from an earlier load (no password prompt needed).
    static Book loadWithKey(const std::string& path, const FileKey& key);
    static bool isEncryptedFile(const std::string& path);

private:
    Account& accountMut(int id);
    Contact& contactMut(int id);
    Item& itemMut(int id);
    Transaction& txnMut(int id);
    Document& docMut(int id);
    Payment& paymentMut(int id);

    void validateAccountNames(const Account& a) const;
    void validateContact(const Contact& c) const;
    void validateTransaction(const Transaction& t) const;
    void checkOpenPeriod(Date d) const;
    int insertTransaction(Transaction t);
    void voidTransactionInternal(Transaction& t);
    bool accountInUse(int accountId) const;
    bool documentNumberTaken(DocKind kind, const std::string& number) const;
    void validateRecurring(const RecurringInvoice& r) const;
    RecurringInvoice& recurringMut(int id);

    std::vector<Account> accounts_;
    std::vector<Contact> contacts_;
    std::vector<Item> items_;
    std::vector<Transaction> transactions_;
    std::vector<Document> documents_;
    std::vector<Payment> payments_;
    std::vector<RecurringInvoice> recurring_;

    int nextRecurringId_ = 1;
    int nextAccountId_ = 1;
    int nextContactId_ = 1;
    int nextItemId_ = 1;
    int nextTxnId_ = 1;
    int nextDocId_ = 1;
    int nextPaymentId_ = 1;
};

}  // namespace ob

#include "openbooks/book.hpp"

#include "openbooks/util.hpp"

#include <algorithm>
#include <climits>
#include <set>

namespace ob {
namespace {

template <typename Vec>
auto findById(Vec& v, int id) -> decltype(&v[0]) {
    auto it = std::lower_bound(v.begin(), v.end(), id, [](const auto& x, int i) { return x.id < i; });
    return (it != v.end() && it->id == id) ? &*it : nullptr;
}

std::optional<int> parseIdRef(std::string_view ref) {
    const std::string t = trim(ref);
    if (t.size() > 1 && t[0] == '#') {
        const auto n = parseInt(std::string_view(t).substr(1));
        if (n && *n > 0 && *n <= INT_MAX) return static_cast<int>(*n);
    }
    return std::nullopt;
}

// Exact (case-insensitive) name match first, then a unique name prefix.
template <typename T, typename Pred>
const T& resolveByName(const std::vector<T>& items, std::string_view ref, Pred include, const std::string& noun) {
    const std::string wanted = trim(ref);
    if (wanted.empty()) throw Error(noun + " name is required");
    const std::string lower = toLower(wanted);
    std::vector<const T*> prefixMatches;
    for (const auto& x : items) {
        if (!include(x)) continue;
        if (iequals(x.name, wanted)) return x;
        if (startsWith(toLower(x.name), lower)) prefixMatches.push_back(&x);
    }
    if (prefixMatches.size() == 1) return *prefixMatches.front();
    if (prefixMatches.size() > 1) {
        std::vector<std::string> names;
        for (const T* x : prefixMatches) names.push_back(x->name);
        throw Error("'" + wanted + "' matches several " + noun + "s: " + join(names, ", "));
    }
    throw Error("no " + noun + " matches '" + wanted + "'");
}

const char* docNoun(DocKind k) { return k == DocKind::Invoice ? "invoice" : "bill"; }

}  // namespace

bool Period::contains(Date d) const { return (!from || d >= *from) && (!to || d <= *to); }

// ------------------------------------------------------------------ lookup

const Account& Book::account(int id) const {
    if (auto* a = findById(accounts_, id)) return *a;
    throw Error("no account with id " + std::to_string(id));
}
const Contact& Book::contact(int id) const {
    if (auto* c = findById(contacts_, id)) return *c;
    throw Error("no customer or vendor with id " + std::to_string(id));
}
const Item& Book::item(int id) const {
    if (auto* i = findById(items_, id)) return *i;
    throw Error("no item with id " + std::to_string(id));
}
const Transaction& Book::transaction(int id) const {
    if (auto* t = findById(transactions_, id)) return *t;
    throw Error("no transaction #" + std::to_string(id));
}
const Document& Book::document(int id) const {
    if (auto* d = findById(documents_, id)) return *d;
    throw Error("no invoice or bill with id " + std::to_string(id));
}
const Payment& Book::payment(int id) const {
    if (auto* p = findById(payments_, id)) return *p;
    throw Error("no payment #" + std::to_string(id));
}

Account& Book::accountMut(int id) { return const_cast<Account&>(account(id)); }
Contact& Book::contactMut(int id) { return const_cast<Contact&>(contact(id)); }
Item& Book::itemMut(int id) { return const_cast<Item&>(item(id)); }
Transaction& Book::txnMut(int id) { return const_cast<Transaction&>(transaction(id)); }
Document& Book::docMut(int id) { return const_cast<Document&>(document(id)); }
Payment& Book::paymentMut(int id) { return const_cast<Payment&>(payment(id)); }

const Account& Book::findAccount(std::string_view ref) const {
    if (auto id = parseIdRef(ref)) return account(*id);
    const std::string t = trim(ref);
    for (const auto& a : accounts_) {
        if (!a.number.empty() && a.number == t) return a;
    }
    return resolveByName(accounts_, t, [](const Account&) { return true; }, "account");
}

const Contact& Book::findContact(ContactKind kind, std::string_view ref) const {
    const std::string noun = kind == ContactKind::Customer ? "customer" : "vendor";
    if (auto id = parseIdRef(ref)) {
        const Contact& c = contact(*id);
        if (c.kind != kind) throw Error("#" + std::to_string(*id) + " is not a " + noun);
        return c;
    }
    return resolveByName(contacts_, ref, [kind](const Contact& c) { return c.kind == kind; }, noun);
}

const Item& Book::findItem(std::string_view ref) const {
    if (auto id = parseIdRef(ref)) return item(*id);
    return resolveByName(items_, ref, [](const Item&) { return true; }, "item");
}

const Document& Book::findDocument(DocKind kind, std::string_view ref) const {
    if (auto id = parseIdRef(ref)) {
        const Document& d = document(*id);
        if (d.kind != kind) throw Error("#" + std::to_string(*id) + " is not a " + docNoun(kind));
        return d;
    }
    const std::string number = trim(ref);
    const Document* found = nullptr;
    for (const auto& d : documents_) {
        if (d.kind != kind || d.number != number) continue;
        if (found) throw Error(std::string("more than one ") + docNoun(kind) + " is numbered '" + number +
                               "'; use #id instead");
        found = &d;
    }
    if (!found) throw Error(std::string("no ") + docNoun(kind) + " numbered '" + number + "'");
    return *found;
}

// ------------------------------------------------------------------- setup

void Book::createDefaultChart() {
    if (!accounts_.empty()) throw Error("the chart of accounts is not empty");
    struct Def {
        const char* number;
        const char* name;
        AccountType type;
    };
    static const Def defs[] = {
        {"1000", "Checking", AccountType::Asset},
        {"1010", "Savings", AccountType::Asset},
        {"1050", "Petty Cash", AccountType::Asset},
        {"1100", "Accounts Receivable", AccountType::Asset},
        {"1200", "Inventory", AccountType::Asset},
        {"1500", "Equipment", AccountType::Asset},
        {"1510", "Accumulated Depreciation", AccountType::Asset},
        {"2000", "Accounts Payable", AccountType::Liability},
        {"2100", "Credit Card", AccountType::Liability},
        {"2200", "Sales Tax Payable", AccountType::Liability},
        {"2300", "Payroll Liabilities", AccountType::Liability},
        {"2500", "Loans Payable", AccountType::Liability},
        {"3000", "Owner's Equity", AccountType::Equity},
        {"3100", "Owner's Draw", AccountType::Equity},
        {"3900", "Retained Earnings", AccountType::Equity},
        {"4000", "Sales", AccountType::Income},
        {"4100", "Service Revenue", AccountType::Income},
        {"4900", "Other Income", AccountType::Income},
        {"4990", "Uncategorized Income", AccountType::Income},
        {"5000", "Cost of Goods Sold", AccountType::Expense},
        {"6000", "Advertising", AccountType::Expense},
        {"6100", "Bank Fees", AccountType::Expense},
        {"6150", "Depreciation", AccountType::Expense},
        {"6200", "Insurance", AccountType::Expense},
        {"6300", "Office Supplies", AccountType::Expense},
        {"6400", "Rent", AccountType::Expense},
        {"6500", "Utilities", AccountType::Expense},
        {"6600", "Wages", AccountType::Expense},
        {"6700", "Professional Fees", AccountType::Expense},
        {"6800", "Travel", AccountType::Expense},
        {"6850", "Meals", AccountType::Expense},
        {"6900", "Software & Subscriptions", AccountType::Expense},
        {"6990", "Uncategorized Expense", AccountType::Expense},
    };
    for (const auto& d : defs) addAccount(Account{0, d.number, d.name, d.type, "", true});
    company.receivablesAccountId = findAccount("1100").id;
    company.payablesAccountId = findAccount("2000").id;
    company.salesTaxAccountId = findAccount("2200").id;
    company.retainedEarningsAccountId = findAccount("3900").id;
}

void Book::validateAccountNames(const Account& a) const {
    if (a.name.empty()) throw Error("account name is required");
    if (a.name[0] == '#' || (!a.number.empty() && a.number[0] == '#'))
        throw Error("account names and numbers may not start with '#'");
    for (const auto& other : accounts_) {
        if (other.id == a.id) continue;
        if (!a.number.empty() && other.number == a.number)
            throw Error("account number " + a.number + " is already used by '" + other.name + "'");
        if (iequals(other.name, a.name)) throw Error("an account named '" + a.name + "' already exists");
    }
}

int Book::addAccount(Account a) {
    a.id = 0;
    a.name = trim(a.name);
    a.number = trim(a.number);
    validateAccountNames(a);
    a.id = nextAccountId_++;
    accounts_.push_back(std::move(a));
    return accounts_.back().id;
}

void Book::updateAccount(const Account& updated) {
    Account& current = accountMut(updated.id);
    Account next = current;
    next.name = trim(updated.name);
    next.number = trim(updated.number);
    next.description = updated.description;
    next.active = updated.active;
    if (updated.type != current.type) {
        if (accountInUse(current.id) || isSystemAccount(current.id))
            throw Error("cannot change the type of an account that is in use");
        next.type = updated.type;
    }
    validateAccountNames(next);
    if (!next.active && current.active) {
        if (isSystemAccount(current.id)) throw Error("'" + current.name + "' is a system account and cannot be deactivated");
        if (!balance(current.id).isZero()) throw Error("cannot deactivate '" + current.name + "': its balance is not zero");
    }
    current = std::move(next);
}

void Book::setSystemAccount(SystemAccount role, int accountId) {
    const Account& a = account(accountId);
    AccountType required = AccountType::Asset;
    int* slot = nullptr;
    switch (role) {
        case SystemAccount::Receivables:
            required = AccountType::Asset;
            slot = &company.receivablesAccountId;
            break;
        case SystemAccount::Payables:
            required = AccountType::Liability;
            slot = &company.payablesAccountId;
            break;
        case SystemAccount::SalesTax:
            required = AccountType::Liability;
            slot = &company.salesTaxAccountId;
            break;
        case SystemAccount::RetainedEarnings:
            required = AccountType::Equity;
            slot = &company.retainedEarningsAccountId;
            break;
    }
    if (a.type != required) throw Error("'" + a.name + "' must be an " + toString(required) + " account for that role");
    if (!a.active) throw Error("'" + a.name + "' is inactive");
    if ((role == SystemAccount::Receivables || role == SystemAccount::Payables) && *slot != accountId) {
        if (!documents_.empty() || !payments_.empty())
            throw Error("the A/R and A/P accounts cannot be changed once invoices, bills or payments exist");
        if (accountInUse(accountId))
            throw Error("'" + a.name + "' already has transactions; choose an unused account for A/R or A/P");
    }
    *slot = accountId;
}

void Book::validateContact(const Contact& c) const {
    if (c.name.empty()) throw Error("name is required");
    if (c.name[0] == '#') throw Error("names may not start with '#'");
    if (c.termsDays < -1) throw Error("payment terms cannot be negative");
    for (const auto& other : contacts_) {
        if (other.id != c.id && other.kind == c.kind && iequals(other.name, c.name))
            throw Error(std::string("a ") + (c.kind == ContactKind::Customer ? "customer" : "vendor") + " named '" +
                        c.name + "' already exists");
    }
}

int Book::addContact(Contact c) {
    c.id = 0;
    c.name = trim(c.name);
    validateContact(c);
    c.id = nextContactId_++;
    contacts_.push_back(std::move(c));
    return contacts_.back().id;
}

void Book::updateContact(const Contact& updated) {
    Contact& current = contactMut(updated.id);
    Contact next = updated;
    next.kind = current.kind;
    next.name = trim(next.name);
    validateContact(next);
    current = std::move(next);
}

int Book::addItem(Item i) {
    i.id = 0;
    i.name = trim(i.name);
    if (i.name.empty()) throw Error("item name is required");
    if (i.name[0] == '#') throw Error("item names may not start with '#'");
    if (i.price < Money()) throw Error("item price cannot be negative");
    if (i.incomeAccountId == 0 && i.expenseAccountId == 0)
        throw Error("an item needs an income account, an expense account, or both");
    for (int id : {i.incomeAccountId, i.expenseAccountId}) {
        if (id == 0) continue;
        if (isSubledgerAccount(id)) throw Error("items cannot post to A/R or A/P");
        if (!account(id).active) throw Error("'" + account(id).name + "' is inactive");
    }
    for (const auto& other : items_) {
        if (iequals(other.name, i.name)) throw Error("an item named '" + i.name + "' already exists");
    }
    i.id = nextItemId_++;
    items_.push_back(std::move(i));
    return items_.back().id;
}

void Book::setItemActive(int itemId, bool active) { itemMut(itemId).active = active; }

// ----------------------------------------------------------------- journal

bool Book::isSubledgerAccount(int accountId) const {
    return accountId != 0 && (accountId == company.receivablesAccountId || accountId == company.payablesAccountId);
}

bool Book::isSystemAccount(int accountId) const {
    return accountId != 0 && (accountId == company.receivablesAccountId || accountId == company.payablesAccountId ||
                              accountId == company.salesTaxAccountId ||
                              accountId == company.retainedEarningsAccountId);
}

bool Book::accountInUse(int accountId) const {
    for (const auto& t : transactions_) {
        for (const auto& s : t.splits) {
            if (s.accountId == accountId) return true;
        }
    }
    for (const auto& i : items_) {
        if (i.incomeAccountId == accountId || i.expenseAccountId == accountId) return true;
    }
    return false;
}

void Book::checkOpenPeriod(Date d) const {
    if (company.closedThrough && d <= *company.closedThrough)
        throw Error("the books are closed through " + company.closedThrough->str() +
                    "; nothing dated " + d.str() + " can be added or changed");
}

void Book::validateTransaction(const Transaction& t) const {
    checkOpenPeriod(t.date);
    if (t.splits.size() < 2) throw Error("a transaction needs at least two non-zero lines");
    Money sum;
    for (const auto& s : t.splits) {
        const Account& a = account(s.accountId);
        if (!a.active) throw Error("account '" + a.name + "' is inactive");
        sum += s.amount;
    }
    if (!sum.isZero())
        throw Error("transaction is out of balance by " + sum.formatted() + " (debits must equal credits)");
}

int Book::insertTransaction(Transaction t) {
    t.splits.erase(std::remove_if(t.splits.begin(), t.splits.end(), [](const Split& s) { return s.amount.isZero(); }),
                   t.splits.end());
    validateTransaction(t);
    t.id = nextTxnId_++;
    t.voided = false;
    transactions_.push_back(std::move(t));
    return transactions_.back().id;
}

int Book::postTransaction(Transaction t) {
    if (!isManualKind(t.kind))
        throw Error("invoice, bill and payment transactions are created through their own commands");
    for (auto& s : t.splits) {
        if (isSubledgerAccount(s.accountId))
            throw Error("'" + account(s.accountId).name +
                        "' is managed through invoices, bills and payments and cannot be used directly");
        s.state = SplitState::Uncleared;
    }
    return insertTransaction(std::move(t));
}

void Book::voidTransactionInternal(Transaction& t) {
    if (t.voided) throw Error("transaction #" + std::to_string(t.id) + " is already void");
    checkOpenPeriod(t.date);
    for (const auto& s : t.splits) {
        if (s.state == SplitState::Reconciled)
            throw Error("transaction #" + std::to_string(t.id) + " has been reconciled and cannot be voided");
    }
    t.voided = true;
}

void Book::voidTransaction(int txnId) {
    Transaction& t = txnMut(txnId);
    if (!isManualKind(t.kind))
        throw Error("transaction #" + std::to_string(txnId) + " belongs to a " + toString(t.kind) +
                    "; void that instead");
    voidTransactionInternal(t);
}

void Book::recategorize(int txnId, int fromAccountId, int toAccountId) {
    Transaction& t = txnMut(txnId);
    if (!isManualKind(t.kind))
        throw Error("transaction #" + std::to_string(txnId) + " belongs to a " + toString(t.kind) +
                    " and cannot be recategorized");
    if (t.voided) throw Error("transaction #" + std::to_string(txnId) + " is void");
    checkOpenPeriod(t.date);
    if (fromAccountId == toAccountId) throw Error("the source and destination accounts are the same");
    const Account& to = account(toAccountId);
    if (!to.active) throw Error("account '" + to.name + "' is inactive");
    if (isSubledgerAccount(toAccountId)) throw Error("'" + to.name + "' cannot be used directly");
    bool found = false;
    for (const auto& s : t.splits) {
        if (s.accountId != fromAccountId) continue;
        if (s.state == SplitState::Reconciled) throw Error("a reconciled line cannot be moved to another account");
        found = true;
    }
    if (!found)
        throw Error("transaction #" + std::to_string(txnId) + " has no line in '" + account(fromAccountId).name + "'");
    for (auto& s : t.splits) {
        if (s.accountId != fromAccountId) continue;
        s.accountId = toAccountId;
        s.state = SplitState::Uncleared;
    }
}

// --------------------------------------------------------- invoices & bills

bool Book::documentNumberTaken(DocKind kind, const std::string& number) const {
    for (const auto& d : documents_) {
        if (d.kind == kind && d.number == number) return true;
    }
    return false;
}

int Book::createDocument(Document d) {
    const bool invoice = d.kind == DocKind::Invoice;
    const Contact& c = contact(d.contactId);
    const ContactKind wanted = invoice ? ContactKind::Customer : ContactKind::Vendor;
    if (c.kind != wanted)
        throw Error(std::string(invoice ? "invoices" : "bills") + " must be for a " + toLower(toString(wanted)));
    if (!c.active) throw Error("'" + c.name + "' is inactive");

    const int controlId = invoice ? company.receivablesAccountId : company.payablesAccountId;
    if (controlId == 0)
        throw Error(std::string("no ") + (invoice ? "Accounts Receivable" : "Accounts Payable") +
                    " account is configured");
    if (d.lines.empty()) throw Error("at least one line is required");
    if (d.dueDate < d.date) throw Error("the due date is before the document date");
    if (d.taxRate < Decimal()) throw Error("tax rate cannot be negative");
    if (!invoice && !d.taxRate.isZero()) throw Error("bills do not take a tax rate; include tax in the line amounts");
    if (!d.taxRate.isZero() && company.salesTaxAccountId == 0)
        throw Error("no Sales Tax Payable account is configured");

    for (auto& l : d.lines) {
        if (l.itemId != 0) item(l.itemId);  // must exist
        const Account& a = account(l.accountId);
        if (isSubledgerAccount(a.id)) throw Error("lines cannot post to A/R or A/P");
        l.amount = multiply(l.rate, l.quantity);
    }
    if (d.total() <= Money()) throw Error(std::string(docNoun(d.kind)) + " total must be greater than zero");

    // Work out the number without touching state until everything has validated.
    d.number = trim(d.number);
    bool autoNumbered = false;
    int nextInvoice = company.nextInvoiceNumber;
    if (d.number.empty()) {
        autoNumbered = true;
        if (invoice) {
            while (documentNumberTaken(DocKind::Invoice, std::to_string(nextInvoice))) ++nextInvoice;
            d.number = std::to_string(nextInvoice);
        } else {
            d.number = "B" + std::to_string(nextDocId_);
        }
    } else if (invoice && documentNumberTaken(DocKind::Invoice, d.number)) {
        throw Error("invoice number " + d.number + " is already used");
    }

    Transaction t;
    t.date = d.date;
    t.kind = invoice ? TxnKind::Invoice : TxnKind::Bill;
    t.ref = d.number;
    t.payee = c.name;
    t.memo = d.memo;
    const Money total = d.total();
    if (invoice) {
        t.splits.push_back(Split{controlId, total, "", SplitState::Uncleared});
        for (const auto& l : d.lines) t.splits.push_back(Split{l.accountId, -l.amount, l.description, SplitState::Uncleared});
        const Money tax = d.tax();
        if (!tax.isZero()) t.splits.push_back(Split{company.salesTaxAccountId, -tax, "Sales tax", SplitState::Uncleared});
    } else {
        for (const auto& l : d.lines) t.splits.push_back(Split{l.accountId, l.amount, l.description, SplitState::Uncleared});
        t.splits.push_back(Split{controlId, -total, "", SplitState::Uncleared});
    }

    d.txnId = insertTransaction(std::move(t));  // last step that can throw
    d.id = nextDocId_++;
    d.voided = false;
    if (invoice) {
        if (autoNumbered) {
            company.nextInvoiceNumber = nextInvoice + 1;
        } else if (auto n = parseInt(d.number); n && *n >= company.nextInvoiceNumber && *n < INT_MAX) {
            company.nextInvoiceNumber = static_cast<int>(*n) + 1;
        }
    }
    documents_.push_back(std::move(d));
    return documents_.back().id;
}

void Book::voidDocument(int documentId) {
    Document& d = docMut(documentId);
    if (d.voided) throw Error(std::string(docNoun(d.kind)) + " " + d.number + " is already void");
    for (const auto& p : payments_) {
        if (p.voided) continue;
        for (const auto& a : p.applications) {
            if (a.documentId == documentId)
                throw Error("payment #" + std::to_string(p.id) + " is applied to " + docNoun(d.kind) + " " +
                            d.number + "; void the payment first");
        }
    }
    voidTransactionInternal(txnMut(d.txnId));
    d.voided = true;
}

// ---------------------------------------------------------------- payments

int Book::recordPayment(Payment p) {
    const bool received = p.kind == PaymentKind::Received;
    const ContactKind wanted = received ? ContactKind::Customer : ContactKind::Vendor;
    const DocKind docKind = received ? DocKind::Invoice : DocKind::Bill;
    const Contact& c = contact(p.contactId);
    if (c.kind != wanted)
        throw Error(std::string(received ? "received payments" : "bill payments") + " must be for a " +
                    toLower(toString(wanted)));
    if (p.amount <= Money()) throw Error("payment amount must be greater than zero");
    const int controlId = received ? company.receivablesAccountId : company.payablesAccountId;
    if (controlId == 0) throw Error("no A/R or A/P account is configured");
    const Account& bank = account(p.accountId);
    if (isSubledgerAccount(bank.id)) throw Error("choose a bank, cash or credit card account for the payment");

    if (p.applications.empty()) {
        std::vector<const Document*> open;
        for (const auto& d : documents_) {
            if (d.kind == docKind && d.contactId == c.id && !d.voided && documentBalance(d.id) > Money())
                open.push_back(&d);
        }
        std::stable_sort(open.begin(), open.end(), [](const Document* a, const Document* b) {
            return a->dueDate != b->dueDate ? a->dueDate < b->dueDate : a->id < b->id;
        });
        Money left = p.amount;
        for (const Document* d : open) {
            if (left.isZero()) break;
            const Money due = documentBalance(d->id);
            const Money amount = due < left ? due : left;
            p.applications.push_back(Application{d->id, amount});
            left -= amount;
        }
    } else {
        std::set<int> seen;
        Money sum;
        for (const auto& a : p.applications) {
            const Document& d = document(a.documentId);
            if (d.kind != docKind || d.contactId != c.id)
                throw Error(std::string(docNoun(docKind)) + " " + d.number + " does not belong to " + c.name);
            if (d.voided) throw Error(std::string(docNoun(docKind)) + " " + d.number + " is void");
            if (!seen.insert(d.id).second)
                throw Error(std::string(docNoun(docKind)) + " " + d.number + " is listed more than once");
            if (a.amount <= Money()) throw Error("applied amounts must be greater than zero");
            const Money due = documentBalance(d.id);
            if (a.amount > due)
                throw Error("cannot apply " + a.amount.formatted() + " to " + docNoun(docKind) + " " + d.number +
                            "; its open balance is " + due.formatted());
            sum += a.amount;
        }
        if (sum > p.amount)
            throw Error("applied amounts (" + sum.formatted() + ") exceed the payment amount (" +
                        p.amount.formatted() + ")");
    }

    Transaction t;
    t.date = p.date;
    t.kind = received ? TxnKind::CustomerPayment : TxnKind::VendorPayment;
    t.ref = p.ref;
    t.payee = c.name;
    t.memo = p.memo;
    if (received) {
        t.splits = {Split{p.accountId, p.amount, "", SplitState::Uncleared},
                    Split{controlId, -p.amount, "", SplitState::Uncleared}};
    } else {
        t.splits = {Split{controlId, p.amount, "", SplitState::Uncleared},
                    Split{p.accountId, -p.amount, "", SplitState::Uncleared}};
    }
    p.txnId = insertTransaction(std::move(t));
    p.id = nextPaymentId_++;
    p.voided = false;
    payments_.push_back(std::move(p));
    return payments_.back().id;
}

void Book::voidPayment(int paymentId) {
    Payment& p = paymentMut(paymentId);
    if (p.voided) throw Error("payment #" + std::to_string(paymentId) + " is already void");
    voidTransactionInternal(txnMut(p.txnId));
    p.voided = true;
}

// ---------------------------------------------------------- reconciliation

void Book::setSplitState(int txnId, int accountId, SplitState state) {
    Transaction& t = txnMut(txnId);
    if (t.voided) throw Error("transaction #" + std::to_string(txnId) + " is void");
    bool found = false;
    for (const auto& s : t.splits) {
        if (s.accountId != accountId) continue;
        if (s.state == SplitState::Reconciled && state != SplitState::Reconciled)
            throw Error("transaction #" + std::to_string(txnId) + " is already reconciled in this account");
        found = true;
    }
    if (!found)
        throw Error("transaction #" + std::to_string(txnId) + " has no line in '" + account(accountId).name + "'");
    for (auto& s : t.splits) {
        if (s.accountId == accountId) s.state = state;
    }
}

int Book::finishReconciliation(int accountId) {
    int count = 0;
    for (auto& t : transactions_) {
        if (t.voided) continue;
        for (auto& s : t.splits) {
            if (s.accountId == accountId && s.state == SplitState::Cleared) {
                s.state = SplitState::Reconciled;
                ++count;
            }
        }
    }
    return count;
}

// ----------------------------------------------------------------- queries

std::map<int, Money> Book::balances(const Period& period) const {
    std::map<int, Money> out;
    for (const auto& t : transactions_) {
        if (t.voided || !period.contains(t.date)) continue;
        for (const auto& s : t.splits) out[s.accountId] += s.amount;
    }
    return out;
}

Money Book::balance(int accountId, const Period& period) const {
    Money total;
    for (const auto& t : transactions_) {
        if (t.voided || !period.contains(t.date)) continue;
        for (const auto& s : t.splits) {
            if (s.accountId == accountId) total += s.amount;
        }
    }
    return total;
}

Money Book::clearedBalance(int accountId) const {
    Money total;
    for (const auto& t : transactions_) {
        if (t.voided) continue;
        for (const auto& s : t.splits) {
            if (s.accountId == accountId && s.state != SplitState::Uncleared) total += s.amount;
        }
    }
    return total;
}

Money Book::documentPaid(int documentId, std::optional<Date> asOf) const {
    Money paid;
    for (const auto& p : payments_) {
        if (p.voided || (asOf && p.date > *asOf)) continue;
        for (const auto& a : p.applications) {
            if (a.documentId == documentId) paid += a.amount;
        }
    }
    return paid;
}

Money Book::documentBalance(int documentId, std::optional<Date> asOf) const {
    const Document& d = document(documentId);
    if (d.voided) return Money();
    return d.total() - documentPaid(documentId, asOf);
}

Money Book::contactBalance(int contactId) const {
    Money total;
    for (const auto& d : documents_) {
        if (d.contactId == contactId && !d.voided) total += documentBalance(d.id);
    }
    for (const auto& p : payments_) {
        if (p.contactId == contactId && !p.voided) total -= p.unapplied();
    }
    return total;
}

Date Book::fiscalYearStart(Date d) const {
    const auto startMonth = static_cast<unsigned>(company.fiscalYearStartMonth);
    const int year = d.month() >= startMonth ? d.year() : d.year() - 1;
    return Date::fromYMD(year, startMonth, 1);
}

}  // namespace ob

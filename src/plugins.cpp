#include "openbooks/plugins.hpp"

#include "openbooks/cli.hpp"
#include "openbooks/paths.hpp"
#include "openbooks/reports.hpp"

#include "openplugin/process.hpp"
#include "openplugin/sha256.hpp"

#include <algorithm>
#include <deque>
#include <filesystem>
#include <random>
#include <set>
#include <thread>

namespace ob::plugins {

namespace fs = std::filesystem;
using opl::rpc::RemoteError;
namespace rpc = opl::rpc;

// ------------------------------------------------------------------ locations

std::string pluginsDirectory() { return (fs::u8path(userConfigDirectory()) / "plugins").u8string(); }

std::string registryFile() { return (fs::u8path(userConfigDirectory()) / "plugins.json").u8string(); }

std::string runnerPath() {
#ifdef _WIN32
    const char* name = "openplugin-runner.exe";
#else
    const char* name = "openplugin-runner";
#endif
    const std::string dir = executableDirectory();
    return dir.empty() ? std::string() : (fs::u8path(dir) / name).u8string();
}

opl::AppIdentity appIdentity() { return opl::AppIdentity{"openbooks", kVersion, 1, 1}; }

// ======================================================================= views

namespace {

bool granted(const std::vector<std::string>& perms, const char* p) {
    return std::find(perms.begin(), perms.end(), p) != perms.end();
}

const char* docKindToken(DocKind k) {
    switch (k) {
        case DocKind::Invoice: return "invoice";
        case DocKind::Bill: return "bill";
        case DocKind::Estimate: return "estimate";
        case DocKind::CreditMemo: return "credit-memo";
        case DocKind::SalesReceipt: return "sales-receipt";
    }
    return "?";
}

const char* accountTypeToken(AccountType t) {
    switch (t) {
        case AccountType::Asset: return "asset";
        case AccountType::Liability: return "liability";
        case AccountType::Equity: return "equity";
        case AccountType::Income: return "income";
        case AccountType::Expense: return "expense";
    }
    return "?";
}

const char* txnKindToken(TxnKind k) {
    switch (k) {
        case TxnKind::Journal: return "journal";
        case TxnKind::Expense: return "expense";
        case TxnKind::Deposit: return "deposit";
        case TxnKind::Transfer: return "transfer";
        case TxnKind::Import: return "import";
        case TxnKind::Invoice: return "invoice";
        case TxnKind::Bill: return "bill";
        case TxnKind::CustomerPayment: return "customer-payment";
        case TxnKind::VendorPayment: return "vendor-payment";
        case TxnKind::CreditMemo: return "credit-memo";
        case TxnKind::SalesReceipt: return "sales-receipt";
    }
    return "?";
}

const char* contactKindToken(ContactKind k) { return k == ContactKind::Customer ? "customer" : "vendor"; }

json::Value contactJson(const Contact& c) {
    json::Object o{{"id", c.id},       {"kind", contactKindToken(c.kind)}, {"name", c.name},
                   {"email", c.email}, {"phone", c.phone},                 {"address", c.address},
                   {"active", c.active}};
    if (c.termsDays >= 0) o.set("termsDays", c.termsDays);
    return json::Value(std::move(o));
}

json::Value documentJson(const Book& b, const Document& d) {
    json::Array lines;
    for (const DocLine& l : d.lines) {
        json::Object line{{"description", l.description}, {"quantity", l.quantity.str()}, {"rate", l.rate.str()},
                          {"amount", l.amount.str()},     {"taxable", l.taxable}};
        if (l.itemId) line.set("item", l.itemId);
        if (l.accountId) line.set("account", l.accountId);
        lines.emplace_back(std::move(line));
    }
    json::Object o{{"id", d.id},
                   {"kind", docKindToken(d.kind)},
                   {"number", d.number},
                   {"contact", d.contactId},
                   {"contactName", b.contact(d.contactId).name},
                   {"date", d.date.str()},
                   {"memo", d.memo},
                   {"subtotal", d.subtotal().str()},
                   {"tax", d.tax().str()},
                   {"total", d.total().str()},
                   {"voided", d.voided},
                   {"status", documentStatus(b, d, Date::today())},
                   {"lines", json::Value(std::move(lines))}};
    if (docHasDueDate(d.kind)) o.set(d.kind == DocKind::Estimate ? "expires" : "due", d.dueDate.str());
    if (d.kind != DocKind::Bill) o.set("taxRate", d.taxRate.str());
    if (d.kind == DocKind::Invoice || d.kind == DocKind::Bill || d.kind == DocKind::CreditMemo)
        o.set("balance", b.documentBalance(d.id).str());
    return json::Value(std::move(o));
}

json::Value paymentJson(const Payment& p) {
    json::Array apps;
    for (const Application& a : p.applications)
        apps.emplace_back(json::Object{{"document", a.documentId}, {"amount", a.amount.str()}});
    return json::Object{{"id", p.id},
                        {"kind", p.kind == PaymentKind::Received ? "received" : "paid"},
                        {"contact", p.contactId},
                        {"date", p.date.str()},
                        {"account", p.accountId},
                        {"amount", p.amount.str()},
                        {"unapplied", p.unapplied().str()},
                        {"ref", p.ref},
                        {"memo", p.memo},
                        {"voided", p.voided},
                        {"applications", json::Value(std::move(apps))}};
}

json::Value accountJson(const Book& b, const Account& a) {
    return json::Object{{"id", a.id},
                        {"number", a.number},
                        {"name", a.name},
                        {"type", accountTypeToken(a.type)},
                        {"description", a.description},
                        {"active", a.active},
                        {"balance", naturalSign(a.type, b.balance(a.id)).str()}};
}

json::Value transactionJson(const Transaction& t) {
    json::Array splits;
    for (const Split& s : t.splits)
        splits.emplace_back(json::Object{{"account", s.accountId}, {"amount", s.amount.str()}, {"memo", s.memo}});
    return json::Object{{"id", t.id},     {"date", t.date.str()}, {"kind", txnKindToken(t.kind)},
                        {"ref", t.ref},   {"payee", t.payee},     {"memo", t.memo},
                        {"voided", t.voided}, {"splits", json::Value(std::move(splits))}};
}

// ---- query helpers

const json::Value* field(const json::Value& query, const char* name) {
    if (!query.isObject()) return nullptr;
    const json::Value* v = query.find(name);
    return v && !v->isNull() ? v : nullptr;
}

int intField(const json::Value& query, const char* name, int fallback = 0) {
    const json::Value* v = field(query, name);
    if (!v) return fallback;
    if (!v->isInteger() || v->asInt() < 0 || v->asInt() > INT32_MAX)
        throw RemoteError(rpc::InvalidParams, std::string("\"") + name + "\" must be an id");
    return static_cast<int>(v->asInt());
}

std::string stringField(const json::Value& query, const char* name, const std::string& fallback = {}) {
    const json::Value* v = field(query, name);
    if (!v) return fallback;
    if (!v->isString()) throw RemoteError(rpc::InvalidParams, std::string("\"") + name + "\" must be a string");
    return v->asString();
}

// Plugins send dates as YYYY-MM-DD only (not "today" or MM/DD/YYYY).
std::optional<Date> strictDate(const std::string& s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return std::nullopt;
    return Date::parse(s);
}

std::optional<Date> dateField(const json::Value& query, const char* name) {
    const std::string s = stringField(query, name);
    if (s.empty()) return std::nullopt;
    const auto d = strictDate(s);
    if (!d) throw RemoteError(rpc::InvalidParams, std::string("\"") + name + "\" must be a date like 2026-09-30");
    return d;
}

void requirePermission(const std::vector<std::string>& perms, const char* p, const std::string& view) {
    if (!granted(perms, p))
        throw RemoteError(rpc::PermissionDenied, "reading \"" + view + "\" needs the " + p + " permission, which wasn't granted");
}

}  // namespace

const std::vector<ViewInfo>& views() {
    static const std::vector<ViewInfo> list = {
        {"company", "read:company", "Company name, address, email, phone and fiscal year"},
        {"contacts", "read:contacts", "Customers and vendors. Query: kind, includeInactive"},
        {"contact", "read:contacts", "One customer or vendor. Query: id"},
        {"items", "read:invoices", "Products and services"},
        {"invoices", "read:invoices", "Invoices. Query: status (open|all), contact, since, number"},
        {"estimates", "read:invoices", "Estimates. Query: contact, since, number"},
        {"credit-memos", "read:invoices", "Credit memos. Query: status, contact, since, number"},
        {"sales-receipts", "read:invoices", "Sales receipts. Query: contact, since, number"},
        {"invoice", "read:invoices", "One customer document (invoice, estimate, credit memo, sales receipt). Query: id"},
        {"bills", "read:bills", "Bills. Query: status, contact, since, number"},
        {"bill", "read:bills", "One bill. Query: id"},
        {"payments", "read:payments", "Payments received and made. Query: kind (received|paid), contact, since"},
        {"accounts", "read:ledger", "The chart of accounts with balances"},
        {"transactions", "read:ledger", "Journal transactions. Query: since, until, account"},
    };
    return list;
}

json::Value readView(const Book& b, const std::string& view, const json::Value& query,
                     const std::vector<std::string>& perms) {
    const auto info = std::find_if(views().begin(), views().end(), [&](const ViewInfo& v) { return view == v.name; });
    if (info == views().end()) throw RemoteError(rpc::InvalidParams, "OpenBooks has no view named \"" + view + "\"");
    requirePermission(perms, info->permission, view);

    if (view == "company") {
        const Company& c = b.company;
        json::Object o{{"name", c.name},   {"address", c.address}, {"email", c.email},
                       {"phone", c.phone}, {"fiscalYearStartMonth", c.fiscalYearStartMonth}};
        if (c.closedThrough) o.set("closedThrough", c.closedThrough->str());
        return json::Value(std::move(o));
    }
    if (view == "contacts") {
        const std::string kind = stringField(query, "kind");
        if (!kind.empty() && kind != "customer" && kind != "vendor")
            throw RemoteError(rpc::InvalidParams, "\"kind\" must be customer or vendor");
        const json::Value* inactive = field(query, "includeInactive");
        const bool all = inactive && inactive->isBool() && inactive->asBool();
        json::Array out;
        for (const Contact& c : b.contacts()) {
            if (!kind.empty() && kind != contactKindToken(c.kind)) continue;
            if (!all && !c.active) continue;
            out.push_back(contactJson(c));
        }
        return json::Value(std::move(out));
    }
    if (view == "contact") {
        const int id = intField(query, "id");
        for (const Contact& c : b.contacts())
            if (c.id == id) return contactJson(c);
        throw RemoteError(rpc::InvalidParams, "no contact #" + std::to_string(id));
    }
    if (view == "items") {
        json::Array out;
        for (const Item& i : b.items())
            out.emplace_back(json::Object{{"id", i.id},           {"name", i.name},       {"description", i.description},
                                          {"price", i.price.str()}, {"taxable", i.taxable}, {"active", i.active}});
        return json::Value(std::move(out));
    }
    if (view == "invoice" || view == "bill") {
        const int id = intField(query, "id");
        for (const Document& d : b.documents()) {
            if (d.id != id) continue;
            if ((view == "bill") != (d.kind == DocKind::Bill)) break;  // each view covers only its own documents
            return documentJson(b, d);
        }
        throw RemoteError(rpc::InvalidParams, "no " + view + " #" + std::to_string(id));
    }
    if (view == "invoices" || view == "estimates" || view == "credit-memos" || view == "sales-receipts" || view == "bills") {
        const DocKind kind = view == "invoices"       ? DocKind::Invoice
                             : view == "estimates"    ? DocKind::Estimate
                             : view == "credit-memos" ? DocKind::CreditMemo
                             : view == "bills"        ? DocKind::Bill
                                                      : DocKind::SalesReceipt;
        const std::string status = stringField(query, "status", "all");
        if (status != "all" && status != "open") throw RemoteError(rpc::InvalidParams, "\"status\" must be open or all");
        const int contact = intField(query, "contact");
        const auto since = dateField(query, "since");
        const std::string number = stringField(query, "number");
        json::Array out;
        for (const Document& d : b.documents()) {
            if (d.kind != kind) continue;
            if (contact && d.contactId != contact) continue;
            if (since && d.date < *since) continue;
            if (!number.empty() && d.number != number) continue;
            if (status == "open" && (d.voided || !(b.documentBalance(d.id) > Money()))) continue;
            out.push_back(documentJson(b, d));
        }
        return json::Value(std::move(out));
    }
    if (view == "payments") {
        const std::string kind = stringField(query, "kind");
        if (!kind.empty() && kind != "received" && kind != "paid")
            throw RemoteError(rpc::InvalidParams, "\"kind\" must be received or paid");
        const int contact = intField(query, "contact");
        const auto since = dateField(query, "since");
        json::Array out;
        for (const Payment& p : b.payments()) {
            if (!kind.empty() && kind != (p.kind == PaymentKind::Received ? "received" : "paid")) continue;
            if (contact && p.contactId != contact) continue;
            if (since && p.date < *since) continue;
            out.push_back(paymentJson(p));
        }
        return json::Value(std::move(out));
    }
    if (view == "accounts") {
        json::Array out;
        for (const Account& a : b.accounts()) out.push_back(accountJson(b, a));
        return json::Value(std::move(out));
    }
    // transactions
    const auto since = dateField(query, "since");
    const auto until = dateField(query, "until");
    const int account = intField(query, "account");
    json::Array out;
    for (const Transaction& t : b.transactions()) {
        if (since && t.date < *since) continue;
        if (until && t.date > *until) continue;
        if (account && std::none_of(t.splits.begin(), t.splits.end(), [&](const Split& s) { return s.accountId == account; }))
            continue;
        out.push_back(transactionJson(t));
    }
    return json::Value(std::move(out));
}

// =================================================================== proposals

namespace {

[[noreturn]] void bad(std::size_t index, const std::string& message) {
    throw RemoteError(rpc::InvalidParams, "change " + std::to_string(index + 1) + ": " + message);
}

// Amounts from plugins: "-1234.50" style only (Money::parse is more forgiving for people).
std::optional<Money> strictMoney(const json::Value* v) {
    if (!v || !v->isString()) return std::nullopt;
    const std::string& s = v->asString();
    std::size_t i = s.size() > 0 && s[0] == '-' ? 1 : 0;
    const std::size_t digitsStart = i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    if (i == digitsStart || i - digitsStart > 13) return std::nullopt;
    if (i < s.size()) {
        if (s[i] != '.') return std::nullopt;
        const std::size_t fracStart = ++i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        if (i != s.size() || i - fracStart < 1 || i - fracStart > 2) return std::nullopt;
    }
    return Money::parse(s);
}

std::string text(const json::Value& change, const char* name, std::size_t index, std::size_t maxLen = 500) {
    const json::Value* v = change.find(name);
    if (!v || v->isNull()) return {};
    if (!v->isString()) bad(index, std::string("\"") + name + "\" must be a string");
    if (v->asString().size() > maxLen) bad(index, std::string("\"") + name + "\" is too long");
    return v->asString();
}

int id(const json::Value& change, const char* name, std::size_t index) {
    const json::Value* v = change.find(name);
    if (!v || !v->isInteger() || v->asInt() <= 0 || v->asInt() > INT32_MAX)
        bad(index, std::string("\"") + name + "\" must be an id");
    return static_cast<int>(v->asInt());
}

Date date(const json::Value& change, std::size_t index) {
    const auto d = strictDate(text(change, "date", index));
    if (!d) bad(index, "\"date\" must be a date like 2026-09-30");
    return *d;
}

const Contact& contactFor(const Book& b, int contactId, std::size_t index) {
    for (const Contact& c : b.contacts())
        if (c.id == contactId) return c;
    bad(index, "there is no contact #" + std::to_string(contactId));
}

const Account& accountFor(const Book& b, int accountId, std::size_t index) {
    for (const Account& a : b.accounts())
        if (a.id == accountId) return a;
    bad(index, "there is no account #" + std::to_string(accountId));
}

const Document& documentFor(const Book& b, int documentId, std::size_t index) {
    for (const Document& d : b.documents())
        if (d.id == documentId) return d;
    bad(index, "there is no invoice or bill #" + std::to_string(documentId));
}

// ---- recordPayment

Payment paymentFrom(const Book& b, const json::Value& c, std::size_t index) {
    Payment p;
    const std::string kind = text(c, "kind", index);
    if (kind.empty() || kind == "received") p.kind = PaymentKind::Received;
    else if (kind == "paid") p.kind = PaymentKind::Paid;
    else bad(index, "\"kind\" must be received or paid");
    p.contactId = id(c, "contact", index);
    const Contact& contact = contactFor(b, p.contactId, index);
    if (contact.kind != (p.kind == PaymentKind::Received ? ContactKind::Customer : ContactKind::Vendor))
        bad(index, p.kind == PaymentKind::Received ? "payments are received from customers" : "payments are paid to vendors");
    p.date = date(c, index);
    p.accountId = id(c, "account", index);
    accountFor(b, p.accountId, index);
    const auto amount = strictMoney(c.find("amount"));
    if (!amount || !(*amount > Money())) bad(index, "\"amount\" must be a positive amount like \"1250.00\"");
    p.amount = *amount;
    p.ref = text(c, "ref", index, 100);
    p.memo = text(c, "memo", index);
    if (const json::Value* apps = c.find("applications"); apps && !apps->isNull()) {
        if (!apps->isArray()) bad(index, "\"applications\" must be a list");
        for (const json::Value& a : apps->asArray()) {
            if (!a.isObject()) bad(index, "each application needs a document and an amount");
            const int doc = id(a, "document", index);
            documentFor(b, doc, index);
            const auto applied = strictMoney(a.find("amount"));
            if (!applied || !(*applied > Money())) bad(index, "each application needs a positive amount");
            p.applications.push_back(Application{doc, *applied});
        }
    }
    return p;
}

std::string describePayment(const Book& b, const Payment& p) {
    std::string s = p.kind == PaymentKind::Received ? "Receive " + p.amount.formatted() + " from " + b.contact(p.contactId).name +
                                                          " into " + b.account(p.accountId).name
                                                    : "Pay " + p.amount.formatted() + " to " + b.contact(p.contactId).name + " from " +
                                                          b.account(p.accountId).name;
    s += " on " + p.date.str();
    if (!p.applications.empty()) {
        s += ", for ";
        for (std::size_t i = 0; i < p.applications.size(); ++i) {
            const Document& d = b.document(p.applications[i].documentId);
            if (i) s += ", ";
            s += docNoun(d.kind) + std::string(" ") + d.number + " (" + p.applications[i].amount.formatted() + ")";
        }
    } else {
        s += p.kind == PaymentKind::Received ? ", applied to the oldest open invoices" : ", applied to the oldest open bills";
    }
    if (!p.ref.empty()) s += " [ref " + p.ref + "]";
    return s;
}

// ---- postTransaction

Transaction transactionFrom(const Book& b, const json::Value& c, std::size_t index) {
    Transaction t;
    const std::string kind = text(c, "kind", index);
    if (kind == "expense") t.kind = TxnKind::Expense;
    else if (kind == "deposit") t.kind = TxnKind::Deposit;
    else if (kind == "transfer") t.kind = TxnKind::Transfer;
    else if (kind.empty() || kind == "journal") t.kind = TxnKind::Journal;
    else bad(index, "\"kind\" must be expense, deposit, transfer or journal");
    t.date = date(c, index);
    t.ref = text(c, "ref", index, 100);
    t.payee = text(c, "payee", index, 200);
    t.memo = text(c, "memo", index);
    const json::Value* splits = c.find("splits");
    if (!splits || !splits->isArray() || splits->asArray().size() < 2 || splits->asArray().size() > 100)
        bad(index, "\"splits\" must list 2 to 100 lines");
    for (const json::Value& s : splits->asArray()) {
        if (!s.isObject()) bad(index, "each split needs an account and a debit or credit");
        Split split;
        split.accountId = id(s, "account", index);
        accountFor(b, split.accountId, index);
        const auto debit = strictMoney(s.find("debit"));
        const auto credit = strictMoney(s.find("credit"));
        if (debit.has_value() == credit.has_value()) bad(index, "each split needs exactly one of \"debit\" and \"credit\"");
        const Money amount = debit ? *debit : *credit;
        if (!(amount > Money())) bad(index, "debits and credits must be positive");
        split.amount = debit ? amount : -amount;
        split.memo = text(s, "memo", index);
        t.splits.push_back(std::move(split));
    }
    return t;
}

std::string describeTransaction(const Book& b, const Transaction& t) {
    std::string kind = txnKindToken(t.kind);
    kind[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(kind[0])));
    std::string s = kind + " on " + t.date.str();
    if (!t.payee.empty()) s += " to " + t.payee;
    if (!t.memo.empty()) s += " (" + t.memo + ")";
    s += ":";
    for (const Split& split : t.splits)
        s += std::string(split.amount > Money() ? " debit " : " credit ") + b.account(split.accountId).name + " " +
             (split.amount > Money() ? split.amount : -split.amount).formatted() + ";";
    s.pop_back();
    return s;
}

// ---- addContact

Contact contactFrom(const json::Value& c, std::size_t index) {
    Contact k;
    const std::string kind = text(c, "kind", index);
    if (kind == "customer") k.kind = ContactKind::Customer;
    else if (kind == "vendor") k.kind = ContactKind::Vendor;
    else bad(index, "\"kind\" must be customer or vendor");
    k.name = text(c, "name", index, 200);
    if (k.name.empty()) bad(index, "\"name\" is required");
    k.email = text(c, "email", index, 200);
    k.phone = text(c, "phone", index, 100);
    k.address = text(c, "address", index, 1000);
    if (const json::Value* terms = c.find("termsDays"); terms && !terms->isNull()) {
        if (!terms->isInteger() || terms->asInt() < 0 || terms->asInt() > 3650) bad(index, "\"termsDays\" must be 0 to 3650");
        k.termsDays = static_cast<int>(terms->asInt());
    }
    return k;
}

struct OpInfo {
    const char* op;
    const char* permission;
};
const OpInfo kOps[] = {{"recordPayment", "propose:payments"},
                       {"postTransaction", "propose:transactions"},
                       {"addContact", "propose:contacts"}};

}  // namespace

Proposal parseProposal(const Book& b, const std::string& pluginId, const std::string& pluginName, const json::Value& params,
                       const std::vector<std::string>& perms) {
    if (!params.isObject()) throw RemoteError(rpc::InvalidParams, "a proposal must be an object");
    Proposal p;
    p.pluginId = pluginId;
    p.pluginName = pluginName;
    if (const json::Value* s = params.find("summary"); s && s->isString()) p.summary = s->asString().substr(0, 300);
    if (p.summary.empty()) throw RemoteError(rpc::InvalidParams, "a proposal needs a \"summary\" for the person reviewing it");
    const json::Value* changes = params.find("changes");
    if (!changes || !changes->isArray()) throw RemoteError(rpc::InvalidParams, "a proposal needs a \"changes\" list");
    if (changes->asArray().size() > 500) throw RemoteError(rpc::InvalidParams, "a proposal can have at most 500 changes");

    for (std::size_t i = 0; i < changes->asArray().size(); ++i) {
        const json::Value& c = changes->asArray()[i];
        if (!c.isObject()) bad(i, "each change must be an object");
        const std::string op = text(c, "op", i, 40);
        const auto info = std::find_if(std::begin(kOps), std::end(kOps), [&](const OpInfo& o) { return op == o.op; });
        if (info == std::end(kOps)) bad(i, "unknown op \"" + op + "\" (OpenBooks takes recordPayment, postTransaction, addContact)");
        if (!granted(perms, info->permission))
            throw RemoteError(rpc::PermissionDenied, op + " needs the " + info->permission + " permission, which wasn't granted");
        ProposedChange pc;
        pc.op = op;
        pc.data = c;
        if (op == "recordPayment") pc.description = describePayment(b, paymentFrom(b, c, i));
        else if (op == "postTransaction") pc.description = describeTransaction(b, transactionFrom(b, c, i));
        else {
            const Contact k = contactFrom(c, i);
            pc.description = std::string("Add ") + contactKindToken(k.kind) + " " + k.name;
        }
        p.changes.push_back(std::move(pc));
    }
    if (p.changes.empty()) throw RemoteError(rpc::InvalidParams, "the proposal has no changes");

    if (const json::Value* store = params.find("store"); store && !store->isNull()) {
        if (!store->isObject()) throw RemoteError(rpc::InvalidParams, "\"store\" must map keys to text");
        if (!store->asObject().empty() && !granted(perms, "store"))
            throw RemoteError(rpc::PermissionDenied, "saving plugin data needs the store permission, which wasn't granted");
        for (const auto& [key, value] : store->asObject()) {
            if (!value.isString()) throw RemoteError(rpc::InvalidParams, "\"store\" values must be text");
            if (!key.empty() && key[0] == '@') throw RemoteError(rpc::InvalidParams, "keys starting with '@' are reserved");
            p.store.emplace_back(key, value.asString());
        }
    }
    return p;
}

std::vector<int> applyProposal(Book& b, const Proposal& p, const std::vector<bool>& selected) {
    std::vector<int> ids;
    for (std::size_t i = 0; i < p.changes.size(); ++i) {
        if (!selected.empty() && (i >= selected.size() || !selected[i])) continue;
        const ProposedChange& c = p.changes[i];
        try {
            if (c.op == "recordPayment") ids.push_back(b.recordPayment(paymentFrom(b, c.data, i)));
            else if (c.op == "postTransaction") ids.push_back(b.postTransaction(transactionFrom(b, c.data, i)));
            else if (c.op == "addContact") ids.push_back(b.addContact(contactFrom(c.data, i)));
        } catch (const RemoteError& e) {
            throw Error(e.what());  // e.g. something it refers to was deleted since the review began
        } catch (const Error& e) {
            throw Error("change " + std::to_string(i + 1) + " (" + c.description + "): " + e.what());
        }
    }
    for (const auto& [key, value] : p.store) b.plugins.set(p.pluginId, key, value, false);
    return ids;
}

// ==================================================================== the host

namespace {

constexpr auto kInitTimeout = std::chrono::seconds(10);
constexpr auto kCommandTimeout = std::chrono::minutes(5);
constexpr std::size_t kMaxLogLines = 500;

std::string randomHex(std::size_t bytes) {
    std::random_device rd;
    std::string raw;
    for (std::size_t i = 0; i < bytes; ++i) raw += static_cast<char>(rd() & 0xFF);
    return opl::toHex(reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size());
}

}  // namespace

struct PluginHost::Impl {
    struct Call {
        std::int64_t channelId = 0;  // 0 while queued for startup
        std::string method;
        json::Value params;
        std::chrono::milliseconds timeout{0};
        Done done;
        std::string title;
        std::string progress;
        double fraction = -1;
    };

    struct Running {
        std::string id;
        std::string name;
        opl::Manifest manifest;
        opl::Grant grant;
        std::unique_ptr<rpc::Channel> channel;
        RunState state = RunState::Stopped;
        std::string failure;
        std::deque<std::string> log;
        std::map<std::int64_t, Call> calls;  // by host call id
    };

    std::map<std::string, Running> running;
    std::map<std::string, std::string> lastFailure;
    std::int64_t nextCallId = 1;
};

PluginHost::PluginHost(Delegate& delegate, std::string pluginsDir, std::string registryPath, std::string runner)
    : delegate_(delegate),
      registry_(fs::u8path(pluginsDir), fs::u8path(registryPath), appIdentity()),
      runner_(std::move(runner)),
      impl_(std::make_unique<Impl>()) {
    reload();
}

PluginHost::~PluginHost() { stopAll(); }

std::string PluginHost::reload() {
    try {
        registry_.load();
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}

std::optional<opl::PluginEntry> PluginHost::find(const std::string& pluginId) const {
    for (opl::PluginEntry& e : registry_.scan())
        if (e.id() == pluginId) return std::move(e);
    return std::nullopt;
}

bool PluginHost::runnerAvailable() const {
    std::error_code ec;
    return !runner_.empty() && fs::is_regular_file(fs::u8path(runner_), ec);
}

bool PluginHost::usedWithFile(const std::string& pluginId) const {
    const Book* b = delegate_.books();
    return b && b->plugins.usedWithFile(pluginId);
}

void PluginHost::setUsedWithFile(const std::string& pluginId, bool used) {
    if (!delegate_.books()) throw opl::Error("open your books first");
    if (!used) stop(pluginId);
    delegate_.commit([&](Book& b) {
        b.plugins.setUsedWithFile(pluginId, used);
        // A random id per plugin and file, so a plugin can keep per-file state without learning the
        // file's path, and different plugins can't match up their ids for the same file.
        if (used && !b.plugins.getHost(pluginId, "@fileId")) b.plugins.setHost(pluginId, "@fileId", randomHex(16));
    });
}

void PluginHost::removeData(const std::string& pluginId) {
    stop(pluginId);
    delegate_.commit([&](Book& b) { b.plugins.erasePlugin(pluginId); });
}

RunState PluginHost::state(const std::string& pluginId) const {
    auto it = impl_->running.find(pluginId);
    return it == impl_->running.end() ? RunState::Stopped : it->second.state;
}

std::string PluginHost::failure(const std::string& pluginId) const {
    auto it = impl_->running.find(pluginId);
    if (it != impl_->running.end()) return it->second.failure;
    auto f = impl_->lastFailure.find(pluginId);
    return f == impl_->lastFailure.end() ? std::string() : f->second;
}

std::string PluginHost::log(const std::string& pluginId) const {
    auto it = impl_->running.find(pluginId);
    if (it == impl_->running.end()) return {};
    std::string out;
    for (const std::string& line : it->second.log) out += line + "\n";
    if (it->second.channel) {
        const std::string err = it->second.channel->log();
        if (!err.empty()) out += "\n-- plugin output --\n" + err;
    }
    return out;
}

void PluginHost::start(const std::string& pluginId) {
    if (state(pluginId) == RunState::Starting || state(pluginId) == RunState::Ready) return;
    const Book* books = delegate_.books();
    if (!books) throw opl::Error("open your books first");
    const auto entry = find(pluginId);
    if (!entry) throw opl::Error("no plugin '" + pluginId + "' is installed");
    switch (entry->status) {
        case opl::PluginStatus::Enabled: break;
        case opl::PluginStatus::NotEnabled: throw opl::Error(entry->manifest->name + " isn't enabled");
        case opl::PluginStatus::NeedsApproval:
            throw opl::Error(entry->manifest->name + " needs your approval again: " + entry->detail);
        default: throw opl::Error(entry->manifest ? entry->manifest->name + ": " + entry->detail : entry->detail);
    }
    const opl::Manifest& m = *entry->manifest;
    if (!books->plugins.usedWithFile(pluginId))
        throw opl::Error(m.name + " isn't turned on for this books file");

    opl::SpawnOptions spawn;
    const fs::path folder = fs::absolute(entry->folder);
    if (m.runtime == opl::Runtime::Lua) {
        if (!runnerAvailable())
            throw opl::Error("openplugin-runner isn't installed next to OpenBooks, so Lua plugins can't run");
        spawn.argv = {runner_, "--plugin", folder.u8string()};
    } else {
        const auto& run = m.runForThisPlatform();
        std::string exe = run.at(0);
        if (exe.rfind("./", 0) == 0) exe = exe.substr(2);
        spawn.argv.push_back((folder / fs::u8path(exe)).u8string());
        spawn.argv.insert(spawn.argv.end(), run.begin() + 1, run.end());
    }
    spawn.workingDir = folder;
    spawn.environment = opl::scrubbedEnvironment();
    spawn.environment.emplace_back("OPENPLUGIN_PROTOCOL", "1");

    Impl::Running r;
    r.id = pluginId;
    r.name = m.name;
    r.manifest = m;
    r.grant = *entry->grant;
    r.state = RunState::Starting;
    r.channel = std::make_unique<rpc::Channel>(opl::Process::spawn(spawn));
    impl_->lastFailure.erase(pluginId);
    auto& slot = impl_->running[pluginId];
    slot = std::move(r);
    rpc::Channel& ch = *slot.channel;

    // Activity from the plugin (it is working, or waiting on the user) keeps its calls alive.
    const auto touch = [this, pluginId] {
        auto it = impl_->running.find(pluginId);
        if (it == impl_->running.end()) return;
        for (auto& [callId, call] : it->second.calls)
            if (call.channelId) it->second.channel->extend(call.channelId, call.timeout);
    };
    const auto appendLog = [this, pluginId](std::string line) {
        auto it = impl_->running.find(pluginId);
        if (it == impl_->running.end()) return;
        it->second.log.push_back(std::move(line));
        while (it->second.log.size() > kMaxLogLines) it->second.log.pop_front();
    };

    ch.onNotification([this, pluginId, touch, appendLog](const std::string& method, const json::Value& params) {
        touch();
        const auto str = [&](const char* k) {
            const json::Value* v = params.find(k);
            return v && v->isString() ? v->asString() : std::string();
        };
        if (method == "$/log") {
            appendLog("[" + str("level") + "] " + str("message"));
        } else if (method == "ui/notify") {
            appendLog("[notify " + str("level") + "] " + str("message"));
            delegate_.notify(pluginId, str("level"), str("message"));
        } else if (method == "$/progress") {
            auto it = impl_->running.find(pluginId);
            if (it == impl_->running.end() || it->second.calls.empty()) return;
            Impl::Call& call = it->second.calls.begin()->second;  // the runner handles one call at a time
            call.progress = str("message");
            const json::Value* f = params.find("fraction");
            call.fraction = f && f->isNumber() ? std::clamp(f->asDouble(), 0.0, 1.0) : -1;
        }
    });

    ch.onRequest([this, pluginId, touch](const std::string& method, const json::Value& params, rpc::Responder respond) {
        touch();
        auto it = impl_->running.find(pluginId);
        if (it == impl_->running.end()) throw RemoteError(rpc::InternalError, "the plugin was stopped");
        const Impl::Running& p = it->second;
        const std::vector<std::string>& perms = p.grant.permissions;
        const auto has = [&](const char* perm) { return granted(perms, perm); };
        const Book* books = delegate_.books();
        if (!books) throw RemoteError(rpc::InternalError, "no books are open");
        const auto str = [&](const char* k) {
            const json::Value* v = params.find(k);
            if (!v || !v->isString()) throw RemoteError(rpc::InvalidParams, std::string("\"") + k + "\" must be text");
            return v->asString();
        };

        if (method == "host/read") {
            const json::Value* query = params.find("query");
            respond.result(readView(*books, str("view"), query ? *query : json::Value(), perms));
        } else if (method == "host/propose") {
            Proposal proposal = parseProposal(*books, pluginId, p.name, params, perms);
            const std::string name = p.name;
            delegate_.review(std::move(proposal), [this, pluginId, name, params, perms, respond, touch](
                                                      bool apply, std::vector<bool> selected) -> std::string {
                if (respond.answered()) return {};
                touch();
                const bool none = !selected.empty() && std::none_of(selected.begin(), selected.end(), [](bool s) { return s; });
                if (!apply || none) {
                    respond.result(json::Object{{"applied", false}, {"reason", "declined"}});
                    return {};
                }
                const Book* now = delegate_.books();
                if (!now) {
                    respond.error(rpc::Rejected, "the books were closed");
                    return {};
                }
                std::vector<int> ids;
                try {
                    // Checked again against the books as they are now: they may have changed during the review.
                    const Proposal fresh = parseProposal(*now, pluginId, name, params, perms);
                    delegate_.commit([&](Book& b) { ids = applyProposal(b, fresh, selected); });
                } catch (const std::exception& e) {
                    return e.what();  // the review stays open; nothing was changed
                }
                json::Array created;
                for (int id : ids) created.emplace_back(id);
                respond.result(json::Object{{"applied", true}, {"ids", json::Value(std::move(created))}});
                return {};
            });
        } else if (method == "store/get") {
            if (!has("store")) throw RemoteError(rpc::PermissionDenied, "op.store needs the store permission, which wasn't granted");
            const auto* e = books->plugins.get(pluginId, str("key"));
            respond.result(e ? json::Value(e->value) : json::Value());
        } else if (method == "store/set") {
            if (!has("store")) throw RemoteError(rpc::PermissionDenied, "op.store needs the store permission, which wasn't granted");
            const std::string key = str("key");
            const std::string value = str("value");
            const json::Value* s = params.find("secret");
            const bool secret = s && s->isBool() && s->asBool();
            if (!key.empty() && key[0] == '@') throw RemoteError(rpc::InvalidParams, "keys starting with '@' are reserved");
            const auto save = [this, pluginId, key, value, secret, respond](bool rememberWarning) {
                try {
                    delegate_.commit([&](Book& b) {
                        b.plugins.set(pluginId, key, value, secret);
                        if (rememberWarning) b.plugins.setHost(pluginId, "@secretWarned", "1");
                    });
                    respond.result(nullptr);
                } catch (const opl::StoreError& e) {
                    respond.error(rpc::InvalidParams, e.what());
                } catch (const std::exception& e) {
                    respond.error(rpc::InternalError, e.what());
                }
            };
            if (secret && !delegate_.encrypted() && !books->plugins.getHost(pluginId, "@secretWarned")) {
                delegate_.confirmUnencryptedSecret(p.name, [save, respond](bool ok) {
                    if (ok) save(true);
                    else respond.error(rpc::Rejected, "not saved: the books file has no password");
                });
            } else {
                save(false);
            }
        } else if (method == "store/delete") {
            if (!has("store")) throw RemoteError(rpc::PermissionDenied, "op.store needs the store permission, which wasn't granted");
            const std::string key = str("key");
            if (!key.empty() && key[0] == '@') throw RemoteError(rpc::InvalidParams, "keys starting with '@' are reserved");
            delegate_.commit([&](Book& b) { b.plugins.erase(pluginId, key); });
            respond.result(nullptr);
        } else if (method == "ui/show" || method == "ui/update" || method == "ui/close") {
            if (!delegate_.pluginUi(pluginId, method, params))
                throw RemoteError(rpc::MethodNotFound, "plugin windows are only available in the desktop app");
            respond.result(nullptr);
        } else if (method == "files/save") {
            if (!has("files")) throw RemoteError(rpc::PermissionDenied, "op.files needs the files permission, which wasn't granted");
            const auto path = delegate_.saveFile(str("suggestedName"), str("content"));
            respond.result(path ? json::Value(*path) : json::Value());
        } else {
            throw RemoteError(rpc::MethodNotFound, "OpenBooks doesn't offer " + method);
        }
    });

    // initialize
    const std::string* fileId = books->plugins.getHost(pluginId, "@fileId");
    json::Array perms, hosts;
    for (const std::string& s : slot.grant.permissions) perms.emplace_back(s);
    for (const std::string& s : slot.grant.network) hosts.emplace_back(s);
    json::Object init{{"protocol", 1},
                      {"app", json::Object{{"id", "openbooks"}, {"version", kVersion}}},
                      {"granted", json::Value(std::move(perms))},
                      {"network", json::Value(std::move(hosts))},
                      {"locale", "en-US"},
                      {"file", json::Object{{"opaqueId", fileId ? *fileId : std::string()}, {"encrypted", delegate_.encrypted()}}},
                      {"hash", entry->hash},
                      {"limits", json::Object{{"memoryBytes", 256 << 20}}}};
    ch.request("initialize", json::Value(std::move(init)), kInitTimeout, [this, pluginId](const rpc::Outcome& o) {
        auto it = impl_->running.find(pluginId);
        if (it == impl_->running.end()) return;
        Impl::Running& r = it->second;
        if (!o.ok()) {
            r.state = RunState::Failed;
            r.failure = o.message.empty() ? "the plugin didn't start" : o.message;
            r.channel->close("failed to start");
            // Everything queued for it fails too.
            auto calls = std::move(r.calls);
            r.calls.clear();
            for (auto& [id, call] : calls) {
                rpc::Outcome failed;
                failed.status = rpc::Outcome::Status::Error;
                failed.code = o.code;
                failed.message = r.name + " couldn't start: " + r.failure;
                if (call.done) call.done(failed);
            }
            return;
        }
        r.state = RunState::Ready;
        r.channel->notify("initialized", json::Value());
        for (auto& [id, call] : r.calls) {
            if (call.channelId) continue;
            const std::int64_t callId = id;
            call.channelId = r.channel->request(call.method, call.params, call.timeout, [this, pluginId, callId](const rpc::Outcome& out) {
                auto it2 = impl_->running.find(pluginId);
                if (it2 == impl_->running.end()) return;
                auto c = it2->second.calls.find(callId);
                if (c == it2->second.calls.end()) return;
                Done done = std::move(c->second.done);
                it2->second.calls.erase(c);
                if (done) done(out);
            });
        }
    });
}

void PluginHost::stop(const std::string& pluginId) {
    auto it = impl_->running.find(pluginId);
    if (it == impl_->running.end()) return;
    Impl::Running r = std::move(it->second);
    impl_->running.erase(it);
    if (!r.failure.empty()) impl_->lastFailure[pluginId] = r.failure;
    if (r.channel) r.channel->close("stopped");  // everything it keeps is already saved in the books
    for (auto& [id, call] : r.calls) {
        rpc::Outcome o;
        o.status = rpc::Outcome::Status::Closed;
        o.message = "the plugin was stopped";
        if (call.done) call.done(o);
    }
}

void PluginHost::stopAll() {
    std::vector<std::string> ids;
    for (const auto& [id, r] : impl_->running) ids.push_back(id);
    for (const std::string& id : ids) stop(id);
}

std::int64_t PluginHost::runCommand(const std::string& pluginId, const std::string& commandId, json::Value context, Done done) {
    std::string title = commandId;
    if (auto entry = find(pluginId); entry && entry->manifest)
        for (const opl::CommandDecl& c : entry->manifest->commands)
            if (c.id == commandId) title = c.title;
    return call(pluginId, "command/run", json::Object{{"id", commandId}, {"context", std::move(context)}}, kCommandTimeout,
                std::move(done), title);
}

std::int64_t PluginHost::call(const std::string& pluginId, const std::string& method, json::Value params,
                              std::chrono::milliseconds timeout, Done done, std::string title) {
    if (state(pluginId) == RunState::Failed) stop(pluginId);  // try again from scratch
    start(pluginId);  // no-op when it is already running
    Impl::Running& r = impl_->running.at(pluginId);
    const std::int64_t callId = impl_->nextCallId++;
    Impl::Call c;
    c.method = method;
    c.params = std::move(params);
    c.timeout = timeout;
    c.done = std::move(done);
    c.title = title.empty() ? method : title;
    if (r.state == RunState::Ready) {
        c.channelId = r.channel->request(method, c.params, timeout, [this, pluginId, callId](const rpc::Outcome& out) {
            auto it = impl_->running.find(pluginId);
            if (it == impl_->running.end()) return;
            auto cl = it->second.calls.find(callId);
            if (cl == it->second.calls.end()) return;
            Done d = std::move(cl->second.done);
            it->second.calls.erase(cl);
            if (d) d(out);
        });
    }
    r.calls.emplace(callId, std::move(c));
    return callId;
}

void PluginHost::cancel(const std::string& pluginId, std::int64_t callId) {
    auto it = impl_->running.find(pluginId);
    if (it == impl_->running.end()) return;
    auto c = it->second.calls.find(callId);
    if (c == it->second.calls.end()) return;
    if (c->second.channelId) {
        it->second.channel->cancel(c->second.channelId);  // `done` runs with Cancelled from poll()
    } else {
        Done d = std::move(c->second.done);
        it->second.calls.erase(c);
        rpc::Outcome o;
        o.status = rpc::Outcome::Status::Cancelled;
        o.message = "cancelled";
        if (d) d(o);
    }
}

void PluginHost::event(const std::string& name, const json::Value& details) {
    for (auto& [id, r] : impl_->running)
        if (r.state == RunState::Ready) r.channel->notify("event/" + name, details);
}

void PluginHost::poll(std::chrono::milliseconds wait) {
    bool any = false;
    std::vector<std::string> ids;
    for (const auto& [id, r] : impl_->running) ids.push_back(id);
    for (const std::string& id : ids) {
        auto it = impl_->running.find(id);
        if (it == impl_->running.end() || !it->second.channel) continue;
        rpc::Channel* ch = it->second.channel.get();
        any = ch->poll() || any;
        // A plugin that exited or broke the protocol: record why, fail what it was doing.
        it = impl_->running.find(id);
        if (it != impl_->running.end() && ch->closed() && it->second.state != RunState::Failed && ch->pendingRequests() == 0) {
            it->second.state = RunState::Failed;
            it->second.failure = ch->closeReason();
            std::vector<std::int64_t> waiting;
            for (const auto& [callId, call] : it->second.calls)
                if (!call.channelId) waiting.push_back(callId);
            for (std::int64_t callId : waiting) {
                Done d = std::move(it->second.calls.at(callId).done);
                it->second.calls.erase(callId);
                rpc::Outcome o;
                o.status = rpc::Outcome::Status::Closed;
                o.message = it->second.failure;
                if (d) d(o);
                it = impl_->running.find(id);
                if (it == impl_->running.end()) break;
            }
        }
    }
    if (!any && wait.count() > 0) std::this_thread::sleep_for(std::min(wait, std::chrono::milliseconds(10)));
}

bool PluginHost::busy() const {
    for (const auto& [id, r] : impl_->running)
        if (r.state == RunState::Starting || !r.calls.empty()) return true;
    return false;
}

std::vector<PluginHost::ActiveCall> PluginHost::activeCalls() const {
    std::vector<ActiveCall> out;
    for (const auto& [id, r] : impl_->running)
        for (const auto& [callId, c] : r.calls) out.push_back(ActiveCall{id, r.name, callId, c.title, c.progress, c.fraction});
    return out;
}

}  // namespace ob::plugins

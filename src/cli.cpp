#include "openbooks/cli.hpp"

#include "openbooks/book.hpp"
#include "openbooks/crypto.hpp"
#include "openbooks/import.hpp"
#include "openbooks/invoice_pdf.hpp"
#include "openbooks/reports.hpp"
#include "openbooks/util.hpp"

#include "console.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

namespace ob {
namespace {

// ------------------------------------------------------------- arguments

// Options that never take a value.
const std::set<std::string> kFlags = {"csv", "open", "all", "finish", "empty", "no-header", "negate", "non-taxable", "force", "password",
                                      "help"};

class Args {
public:
    explicit Args(const std::vector<std::string>& tokens) {
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            const std::string& tok = tokens[i];
            if (tok.size() > 2 && startsWith(tok, "--")) {
                std::string name = tok.substr(2);
                const auto eq = name.find('=');
                if (eq != std::string::npos) {
                    options_.emplace_back(name.substr(0, eq), name.substr(eq + 1));
                } else if (kFlags.count(name)) {
                    flags_.insert(name);
                } else {
                    if (i + 1 >= tokens.size()) throw Error("option --" + name + " needs a value");
                    options_.emplace_back(name, tokens[++i]);
                }
            } else {
                positional_.push_back(tok);
            }
        }
    }

    std::string positional(std::size_t i, const std::string& what) {
        if (i >= positional_.size()) throw Error("missing " + what);
        positionalUsed_ = std::max(positionalUsed_, i + 1);
        return positional_[i];
    }
    std::optional<std::string> positionalOpt(std::size_t i) {
        if (i >= positional_.size()) return std::nullopt;
        positionalUsed_ = std::max(positionalUsed_, i + 1);
        return positional_[i];
    }
    std::optional<std::string> get(const std::string& name) {
        used_.insert(name);
        for (auto it = options_.rbegin(); it != options_.rend(); ++it) {
            if (it->first == name) return it->second;
        }
        return std::nullopt;
    }
    std::string require(const std::string& name) {
        auto v = get(name);
        if (!v) throw Error("missing required option --" + name);
        return *v;
    }
    std::vector<std::string> getAll(const std::string& name) {
        used_.insert(name);
        std::vector<std::string> out;
        for (const auto& [key, value] : options_) {
            if (key == name) out.push_back(value);
        }
        return out;
    }
    bool flag(const std::string& name) {
        used_.insert(name);
        return flags_.count(name) > 0;
    }
    // Rejects anything the command did not ask for, so typos never pass silently.
    void finish() const {
        for (const auto& [key, value] : options_) {
            if (!used_.count(key)) throw Error("unknown option --" + key + " for this command");
        }
        for (const auto& f : flags_) {
            if (!used_.count(f)) throw Error("unknown option --" + f + " for this command");
        }
        if (positionalUsed_ < positional_.size())
            throw Error("unexpected argument '" + positional_[positionalUsed_] + "'");
    }

private:
    std::vector<std::string> positional_;
    std::vector<std::pair<std::string, std::string>> options_;
    std::set<std::string> flags_;
    std::set<std::string> used_;
    std::size_t positionalUsed_ = 0;
};

// ---------------------------------------------------------------- context

struct Context {
    std::string path;
    std::ostream& out;
    std::ostream& err;
    std::istream& in;
    std::optional<Book> book;
    bool dirty = false;
    std::unique_ptr<FileKey> key;  // set once an encrypted file has been unlocked

    Book& books();
    Book& modify() {
        dirty = true;
        return books();
    }
};

// Asks for a password: hidden on an interactive console, otherwise one line from the input
// stream (pipes, scripts, tests).
std::string askPassword(Context& c, const std::string& prompt) {
    c.err << prompt << std::flush;
    std::string password;
    if (&c.in == &std::cin && readHiddenConsoleLine(password)) {
        c.err << '\n';
        return password;
    }
    if (!std::getline(c.in, password)) throw Error("no password was entered");
    if (!password.empty() && password.back() == '\r') password.pop_back();
    // Windows PowerShell prefixes piped input with a UTF-8 byte-order mark.
    if (startsWith(password, "\xEF\xBB\xBF")) password.erase(0, 3);
    return password;
}

Book& Context::books() {
    if (book) return *book;
    if (key) {
        book = Book::loadWithKey(path, *key);  // already unlocked earlier in this session
    } else if (Book::isEncryptedFile(path)) {
        // OPENBOOKS_PASSWORD is for scripts; it is visible to other programs run by the same user.
        const char* env = std::getenv("OPENBOOKS_PASSWORD");
        std::string password = env && *env ? env : askPassword(*this, "Password for " + path + ": ");
        try {
            book = Book::load(path, password, &key);
        } catch (...) {
            crypto::wipe(password);
            throw;
        }
        crypto::wipe(password);
    } else {
        book = Book::load(path);
    }
    return *book;
}

// Asks for a new password twice.
std::unique_ptr<FileKey> askNewPassword(Context& c) {
    std::string first = askPassword(c, "New password (at least " + std::to_string(crypto::kMinPasswordLength) + " characters): ");
    std::string second = askPassword(c, "Repeat the new password: ");
    const bool same = first == second;
    crypto::wipe(second);
    if (!same) {
        crypto::wipe(first);
        throw Error("the passwords don't match");
    }
    try {
        auto key = FileKey::fromNewPassword(first);
        crypto::wipe(first);
        return key;
    } catch (...) {
        crypto::wipe(first);
        throw;
    }
}

// ---------------------------------------------------------------- parsing

Money moneyArg(const std::string& text, const std::string& what) {
    const auto m = Money::parse(text);
    if (!m) throw Error("invalid " + what + ": '" + text + "'");
    return *m;
}

Money positiveMoneyArg(const std::string& text, const std::string& what) {
    const Money m = moneyArg(text, what);
    if (m <= Money()) throw Error(what + " must be greater than zero");
    return m;
}

Date dateArg(const std::string& text, const std::string& what) {
    const auto d = Date::parse(text);
    if (!d) throw Error("invalid " + what + " '" + text + "' (use YYYY-MM-DD, MM/DD/YYYY or 'today')");
    return *d;
}

Date dateOr(Args& a, const std::string& name, Date fallback) {
    const auto v = a.get(name);
    return v ? dateArg(*v, "--" + name) : fallback;
}

std::optional<Date> optionalDate(Args& a, const std::string& name) {
    const auto v = a.get(name);
    if (!v) return std::nullopt;
    return dateArg(*v, "--" + name);
}

int intArg(const std::string& text, const std::string& what, long long min, long long max) {
    const auto v = parseInt(text);
    if (!v || *v < min || *v > max) throw Error("invalid " + what + ": '" + text + "'");
    return static_cast<int>(*v);
}

int txnIdArg(const std::string& text) {
    std::string t = trim(text);
    if (!t.empty() && t[0] == '#') t.erase(0, 1);
    return intArg(t, "transaction id", 1, 2147483647);
}

std::vector<int> txnIdList(const std::string& text) {
    std::vector<int> ids;
    for (const auto& part : split(text, ',')) {
        if (!trim(part).empty()) ids.push_back(txnIdArg(part));
    }
    return ids;
}

bool yesNo(const std::string& text, const std::string& what) {
    const std::string l = toLower(trim(text));
    if (l == "yes" || l == "y" || l == "true" || l == "1") return true;
    if (l == "no" || l == "n" || l == "false" || l == "0") return false;
    throw Error("invalid " + what + " '" + text + "' (use yes or no)");
}

// "ACCOUNT=AMOUNT" (split on the last '=' so account names may contain '=').
std::pair<std::string, std::string> splitAssignment(const std::string& text, const std::string& what) {
    const auto eq = text.rfind('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 == text.size())
        throw Error(what + " expects NAME=AMOUNT, got '" + text + "'");
    return {trim(text.substr(0, eq)), trim(text.substr(eq + 1))};
}

// Line spec: "item=Consulting;qty=10;rate=150;desc=October work;account=Service Revenue;taxable=no"
DocLine parseLineSpec(const Book& b, DocKind kind, const std::string& spec) {
    static const std::set<std::string> keys = {"item", "desc", "qty", "rate", "amount", "account", "taxable"};
    std::map<std::string, std::string> kv;
    for (const auto& part : split(spec, ';')) {
        const std::string p = trim(part);
        if (p.empty()) continue;
        const auto eq = p.find('=');
        if (eq == std::string::npos) throw Error("line part '" + p + "' is not key=value (in --line \"" + spec + "\")");
        const std::string key = toLower(trim(p.substr(0, eq)));
        if (!keys.count(key)) throw Error("unknown line key '" + key + "'; use item, desc, qty, rate, amount, account, taxable");
        kv[key] = trim(p.substr(eq + 1));
    }

    DocLine line;
    if (kv.count("item")) {
        const Item& item = b.findItem(kv["item"]);
        if (!item.active) throw Error("item '" + item.name + "' is inactive");
        line.itemId = item.id;
        line.description = item.description.empty() ? item.name : item.description;
        line.rate = item.price;
        line.accountId = kind == DocKind::Bill ? item.expenseAccountId : item.incomeAccountId;
        line.taxable = item.taxable;
    }
    if (kv.count("desc")) line.description = kv["desc"];
    if (kv.count("account")) line.accountId = b.findAccount(kv["account"]).id;
    if (kv.count("qty")) {
        const auto q = Decimal::parse(kv["qty"]);
        if (!q) throw Error("invalid qty '" + kv["qty"] + "'");
        line.quantity = *q;
    }
    if (kv.count("rate")) line.rate = moneyArg(kv["rate"], "rate");
    if (kv.count("amount")) {
        if (kv.count("qty") || kv.count("rate")) throw Error("use either amount= or qty=/rate= on a line, not both");
        line.quantity = Decimal::fromInt(1);
        line.rate = moneyArg(kv["amount"], "amount");
    }
    if (kv.count("taxable")) line.taxable = yesNo(kv["taxable"], "taxable");
    if (!kv.count("item") && !kv.count("rate") && !kv.count("amount"))
        throw Error("line \"" + spec + "\" needs item=, rate= or amount=");
    if (line.accountId == 0)
        throw Error("line \"" + spec + "\" needs account= (the item has no " +
                    (kind == DocKind::Bill ? "expense" : "income") + " account)");
    if (line.description.empty()) line.description = b.account(line.accountId).name;
    return line;
}

void print(Context& c, const Table& t, bool csv) { c.out << (csv ? renderCsv(t) : renderText(t)); }

const char* partyNoun(ContactKind k) { return k == ContactKind::Customer ? "customer" : "vendor"; }

// --------------------------------------------------------------- commands

void cmdInit(Context& c, Args& a) {
    const auto name = a.get("company");
    const bool empty = a.flag("empty");
    const bool protect = a.flag("password");
    a.finish();
    if (std::filesystem::exists(std::filesystem::u8path(c.path)))
        throw Error("'" + c.path + "' already exists; refusing to overwrite it");
    if (protect) c.key = askNewPassword(c);
    Book b;
    if (name) b.company.name = *name;
    if (!empty) b.createDefaultChart();
    c.book = std::move(b);
    c.dirty = true;
    c.out << "Created new " << (protect ? "password-protected " : "") << "books for " << c.book->company.name
          << " in '" << c.path << "'";
    if (!empty) c.out << " with " << c.book->accounts().size() << " starter accounts";
    c.out << ".\n";
}

void cmdPassword(Context& c, Args& a) {
    const std::string sub = a.positional(0, "subcommand (status, set, remove)");
    a.finish();
    if (sub == "status") {
        if (!Book::isEncryptedFile(c.path)) {
            c.out << "'" << c.path << "' is not password protected.\n";
            return;
        }
        c.books();  // unlock, to report the actual settings
        c.out << "'" << c.path << "' is password protected: AES-256-GCM, key from PBKDF2-HMAC-SHA256 with "
              << c.key->iterations() << " iterations (" << crypto::backendName() << ").\n";
    } else if (sub == "set") {
        c.books();  // asks for the current password first if there is one
        const bool changing = c.key != nullptr;
        c.key = askNewPassword(c);
        c.dirty = true;
        c.out << (changing ? "Password changed" : "Password set") << " for '" << c.path << "'.\n"
              << "Keep it safe: without it these books can't be recovered by anyone.\n";
    } else if (sub == "remove") {
        c.books();
        if (!c.key) throw Error("'" + c.path + "' is not password protected");
        c.key.reset();
        c.dirty = true;
        c.out << "Password removed: '" << c.path << "' is now stored unencrypted.\n";
    } else {
        throw Error("unknown password subcommand '" + sub + "'");
    }
}

void cmdCompany(Context& c, Args& a) {
    const auto sub = a.positionalOpt(0);
    if (!sub || *sub == "show") {
        a.finish();
        const Book& b = c.books();
        const Company& co = b.company;
        auto role = [&](int id) { return id ? accountLabel(b.account(id)) : std::string("(not set)"); };
        c.out << "Company:               " << co.name << '\n'
              << "Address:               " << co.address << '\n'
              << "Email:                 " << co.email << '\n'
              << "Phone:                 " << co.phone << '\n'
              << "Invoice footer:        " << co.invoiceFooter << '\n'
              << "Paper size:            " << (co.paperSize == PaperSize::A4 ? "A4" : "Letter") << '\n'
              << "Fiscal year starts:    month " << co.fiscalYearStartMonth << '\n'
              << "Default terms:         Net " << co.defaultTermsDays << '\n'
              << "Books closed through:  " << (co.closedThrough ? co.closedThrough->str() : "(open)") << '\n'
              << "Next invoice number:   " << co.nextInvoiceNumber << '\n'
              << "A/R account:           " << role(co.receivablesAccountId) << '\n'
              << "A/P account:           " << role(co.payablesAccountId) << '\n'
              << "Sales tax account:     " << role(co.salesTaxAccountId) << '\n'
              << "Retained earnings:     " << role(co.retainedEarningsAccountId) << '\n';
        return;
    }
    if (*sub != "set") throw Error("usage: company [show] | company set [options]");
    const auto name = a.get("name");
    const auto address = a.get("address");
    const auto fy = a.get("fiscal-year-start");
    const auto terms = a.get("terms");
    const auto close = a.get("close-through");
    const auto next = a.get("next-invoice");
    const auto ar = a.get("ar-account");
    const auto ap = a.get("ap-account");
    const auto tax = a.get("sales-tax-account");
    const auto re = a.get("retained-earnings-account");
    const auto email = a.get("email");
    const auto phone = a.get("phone");
    const auto footer = a.get("invoice-footer");
    const auto paper = a.get("paper");
    a.finish();

    Book& b = c.modify();
    Company& co = b.company;
    if (name) co.name = trim(*name);
    if (address) co.address = *address;
    if (email) co.email = trim(*email);
    if (phone) co.phone = trim(*phone);
    if (footer) co.invoiceFooter = *footer;
    if (paper) {
        if (iequals(trim(*paper), "letter")) co.paperSize = PaperSize::Letter;
        else if (iequals(trim(*paper), "a4")) co.paperSize = PaperSize::A4;
        else throw Error("--paper must be letter or a4");
    }
    if (fy) co.fiscalYearStartMonth = intArg(*fy, "--fiscal-year-start (1-12)", 1, 12);
    if (terms) co.defaultTermsDays = intArg(*terms, "--terms", 0, 3650);
    if (next) co.nextInvoiceNumber = intArg(*next, "--next-invoice", 1, 2000000000);
    if (close) {
        if (iequals(trim(*close), "none")) co.closedThrough.reset();
        else co.closedThrough = dateArg(*close, "--close-through");
    }
    if (ar) b.setSystemAccount(SystemAccount::Receivables, b.findAccount(*ar).id);
    if (ap) b.setSystemAccount(SystemAccount::Payables, b.findAccount(*ap).id);
    if (tax) b.setSystemAccount(SystemAccount::SalesTax, b.findAccount(*tax).id);
    if (re) b.setSystemAccount(SystemAccount::RetainedEarnings, b.findAccount(*re).id);
    c.out << "Company settings updated.\n";
}

void cmdAccount(Context& c, Args& a) {
    const std::string sub = a.positional(0, "subcommand (list, add, edit, deactivate, activate)");
    if (sub == "list") {
        const bool all = a.flag("all");
        const bool csv = a.flag("csv");
        a.finish();
        print(c, accountList(c.books(), all), csv);
    } else if (sub == "add") {
        Account acct;
        acct.name = a.positional(1, "account name");
        const auto type = parseAccountType(a.require("type"));
        if (!type) throw Error("--type must be asset, liability, equity, income or expense");
        acct.type = *type;
        if (auto n = a.get("number")) acct.number = *n;
        if (auto d = a.get("desc")) acct.description = *d;
        a.finish();
        const int id = c.modify().addAccount(acct);
        c.out << "Added account " << accountLabel(c.books().account(id)) << " (" << toString(acct.type) << ").\n";
    } else if (sub == "edit") {
        const std::string ref = a.positional(1, "account");
        const auto name = a.get("name");
        const auto number = a.get("number");
        const auto desc = a.get("desc");
        a.finish();
        Book& b = c.modify();
        Account acct = b.findAccount(ref);
        if (name) acct.name = *name;
        if (number) acct.number = *number;
        if (desc) acct.description = *desc;
        b.updateAccount(acct);
        c.out << "Updated account " << accountLabel(b.account(acct.id)) << ".\n";
    } else if (sub == "deactivate" || sub == "activate") {
        const std::string ref = a.positional(1, "account");
        a.finish();
        Book& b = c.modify();
        Account acct = b.findAccount(ref);
        acct.active = sub == "activate";
        b.updateAccount(acct);
        c.out << (acct.active ? "Activated " : "Deactivated ") << accountLabel(acct) << ".\n";
    } else {
        throw Error("unknown account subcommand '" + sub + "'");
    }
}

void readContactOptions(Args& a, Contact& ct) {
    if (auto v = a.get("email")) ct.email = *v;
    if (auto v = a.get("phone")) ct.phone = *v;
    if (auto v = a.get("address")) ct.address = *v;
    if (auto v = a.get("terms")) ct.termsDays = iequals(trim(*v), "default") ? -1 : intArg(*v, "--terms", 0, 3650);
}

void cmdContact(Context& c, Args& a, ContactKind kind) {
    const std::string noun = partyNoun(kind);
    const std::string sub = a.positional(0, "subcommand (list, add, edit, show, deactivate, activate)");
    if (sub == "list") {
        const bool all = a.flag("all");
        const bool csv = a.flag("csv");
        a.finish();
        print(c, contactList(c.books(), kind, all), csv);
    } else if (sub == "add") {
        Contact ct;
        ct.kind = kind;
        ct.name = a.positional(1, noun + " name");
        readContactOptions(a, ct);
        a.finish();
        const int id = c.modify().addContact(ct);
        c.out << "Added " << noun << " " << trim(ct.name) << " (#" << id << ").\n";
    } else if (sub == "edit") {
        const std::string ref = a.positional(1, noun);
        const auto name = a.get("name");
        Book& b = c.modify();
        Contact ct = b.findContact(kind, ref);
        if (name) ct.name = *name;
        readContactOptions(a, ct);  // only overwrites the fields that were given
        a.finish();
        b.updateContact(ct);
        c.out << "Updated " << noun << " " << ct.name << ".\n";
    } else if (sub == "show") {
        const std::string ref = a.positional(1, noun);
        a.finish();
        const Book& b = c.books();
        const Contact& ct = b.findContact(kind, ref);
        c.out << ct.name << "  (#" << ct.id << (ct.active ? "" : ", inactive") << ")\n";
        if (!ct.email.empty()) c.out << "Email:   " << ct.email << '\n';
        if (!ct.phone.empty()) c.out << "Phone:   " << ct.phone << '\n';
        if (!ct.address.empty()) c.out << "Address: " << ct.address << '\n';
        c.out << "Terms:   "
              << (ct.termsDays < 0 ? "Net " + std::to_string(b.company.defaultTermsDays) + " (default)"
                                   : "Net " + std::to_string(ct.termsDays))
              << '\n';
        c.out << "Open balance: " << b.contactBalance(ct.id).formatted() << "\n\n";
        const DocKind docKind = kind == ContactKind::Customer ? DocKind::Invoice : DocKind::Bill;
        c.out << renderText(documentList(b, docKind, false, ct.id, Date::today()));
    } else if (sub == "deactivate" || sub == "activate") {
        const std::string ref = a.positional(1, noun);
        a.finish();
        Book& b = c.modify();
        Contact ct = b.findContact(kind, ref);
        ct.active = sub == "activate";
        b.updateContact(ct);
        c.out << (ct.active ? "Activated " : "Deactivated ") << noun << " " << ct.name << ".\n";
    } else {
        throw Error("unknown " + noun + " subcommand '" + sub + "'");
    }
}

void cmdItem(Context& c, Args& a) {
    const std::string sub = a.positional(0, "subcommand (list, add, deactivate, activate)");
    if (sub == "list") {
        const bool all = a.flag("all");
        const bool csv = a.flag("csv");
        a.finish();
        print(c, itemList(c.books(), all), csv);
    } else if (sub == "add") {
        Item item;
        item.name = a.positional(1, "item name");
        item.price = moneyArg(a.require("price"), "--price");
        const auto income = a.get("income");
        const auto expense = a.get("expense");
        if (auto d = a.get("desc")) item.description = *d;
        item.taxable = !a.flag("non-taxable");
        a.finish();
        Book& b = c.modify();
        if (income) item.incomeAccountId = b.findAccount(*income).id;
        if (expense) item.expenseAccountId = b.findAccount(*expense).id;
        const int id = b.addItem(item);
        c.out << "Added item " << trim(item.name) << " (#" << id << ") at " << item.price.formatted() << ".\n";
    } else if (sub == "deactivate" || sub == "activate") {
        const std::string ref = a.positional(1, "item");
        a.finish();
        Book& b = c.modify();
        const Item& item = b.findItem(ref);
        b.setItemActive(item.id, sub == "activate");
        c.out << (sub == "activate" ? "Activated " : "Deactivated ") << "item " << item.name << ".\n";
    } else {
        throw Error("unknown item subcommand '" + sub + "'");
    }
}

Decimal taxArg(const std::string& text) {
    std::string rate = trim(text);
    if (!rate.empty() && rate.back() == '%') rate.pop_back();
    const auto r = Decimal::parse(rate);
    if (!r) throw Error("invalid --tax '" + text + "' (a percentage such as 8.25)");
    return *r;
}

void writePdfFile(Context& c, const Book& b, int documentId, const std::optional<std::string>& outPath, bool force) {
    const std::filesystem::path target = std::filesystem::u8path(outPath ? *outPath : documentPdfFileName(b, documentId));
    if (std::filesystem::exists(target) && !force)
        throw Error("'" + target.u8string() + "' already exists; use --force to overwrite it");
    const std::string bytes = documentPdf(b, documentId);
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) throw Error("could not write '" + target.u8string() + "'");
    c.out << "Wrote " << target.u8string() << " (" << bytes.size() / 1024 + 1 << " KB).\n";
}

// Applies a credit memo to "INVOICE[=AMOUNT]"; without an amount, as much as both allow.
void applyCreditSpec(Book& b, int creditMemoId, const std::string& spec, std::ostream& out) {
    const auto eq = spec.rfind('=');
    const Document& inv = b.findDocument(DocKind::Invoice, trim(eq == std::string::npos ? spec : spec.substr(0, eq)));
    const int invoiceId = inv.id;
    Money amount;
    if (eq == std::string::npos) {
        const Money owed = b.documentBalance(invoiceId);
        const Money credit = b.documentBalance(creditMemoId);
        amount = owed < credit ? owed : credit;
    } else {
        amount = positiveMoneyArg(spec.substr(eq + 1), "applied amount");
    }
    b.applyCredit(creditMemoId, invoiceId, amount);
    out << "  applied " << amount.formatted() << " to invoice " << b.document(invoiceId).number << " (remaining "
        << b.documentBalance(invoiceId).formatted() << ")\n";
}

void cmdDocument(Context& c, Args& a, DocKind kind) {
    const std::string noun = docNoun(kind);
    const ContactKind party = partyKind(kind);
    const std::string partyOpt = partyNoun(party);
    const std::string sub = a.positional(0, "subcommand");

    if (sub == "create") {
        const std::string who = a.require(partyOpt);
        const auto dateText = a.get("date");
        std::optional<std::string> dueText;
        if (kind == DocKind::Estimate) dueText = a.get("expires");
        else if (docHasDueDate(kind)) dueText = a.get("due");
        const auto number = a.get("number");
        const auto memo = a.get("memo");
        std::optional<std::string> tax;
        if (kind != DocKind::Bill) tax = a.get("tax");
        std::optional<std::string> deposit;
        if (kind == DocKind::SalesReceipt) deposit = a.require("deposit-to");
        std::vector<std::string> applies;
        if (kind == DocKind::CreditMemo) applies = a.getAll("apply");
        const auto specs = a.getAll("line");
        a.finish();
        if (specs.empty())
            throw Error("at least one --line is required, e.g. --line \"desc=Consulting;qty=2;rate=100;account=Service Revenue\"");

        Book& b = c.modify();
        const Contact& ct = b.findContact(party, who);
        Document d;
        d.kind = kind;
        d.contactId = ct.id;
        d.date = dateText ? dateArg(*dateText, "--date") : Date::today();
        const int terms = ct.termsDays >= 0 ? ct.termsDays : b.company.defaultTermsDays;
        d.dueDate = dueText ? dateArg(*dueText, kind == DocKind::Estimate ? "--expires" : "--due")
                            : d.date.addDays(kind == DocKind::Estimate ? 30 : terms);
        if (number) d.number = *number;
        if (memo) d.memo = *memo;
        if (tax) d.taxRate = taxArg(*tax);
        if (deposit) d.depositAccountId = b.findAccount(*deposit).id;
        for (const auto& spec : specs) d.lines.push_back(parseLineSpec(b, kind, spec));
        const int id = b.createDocument(d);
        const Document& doc = b.document(id);
        c.out << "Created " << noun << " " << doc.number << " for " << ct.name << ": total " << doc.total().formatted();
        if (kind == DocKind::Estimate) c.out << ", expires " << doc.dueDate;
        else if (docHasDueDate(kind)) c.out << ", due " << doc.dueDate;
        c.out << ".\n";
        for (const auto& spec : applies) applyCreditSpec(b, id, spec, c.out);
    } else if (sub == "list") {
        const bool open = a.flag("open");
        const bool csv = a.flag("csv");
        const auto who = a.get(partyOpt);
        a.finish();
        const Book& b = c.books();
        const int contactId = who ? b.findContact(party, *who).id : 0;
        print(c, documentList(b, kind, open, contactId, Date::today()), csv);
    } else if (sub == "show") {
        const std::string ref = a.positional(1, noun + " number");
        a.finish();
        const Book& b = c.books();
        c.out << renderDocument(b, b.findDocument(kind, ref).id);
    } else if (sub == "pdf") {
        const std::string ref = a.positional(1, noun + " number");
        const auto outPath = a.get("out");
        const bool force = a.flag("force");
        a.finish();
        const Book& b = c.books();
        writePdfFile(c, b, b.findDocument(kind, ref).id, outPath, force);
    } else if (sub == "void") {
        const std::string ref = a.positional(1, noun + " number");
        a.finish();
        Book& b = c.modify();
        const int id = b.findDocument(kind, ref).id;
        b.voidDocument(id);
        c.out << "Voided " << noun << " " << b.document(id).number << ".\n";
    } else if (kind == DocKind::Estimate && (sub == "accept" || sub == "decline" || sub == "reopen")) {
        const std::string ref = a.positional(1, "estimate number");
        a.finish();
        Book& b = c.modify();
        const int id = b.findDocument(kind, ref).id;
        const EstimateStatus status = sub == "accept"    ? EstimateStatus::Accepted
                                      : sub == "decline" ? EstimateStatus::Declined
                                                         : EstimateStatus::Pending;
        b.setEstimateStatus(id, status);
        c.out << "Estimate " << b.document(id).number << " is now " << toLower(toString(status)) << ".\n";
    } else if (kind == DocKind::Estimate && sub == "convert") {
        const std::string ref = a.positional(1, "estimate number");
        const Date date = dateOr(a, "date", Date::today());
        a.finish();
        Book& b = c.modify();
        const int id = b.findDocument(kind, ref).id;
        const int invoiceId = b.convertEstimate(id, date);
        const Document& inv = b.document(invoiceId);
        c.out << "Converted estimate " << b.document(id).number << " to invoice " << inv.number << " ("
              << inv.total().formatted() << ", due " << inv.dueDate << ").\n";
    } else if (kind == DocKind::CreditMemo && sub == "apply") {
        const std::string ref = a.positional(1, "credit memo number");
        const std::string invoice = a.require("invoice");
        const auto amount = a.get("amount");
        a.finish();
        Book& b = c.modify();
        const int id = b.findDocument(kind, ref).id;
        c.out << "Credit memo " << b.document(id).number << ":\n";
        applyCreditSpec(b, id, amount ? invoice + "=" + *amount : invoice, c.out);
        c.out << "  credit left: " << b.documentBalance(id).formatted() << '\n';
    } else if (kind == DocKind::CreditMemo && sub == "unapply") {
        const std::string ref = a.positional(1, "credit memo number");
        const std::string invoice = a.require("invoice");
        a.finish();
        Book& b = c.modify();
        const int id = b.findDocument(kind, ref).id;
        const int invoiceId = b.findDocument(DocKind::Invoice, invoice).id;
        b.unapplyCredit(id, invoiceId);
        c.out << "Removed credit memo " << b.document(id).number << " from invoice " << b.document(invoiceId).number
              << ".\n";
    } else {
        throw Error("unknown " + noun + " subcommand '" + sub + "'; run 'openbooks help " +
                    (kind == DocKind::CreditMemo ? std::string("credit-memo")
                     : kind == DocKind::SalesReceipt ? std::string("sales-receipt")
                                                     : toLower(toString(kind))) +
                    "'");
    }
}

void readRecurringOptions(Args& a, Book& b, RecurringInvoice& r, bool creating) {
    if (auto v = a.get("customer")) r.contactId = b.findContact(ContactKind::Customer, *v).id;
    if (auto v = a.get("frequency")) {
        const auto f = parseFrequency(*v);
        if (!f) throw Error("--frequency must be weekly, monthly or yearly");
        r.frequency = *f;
    }
    if (auto v = a.get("every")) r.interval = intArg(*v, "--every", 1, 120);
    if (auto v = a.get("start")) r.startDate = dateArg(*v, "--start");
    if (auto v = a.get("end")) {
        if (iequals(trim(*v), "none")) r.endDate.reset();
        else r.endDate = dateArg(*v, "--end");
    }
    if (auto v = a.get("tax")) r.taxRate = taxArg(*v);
    if (auto v = a.get("memo")) r.memo = *v;
    const auto specs = a.getAll("line");
    if (!specs.empty()) {
        r.lines.clear();
        for (const auto& spec : specs) r.lines.push_back(parseLineSpec(b, DocKind::Invoice, spec));
    } else if (creating) {
        throw Error("at least one --line is required");
    }
}

void cmdRecurring(Context& c, Args& a) {
    const std::string sub = a.positional(0, "subcommand (add, edit, list, show, run, pause, resume, delete)");
    if (sub == "list") {
        const bool csv = a.flag("csv");
        a.finish();
        print(c, recurringList(c.books(), Date::today()), csv);
    } else if (sub == "add") {
        const std::string name = a.positional(1, "name");
        a.require("customer");  // mark required before the book is loaded
        Book& b = c.modify();
        RecurringInvoice r;
        r.name = name;
        r.startDate = Date::today();
        readRecurringOptions(a, b, r, true);
        a.finish();
        const int id = b.addRecurring(r);
        const RecurringInvoice& saved = b.recurringInvoice(id);
        c.out << "Added recurring invoice '" << saved.name << "': " << saved.total().formatted() << ", "
              << toLower(saved.scheduleText()) << ", first on " << saved.nextDate() << ".\n";
    } else if (sub == "edit") {
        const std::string ref = a.positional(1, "recurring invoice");
        const auto newName = a.get("name");
        Book& b = c.modify();
        RecurringInvoice r = b.findRecurring(ref);
        if (newName) r.name = *newName;
        readRecurringOptions(a, b, r, false);
        a.finish();
        b.updateRecurring(r);
        c.out << "Updated recurring invoice '" << trim(r.name) << "'.\n";
    } else if (sub == "show") {
        const std::string ref = a.positional(1, "recurring invoice");
        a.finish();
        const Book& b = c.books();
        const RecurringInvoice& r = b.findRecurring(ref);
        c.out << r.name << (r.active ? "" : "  (paused)") << '\n'
              << "Customer:  " << b.contact(r.contactId).name << '\n'
              << "Schedule:  " << r.scheduleText() << " from " << r.startDate
              << (r.endDate ? " until " + r.endDate->str() : std::string()) << '\n'
              << "Next:      " << (r.finished() ? std::string("(finished)") : r.nextDate().str()) << '\n'
              << "Created:   " << r.occurrencesCreated << " invoice(s)\n"
              << "Amount:    " << r.total().formatted() << '\n';
        for (const auto& l : r.lines)
            c.out << "  - " << l.description << "  " << l.quantity.str() << " x " << l.rate.formatted() << " = "
                  << l.amount.formatted() << '\n';
    } else if (sub == "pause" || sub == "resume") {
        const std::string ref = a.positional(1, "recurring invoice");
        a.finish();
        Book& b = c.modify();
        RecurringInvoice r = b.findRecurring(ref);
        r.active = sub == "resume";
        b.updateRecurring(r);
        c.out << (r.active ? "Resumed" : "Paused") << " '" << r.name << "'.\n";
    } else if (sub == "delete") {
        const std::string ref = a.positional(1, "recurring invoice");
        a.finish();
        Book& b = c.modify();
        const RecurringInvoice& r = b.findRecurring(ref);
        const std::string name = r.name;
        b.deleteRecurring(r.id);
        c.out << "Deleted recurring invoice '" << name << "' (invoices already created are kept).\n";
    } else if (sub == "run") {
        const Date through = dateOr(a, "through", Date::today());
        a.finish();
        Book& b = c.modify();
        const std::vector<int> created = b.createDueRecurringInvoices(through);
        if (created.empty()) {
            c.out << "No recurring invoices are due through " << through << ".\n";
            return;
        }
        for (int id : created) {
            const Document& d = b.document(id);
            c.out << "Created invoice " << d.number << " for " << b.contact(d.contactId).name << " dated " << d.date
                  << ": " << d.total().formatted() << '\n';
        }
        c.out << created.size() << " invoice(s) created.\n";
    } else {
        throw Error("unknown recurring subcommand '" + sub + "'");
    }
}

void cmdRecordPayment(Context& c, Args& a, PaymentKind kind) {
    const bool received = kind == PaymentKind::Received;
    const ContactKind party = received ? ContactKind::Customer : ContactKind::Vendor;
    const DocKind docKind = received ? DocKind::Invoice : DocKind::Bill;
    const std::string who = a.require(partyNoun(party));
    const std::string amount = a.require("amount");
    const std::string account = a.require(received ? "to" : "from");
    const auto dateText = a.get("date");
    const auto ref = a.get("ref");
    const auto memo = a.get("memo");
    const auto applies = a.getAll("apply");
    a.finish();

    Book& b = c.modify();
    Payment p;
    p.kind = kind;
    p.contactId = b.findContact(party, who).id;
    p.amount = positiveMoneyArg(amount, "--amount");
    p.accountId = b.findAccount(account).id;
    p.date = dateText ? dateArg(*dateText, "--date") : Date::today();
    if (ref) p.ref = *ref;
    if (memo) p.memo = *memo;
    Money remaining = p.amount;
    for (const auto& spec : applies) {
        const auto eq = spec.rfind('=');
        const std::string number = trim(eq == std::string::npos ? spec : spec.substr(0, eq));
        const Document& d = b.findDocument(docKind, number);
        Money applied;
        if (eq == std::string::npos) {  // apply as much as possible
            const Money due = b.documentBalance(d.id);
            applied = due < remaining ? due : remaining;
        } else {
            applied = moneyArg(spec.substr(eq + 1), "--apply amount");
        }
        remaining -= applied;
        p.applications.push_back(Application{d.id, applied});
    }

    const int id = b.recordPayment(p);
    const Payment& saved = b.payment(id);
    c.out << (received ? "Received " : "Paid ") << saved.amount.formatted() << (received ? " from " : " to ")
          << b.contact(saved.contactId).name << " (payment #" << id << ").\n";
    for (const auto& ap : saved.applications) {
        const Document& d = b.document(ap.documentId);
        c.out << "  applied " << ap.amount.formatted() << " to " << toLower(toString(d.kind)) << " " << d.number
              << " (remaining " << b.documentBalance(d.id).formatted() << ")\n";
    }
    if (!saved.unapplied().isZero()) c.out << "  unapplied credit: " << saved.unapplied().formatted() << '\n';
}

void cmdPayment(Context& c, Args& a) {
    const std::string sub = a.positional(0, "subcommand (list, void)");
    if (sub == "list") {
        const auto customer = a.get("customer");
        const auto vendor = a.get("vendor");
        const bool csv = a.flag("csv");
        a.finish();
        const Book& b = c.books();
        std::optional<PaymentKind> kind;
        int contactId = 0;
        if (customer) {
            kind = PaymentKind::Received;
            contactId = b.findContact(ContactKind::Customer, *customer).id;
        } else if (vendor) {
            kind = PaymentKind::Paid;
            contactId = b.findContact(ContactKind::Vendor, *vendor).id;
        }
        print(c, paymentList(b, kind, contactId), csv);
    } else if (sub == "void") {
        const int id = txnIdArg(a.positional(1, "payment id"));
        a.finish();
        c.modify().voidPayment(id);
        c.out << "Voided payment #" << id << ".\n";
    } else {
        throw Error("unknown payment subcommand '" + sub + "'");
    }
}

void readTxnHeader(Args& a, Transaction& t) {
    t.date = dateOr(a, "date", Date::today());
    if (auto v = a.get("payee")) t.payee = *v;
    if (auto v = a.get("memo")) t.memo = *v;
    if (auto v = a.get("ref")) t.ref = *v;
}

void cmdQuickEntry(Context& c, Args& a, TxnKind kind) {
    std::string debitRef;
    std::string creditRef;
    if (kind == TxnKind::Expense) {
        creditRef = a.require("from");
        debitRef = a.require("account");
    } else if (kind == TxnKind::Deposit) {
        debitRef = a.require("to");
        creditRef = a.require("account");
    } else {
        creditRef = a.require("from");
        debitRef = a.require("to");
    }
    const Money amount = positiveMoneyArg(a.require("amount"), "--amount");
    Transaction t;
    t.kind = kind;
    readTxnHeader(a, t);
    a.finish();

    Book& b = c.modify();
    const int debitId = b.findAccount(debitRef).id;
    const int creditId = b.findAccount(creditRef).id;
    t.splits = {Split{debitId, amount, "", SplitState::Uncleared}, Split{creditId, -amount, "", SplitState::Uncleared}};
    const int id = b.postTransaction(t);
    c.out << "Recorded " << toLower(toString(kind)) << " #" << id << ": " << amount.formatted() << " from "
          << b.account(creditId).name << " to " << b.account(debitId).name << ".\n";
}

void cmdJournal(Context& c, Args& a) {
    const auto debits = a.getAll("debit");
    const auto credits = a.getAll("credit");
    Transaction t;
    t.kind = TxnKind::Journal;
    readTxnHeader(a, t);
    a.finish();
    if (debits.empty() || credits.empty()) throw Error("a journal entry needs at least one --debit and one --credit");

    Book& b = c.modify();
    for (const auto& spec : debits) {
        const auto [acct, amount] = splitAssignment(spec, "--debit");
        t.splits.push_back(Split{b.findAccount(acct).id, positiveMoneyArg(amount, "debit amount"), "", SplitState::Uncleared});
    }
    for (const auto& spec : credits) {
        const auto [acct, amount] = splitAssignment(spec, "--credit");
        t.splits.push_back(Split{b.findAccount(acct).id, -positiveMoneyArg(amount, "credit amount"), "", SplitState::Uncleared});
    }
    const int id = b.postTransaction(t);
    c.out << "Posted journal entry #" << id << " for " << b.transaction(id).debitTotal().formatted() << ".\n";
}

void cmdTxn(Context& c, Args& a) {
    const std::string sub = a.positional(0, "subcommand (list, show, void, recategorize)");
    if (sub == "list") {
        Period p;
        p.from = optionalDate(a, "from");
        p.to = optionalDate(a, "to");
        const auto account = a.get("account");
        const bool csv = a.flag("csv");
        a.finish();
        const Book& b = c.books();
        print(c, transactionList(b, p, account ? b.findAccount(*account).id : 0), csv);
    } else if (sub == "show") {
        const int id = txnIdArg(a.positional(1, "transaction id"));
        a.finish();
        c.out << renderText(transactionDetail(c.books(), id));
    } else if (sub == "void") {
        const int id = txnIdArg(a.positional(1, "transaction id"));
        a.finish();
        c.modify().voidTransaction(id);
        c.out << "Voided transaction #" << id << ".\n";
    } else if (sub == "recategorize") {
        const int id = txnIdArg(a.positional(1, "transaction id"));
        const std::string from = a.require("from");
        const std::string to = a.require("to");
        a.finish();
        Book& b = c.modify();
        const int fromId = b.findAccount(from).id;
        const int toId = b.findAccount(to).id;
        b.recategorize(id, fromId, toId);
        c.out << "Moved transaction #" << id << " from " << b.account(fromId).name << " to " << b.account(toId).name
              << ".\n";
    } else {
        throw Error("unknown txn subcommand '" + sub + "'");
    }
}

void cmdRegister(Context& c, Args& a) {
    const std::string ref = a.positional(0, "account");
    Period p;
    p.from = optionalDate(a, "from");
    p.to = optionalDate(a, "to");
    const bool csv = a.flag("csv");
    a.finish();
    const Book& b = c.books();
    print(c, accountRegister(b, b.findAccount(ref).id, p), csv);
}

void cmdReport(Context& c, Args& a) {
    const std::string name = toLower(a.positional(
        0, "report name (trial-balance, balance-sheet, profit-loss, ar-aging, ap-aging, journal)"));
    const bool csv = a.flag("csv");
    const Date today = Date::today();
    const Book& b = c.books();
    Table t;
    if (name == "trial-balance" || name == "tb") {
        const Date asOf = dateOr(a, "as-of", today);
        a.finish();
        t = trialBalance(b, asOf);
    } else if (name == "balance-sheet" || name == "bs") {
        const Date asOf = dateOr(a, "as-of", today);
        a.finish();
        t = balanceSheet(b, asOf);
    } else if (name == "profit-loss" || name == "pnl" || name == "income-statement") {
        const Date to = dateOr(a, "to", today);
        const Date from = dateOr(a, "from", b.fiscalYearStart(to));
        a.finish();
        t = profitAndLoss(b, from, to);
    } else if (name == "ar-aging" || name == "ap-aging") {
        const Date asOf = dateOr(a, "as-of", today);
        a.finish();
        t = agingReport(b, name == "ar-aging" ? DocKind::Invoice : DocKind::Bill, asOf);
    } else if (name == "journal") {
        const Date to = dateOr(a, "to", today);
        const Date from = dateOr(a, "from", b.fiscalYearStart(to));
        a.finish();
        t = journalReport(b, Period{from, to});
    } else {
        throw Error("unknown report '" + name +
                    "'; available: trial-balance, balance-sheet, profit-loss, ar-aging, ap-aging, journal");
    }
    print(c, t, csv);
}

void cmdImportCsv(Context& c, Args& a) {
    const std::string accountRef = a.positional(0, "bank or card account");
    const std::string file = a.positional(1, "CSV file");
    const auto offset = a.get("offset");
    auto column = [&](const std::string& name, int fallback) {
        const auto v = a.get(name);
        return v ? intArg(*v, "--" + name, 1, 1000) : fallback;
    };
    CsvImportOptions options;
    options.dateColumn = column("date-col", 1);
    options.descriptionColumn = column("desc-col", 2);
    options.amountColumn = column("amount-col", 3);
    options.hasHeader = !a.flag("no-header");
    options.negate = a.flag("negate");
    a.finish();

    std::ifstream in(std::filesystem::u8path(file), std::ios::binary);
    if (!in) throw Error("cannot open '" + file + "'");
    std::stringstream buffer;
    buffer << in.rdbuf();

    Book& b = c.modify();
    const int accountId = b.findAccount(accountRef).id;
    if (offset) options.offsetAccountId = b.findAccount(*offset).id;
    const CsvImportResult result = importCsv(b, accountId, buffer.str(), options);
    c.out << "Imported " << result.imported << " transaction(s) into " << b.account(accountId).name << "; skipped "
          << result.skipped << " duplicate or zero row(s).\n";
    if (result.imported)
        c.out << "Use 'txn recategorize ID --from \"Uncategorized Expense\" --to ACCOUNT' to categorize them.\n";
}

void cmdReconcile(Context& c, Args& a) {
    const std::string ref = a.positional(0, "account");
    const Date through = dateOr(a, "through", Date::today());
    const auto clear = a.get("clear");
    const auto unclear = a.get("unclear");
    const auto statement = a.get("statement-balance");
    const bool finish = a.flag("finish");
    a.finish();
    if (finish && !statement) throw Error("--finish requires --statement-balance");

    Book& b = (clear || unclear || finish) ? c.modify() : c.books();
    const Account& acct = b.findAccount(ref);
    const int id = acct.id;
    if (acct.type != AccountType::Asset && acct.type != AccountType::Liability)
        throw Error("only asset and liability accounts (bank, cash, credit card) can be reconciled");
    if (clear) {
        for (int txn : txnIdList(*clear)) b.setSplitState(txn, id, SplitState::Cleared);
    }
    if (unclear) {
        for (int txn : txnIdList(*unclear)) b.setSplitState(txn, id, SplitState::Uncleared);
    }

    c.out << renderText(reconciliationWorksheet(b, id, through)) << '\n';
    const Money cleared = naturalSign(acct.type, b.clearedBalance(id));
    c.out << "Cleared balance:    " << cleared.formatted() << '\n';
    if (statement) {
        const Money target = moneyArg(*statement, "--statement-balance");
        const Money difference = target - cleared;
        c.out << "Statement balance:  " << target.formatted() << '\n'
              << "Difference:         " << difference.formatted() << '\n';
        if (finish) {
            if (!difference.isZero())
                throw Error("cannot finish: the cleared balance is off from the statement by " + difference.formatted());
            const int count = b.finishReconciliation(id);
            c.out << "Reconciliation complete: " << count << " line(s) marked reconciled.\n";
        }
    }
}

// ------------------------------------------------------------ dispatching

using Handler = std::function<void(Context&, Args&)>;

struct Command {
    std::string name;
    std::string usage;
    std::string summary;
    Handler run;
};

const std::vector<Command>& commands();

void printHelp(std::ostream& out) {
    out << "OpenBooks " << kVersion << " - open source double-entry accounting for small business\n\n"
        << "Usage: openbooks [-f FILE] <command> [arguments]\n"
        << "The books file defaults to $OPENBOOKS_FILE, then ./books.obk\n\n"
        << "Commands:\n";
    for (const auto& cmd : commands()) out << "  " << cmd.name << std::string(18 - std::min<std::size_t>(17, cmd.name.size()), ' ') << cmd.summary << '\n';
    out << "\nShortcuts: accounts, customers, vendors, items, invoices, estimates, credit-memos,\n"
        << "           sales-receipts, bills, payments, transactions\n"
        << "Run 'openbooks help COMMAND' for details.\n\n"
        << "References: accounts, customers, vendors and items may be given by name, a unique\n"
        << "name prefix, account number, or #id. Dates: YYYY-MM-DD, MM/DD/YYYY or 'today'.\n\n"
        << "Invoice/bill lines (--line) are key=value pairs separated by ';':\n"
        << "  item=NAME desc=TEXT qty=N rate=PRICE amount=TOTAL account=ACCOUNT taxable=yes|no\n"
        << "  e.g. --line \"item=Consulting;qty=12\"  --line \"desc=Parts;amount=80;account=Sales\"\n";
}

void cmdHelp(Context& c, Args& a) {
    const auto topic = a.positionalOpt(0);
    a.finish();
    if (!topic) {
        printHelp(c.out);
        return;
    }
    for (const auto& cmd : commands()) {
        if (cmd.name == *topic) {
            c.out << cmd.summary << "\n\nUsage:\n" << cmd.usage << '\n';
            return;
        }
    }
    throw Error("no command named '" + *topic + "'");
}

void runShell(Context& c);

const std::vector<Command>& commands() {
    using CK = ContactKind;
    static const std::vector<Command> list = {
        {"init", "  openbooks init [--company NAME] [--empty] [--password]\n"
                 "  --password asks for a password and creates the file encrypted.",
         "Create a new books file (starter chart of accounts unless --empty)", cmdInit},
        {"password",
         "  openbooks password status\n"
         "  openbooks password set       (add a password, or change it)\n"
         "  openbooks password remove    (store the file unencrypted again)\n"
         "  Encrypted files ask for their password when opened. For scripts, set OPENBOOKS_PASSWORD\n"
         "  (note: other programs you run can read environment variables). There is no recovery:\n"
         "  a forgotten password means the books can't be opened.",
         "Protect the books file with a password (AES-256-GCM)", cmdPassword},
        {"company",
         "  openbooks company [show]\n"
         "  openbooks company set [--name N] [--address A] [--email E] [--phone P] [--invoice-footer TEXT]\n"
         "                        [--paper letter|a4] [--fiscal-year-start MONTH] [--terms DAYS]\n"
         "                        [--next-invoice N] [--close-through DATE|none] [--ar-account A]\n"
         "                        [--ap-account A] [--sales-tax-account A] [--retained-earnings-account A]",
         "Show or change company settings, close the books through a date", cmdCompany},
        {"account",
         "  openbooks account list [--all] [--csv]\n"
         "  openbooks account add NAME --type asset|liability|equity|income|expense [--number N] [--desc TEXT]\n"
         "  openbooks account edit ACCOUNT [--name N] [--number N] [--desc TEXT]\n"
         "  openbooks account deactivate|activate ACCOUNT",
         "Manage the chart of accounts", cmdAccount},
        {"customer",
         "  openbooks customer list [--all] [--csv]\n"
         "  openbooks customer add NAME [--email E] [--phone P] [--address A] [--terms DAYS]\n"
         "  openbooks customer edit CUSTOMER [--name N] [--email E] [--phone P] [--address A] [--terms DAYS|default]\n"
         "  openbooks customer show CUSTOMER\n"
         "  openbooks customer deactivate|activate CUSTOMER",
         "Manage customers", [](Context& c, Args& a) { cmdContact(c, a, CK::Customer); }},
        {"vendor",
         "  openbooks vendor list [--all] [--csv]\n"
         "  openbooks vendor add NAME [--email E] [--phone P] [--address A] [--terms DAYS]\n"
         "  openbooks vendor edit VENDOR [--name N] [--email E] [--phone P] [--address A] [--terms DAYS|default]\n"
         "  openbooks vendor show VENDOR\n"
         "  openbooks vendor deactivate|activate VENDOR",
         "Manage vendors", [](Context& c, Args& a) { cmdContact(c, a, CK::Vendor); }},
        {"item",
         "  openbooks item list [--all] [--csv]\n"
         "  openbooks item add NAME --price P [--income ACCOUNT] [--expense ACCOUNT] [--desc TEXT] [--non-taxable]\n"
         "  openbooks item deactivate|activate ITEM",
         "Manage products and services", cmdItem},
        {"invoice",
         "  openbooks invoice create --customer C --line SPEC [--line SPEC ...] [--date D] [--due D]\n"
         "                           [--number N] [--tax PERCENT] [--memo TEXT]\n"
         "  openbooks invoice list [--open] [--customer C] [--csv]\n"
         "  openbooks invoice show NUMBER\n"
         "  openbooks invoice pdf NUMBER [--out FILE] [--force]\n"
         "  openbooks invoice void NUMBER",
         "Create, list, print and void invoices", [](Context& c, Args& a) { cmdDocument(c, a, DocKind::Invoice); }},
        {"estimate",
         "  openbooks estimate create --customer C --line SPEC [--line SPEC ...] [--date D] [--expires D]\n"
         "                            [--number N] [--tax PERCENT] [--memo TEXT]\n"
         "  openbooks estimate list [--open] [--customer C] [--csv]\n"
         "  openbooks estimate show|pdf|void NUMBER\n"
         "  openbooks estimate accept|decline|reopen NUMBER\n"
         "  openbooks estimate convert NUMBER [--date D]      (creates an invoice)\n"
         "  Estimates never touch the ledger until converted.",
         "Quotes that can be converted into invoices", [](Context& c, Args& a) { cmdDocument(c, a, DocKind::Estimate); }},
        {"credit-memo",
         "  openbooks credit-memo create --customer C --line SPEC [--line SPEC ...] [--date D] [--number N]\n"
         "                               [--tax PERCENT] [--memo TEXT] [--apply INVOICE[=AMOUNT] ...]\n"
         "  openbooks credit-memo apply NUMBER --invoice INVOICE [--amount X]\n"
         "  openbooks credit-memo unapply NUMBER --invoice INVOICE\n"
         "  openbooks credit-memo list|show|pdf|void ...\n"
         "  A credit memo reduces what the customer owes; apply it to their invoices.",
         "Customer credits (returns, discounts, corrections)",
         [](Context& c, Args& a) { cmdDocument(c, a, DocKind::CreditMemo); }},
        {"sales-receipt",
         "  openbooks sales-receipt create --customer C --deposit-to ACCOUNT --line SPEC [--line SPEC ...]\n"
         "                                 [--date D] [--number N] [--tax PERCENT] [--memo TEXT]\n"
         "  openbooks sales-receipt list|show|pdf|void ...\n"
         "  For sales paid on the spot: no invoice, the money goes straight into ACCOUNT.",
         "Record a sale paid immediately", [](Context& c, Args& a) { cmdDocument(c, a, DocKind::SalesReceipt); }},
        {"recurring",
         "  openbooks recurring add NAME --customer C --line SPEC [--line SPEC ...] [--frequency weekly|monthly|yearly]\n"
         "                          [--every N] [--start D] [--end D] [--tax PERCENT] [--memo TEXT]\n"
         "  openbooks recurring edit NAME [--name N] [same options as add; --line replaces all lines; --end none]\n"
         "  openbooks recurring list [--csv] | show NAME | pause NAME | resume NAME | delete NAME\n"
         "  openbooks recurring run [--through D]     (creates every invoice that is due)\n"
         "  Invoices are only created when you run 'recurring run' (or click Create in the app).",
         "Invoice templates that repeat on a schedule", cmdRecurring},
        {"bill",
         "  openbooks bill create --vendor V --line SPEC [--line SPEC ...] [--date D] [--due D] [--number N] [--memo TEXT]\n"
         "  openbooks bill list [--open] [--vendor V] [--csv]\n"
         "  openbooks bill show NUMBER\n"
         "  openbooks bill pdf NUMBER [--out FILE] [--force]\n"
         "  openbooks bill void NUMBER",
         "Enter, list and void vendor bills", [](Context& c, Args& a) { cmdDocument(c, a, DocKind::Bill); }},
        {"receive-payment",
         "  openbooks receive-payment --customer C --amount X --to ACCOUNT [--apply INVOICE[=AMOUNT] ...]\n"
         "                            [--date D] [--ref R] [--memo M]\n"
         "  Without --apply the payment pays the customer's oldest open invoices first.",
         "Record a customer payment against invoices",
         [](Context& c, Args& a) { cmdRecordPayment(c, a, PaymentKind::Received); }},
        {"pay-bill",
         "  openbooks pay-bill --vendor V --amount X --from ACCOUNT [--apply BILL[=AMOUNT] ...]\n"
         "                     [--date D] [--ref CHECKNO] [--memo M]\n"
         "  Without --apply the payment pays the vendor's oldest open bills first.",
         "Pay vendor bills", [](Context& c, Args& a) { cmdRecordPayment(c, a, PaymentKind::Paid); }},
        {"payment",
         "  openbooks payment list [--customer C | --vendor V] [--csv]\n"
         "  openbooks payment void ID",
         "List or void payments", cmdPayment},
        {"expense",
         "  openbooks expense --from ACCOUNT --account CATEGORY --amount X [--date D] [--payee P] [--memo M] [--ref R]",
         "Record money spent (e.g. a debit card purchase)",
         [](Context& c, Args& a) { cmdQuickEntry(c, a, TxnKind::Expense); }},
        {"deposit",
         "  openbooks deposit --to ACCOUNT --account CATEGORY --amount X [--date D] [--payee P] [--memo M] [--ref R]",
         "Record money received that is not an invoice payment",
         [](Context& c, Args& a) { cmdQuickEntry(c, a, TxnKind::Deposit); }},
        {"transfer",
         "  openbooks transfer --from ACCOUNT --to ACCOUNT --amount X [--date D] [--memo M] [--ref R]",
         "Move money between accounts (e.g. pay a credit card)",
         [](Context& c, Args& a) { cmdQuickEntry(c, a, TxnKind::Transfer); }},
        {"journal",
         "  openbooks journal --debit ACCOUNT=AMOUNT [...] --credit ACCOUNT=AMOUNT [...] [--date D] [--memo M] [--ref R]",
         "Post a general journal entry", cmdJournal},
        {"txn",
         "  openbooks txn list [--from D] [--to D] [--account A] [--csv]\n"
         "  openbooks txn show ID\n"
         "  openbooks txn void ID\n"
         "  openbooks txn recategorize ID --from ACCOUNT --to ACCOUNT",
         "List, inspect, void or recategorize transactions", cmdTxn},
        {"register", "  openbooks register ACCOUNT [--from D] [--to D] [--csv]",
         "Show an account register with running balance", cmdRegister},
        {"report",
         "  openbooks report trial-balance [--as-of D] [--csv]\n"
         "  openbooks report balance-sheet [--as-of D] [--csv]\n"
         "  openbooks report profit-loss [--from D] [--to D] [--csv]   (defaults to fiscal year to date)\n"
         "  openbooks report ar-aging|ap-aging [--as-of D] [--csv]\n"
         "  openbooks report journal [--from D] [--to D] [--csv]",
         "Financial reports", cmdReport},
        {"import-csv",
         "  openbooks import-csv ACCOUNT FILE [--offset ACCOUNT] [--date-col N] [--desc-col N] [--amount-col N]\n"
         "                       [--no-header] [--negate]\n"
         "  Columns default to date=1, description=2, amount=3. Positive amounts are money into the\n"
         "  account; use --negate if your bank exports the opposite sign. Rows already imported\n"
         "  (same date, description and amount) are skipped.",
         "Import a bank or credit card statement", cmdImportCsv},
        {"reconcile",
         "  openbooks reconcile ACCOUNT [--through D] [--clear TXN,TXN,...] [--unclear TXN,...]\n"
         "                      [--statement-balance X [--finish]]",
         "Reconcile a bank or credit card account against a statement", cmdReconcile},
        {"shell", "  openbooks shell", "Interactive mode: run many commands against the same books",
         [](Context& c, Args& a) {
             a.finish();
             runShell(c);
         }},
        {"help", "  openbooks help [COMMAND]", "Show help", cmdHelp},
        {"version", "  openbooks version", "Show the version",
         [](Context& c, Args& a) {
             a.finish();
             c.out << "OpenBooks " << kVersion << '\n';
         }},
    };
    return list;
}

const std::map<std::string, std::string> kShortcuts = {
    {"accounts", "account"}, {"customers", "customer"}, {"vendors", "vendor"},  {"items", "item"},
    {"invoices", "invoice"}, {"bills", "bill"},         {"payments", "payment"}, {"transactions", "txn"},
    {"estimates", "estimate"}, {"credit-memos", "credit-memo"}, {"sales-receipts", "sales-receipt"},
};

void dispatch(Context& c, std::vector<std::string> tokens) {
    if (tokens.empty()) {
        printHelp(c.out);
        return;
    }
    const auto shortcut = kShortcuts.find(tokens[0]);
    if (shortcut != kShortcuts.end()) {
        tokens[0] = shortcut->second;
        tokens.insert(tokens.begin() + 1, "list");
    }
    for (const auto& cmd : commands()) {
        if (cmd.name != tokens[0]) continue;
        if (std::find(tokens.begin() + 1, tokens.end(), "--help") != tokens.end()) {
            c.out << cmd.summary << "\n\nUsage:\n" << cmd.usage << '\n';
            return;
        }
        Args args(std::vector<std::string>(tokens.begin() + 1, tokens.end()));
        cmd.run(c, args);
        return;
    }
    throw Error("unknown command '" + tokens[0] + "'; run 'openbooks help'");
}

// Runs one command; saves on success, discards in-memory changes on failure.
int execute(Context& c, const std::vector<std::string>& tokens) {
    try {
        dispatch(c, tokens);
        if (c.dirty && c.book) {
            c.book->save(c.path, c.key.get());
            c.dirty = false;
        }
        return 0;
    } catch (const std::exception& e) {
        c.err << "error: " << e.what() << '\n';
    }
    c.book.reset();  // reload from disk so a half-applied command never lingers
    c.dirty = false;
    return 1;
}

void runShell(Context& c) {
    c.out << "OpenBooks shell (" << c.path << "). Type 'help' for commands, 'exit' to quit.\n";
    std::string line;
    while (true) {
        c.out << "openbooks> " << std::flush;
        if (!std::getline(c.in, line)) break;
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t == "exit" || t == "quit") break;
        std::vector<std::string> tokens;
        try {
            tokens = tokenize(t);
        } catch (const Error& e) {
            c.err << "error: " << e.what() << '\n';
            continue;
        }
        if (tokens.empty()) continue;
        if (tokens[0] == "shell") {
            c.out << "already in the shell\n";
            continue;
        }
        execute(c, tokens);
    }
    c.out << '\n';
}

}  // namespace

std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::string current;
    bool inToken = false;
    char quote = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (quote) {
            if (ch == quote) {
                quote = 0;
            } else if (ch == '\\' && quote == '"' && i + 1 < line.size() && (line[i + 1] == '"' || line[i + 1] == '\\')) {
                current += line[++i];
            } else {
                current += ch;
            }
        } else if (ch == '"' || ch == '\'') {
            quote = ch;
            inToken = true;
        } else if (std::isspace(static_cast<unsigned char>(ch))) {
            if (inToken) {
                tokens.push_back(current);
                current.clear();
                inToken = false;
            }
        } else {
            current += ch;
            inToken = true;
        }
    }
    if (quote) throw Error("unterminated quote");
    if (inToken) tokens.push_back(current);
    return tokens;
}

int runCli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err, std::istream& in) {
    std::string path;
    if (const char* env = std::getenv("OPENBOOKS_FILE")) path = env;
    std::size_t i = 0;
    for (; i < args.size(); ++i) {
        if ((args[i] == "-f" || args[i] == "--file") && i + 1 < args.size()) {
            path = args[++i];
        } else if (startsWith(args[i], "--file=")) {
            path = args[i].substr(7);
        } else {
            break;
        }
    }
    if (path.empty()) path = "books.obk";
    Context c{path, out, err, in, std::nullopt, false, nullptr};
    return execute(c, std::vector<std::string>(args.begin() + static_cast<std::ptrdiff_t>(i), args.end()));
}

}  // namespace ob

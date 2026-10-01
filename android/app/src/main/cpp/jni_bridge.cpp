// The bridge between the Kotlin UI and the OpenBooks engine.
//
// One JNI entry point, NativeBooks.callBytes(byte[] requestUtf8): the request is a JSON object
// {"op": "...", ...arguments}; the reply is {"ok": true, "data": ...} or
// {"ok": false, "code": "password_required" | "wrong_password" | "error", "error": "..."}.
// Money is exchanged as exact decimal strings, dates as "YYYY-MM-DD".
//
// Changes follow the desktop app's rule: apply to a copy, save (re-encrypting when the file has
// a password), and only then swap the copy in, so a failure never leaves half-applied state.
// A mutex serializes all calls.

#include "jni_env.hpp"
#include "json.hpp"

#include "openbooks/book.hpp"
#include "openbooks/cli.hpp"
#include "openbooks/crypto.hpp"
#include "openbooks/import.hpp"
#include "openbooks/invoice_pdf.hpp"
#include "openbooks/reports.hpp"
#include "openbooks/util.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace fs = std::filesystem;
using json::Value;
using json::Writer;
using namespace ob;

// ------------------------------------------------------------------ JNI plumbing

namespace jx {
namespace {
JavaVM* g_vm = nullptr;
jclass g_crypto = nullptr;
jmethodID g_random = nullptr, g_pbkdf2 = nullptr, g_encrypt = nullptr, g_decrypt = nullptr;
}  // namespace

bool initialize(JavaVM* vm, JNIEnv* env) {
    g_vm = vm;
    jclass local = env->FindClass("org/openbooks/core/NativeCrypto");
    if (!local) return false;
    g_crypto = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    g_random = env->GetStaticMethodID(g_crypto, "random", "(I)[B");
    g_pbkdf2 = env->GetStaticMethodID(g_crypto, "pbkdf2", "([B[BII)[B");
    g_encrypt = env->GetStaticMethodID(g_crypto, "gcmEncrypt", "([B[B[B[B)[B");
    g_decrypt = env->GetStaticMethodID(g_crypto, "gcmDecrypt", "([B[B[B[B)[B");
    return g_random && g_pbkdf2 && g_encrypt && g_decrypt;
}

JNIEnv* env() {
    JNIEnv* e = nullptr;
    if (!g_vm || g_vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6) != JNI_OK)
        throw Error("encryption is unavailable (no Java environment)");
    return e;
}

jclass cryptoClass() { return g_crypto; }
jmethodID cryptoRandom() { return g_random; }
jmethodID cryptoPbkdf2() { return g_pbkdf2; }
jmethodID cryptoEncrypt() { return g_encrypt; }
jmethodID cryptoDecrypt() { return g_decrypt; }
}  // namespace jx

namespace {

// ------------------------------------------------------------------ session

struct Session {
    std::optional<Book> book;
    std::string path;
    std::unique_ptr<FileKey> key;
};

Session g_session;
std::mutex g_mutex;

class ErrorWithCode : public Error {
public:
    ErrorWithCode(std::string code, const std::string& message) : Error(message), code_(std::move(code)) {}
    const std::string& code() const { return code_; }

private:
    std::string code_;
};

Book& book() {
    if (!g_session.book) throw Error("no books are open");
    return *g_session.book;
}

void mutate(const std::function<void(Book&)>& change) {
    Book draft = book();
    change(draft);
    draft.save(g_session.path, g_session.key.get());
    *g_session.book = std::move(draft);
}

// ------------------------------------------------------------------ argument parsing

Money moneyArg(const Value& v, std::string_view key) {
    const std::string text = v.str(key);
    const auto m = Money::parse(text);
    if (!m) throw Error("invalid amount '" + text + "'");
    return *m;
}

std::optional<Money> optionalMoney(const Value& v, std::string_view key) {
    if (trim(v.str(key)).empty()) return std::nullopt;
    return moneyArg(v, key);
}

Date dateArg(const Value& v, std::string_view key) {
    const std::string text = v.str(key);
    const auto d = Date::parse(text);
    if (!d) throw Error("invalid date '" + text + "'");
    return *d;
}

std::optional<Date> optionalDate(const Value& v, std::string_view key) {
    if (trim(v.str(key)).empty()) return std::nullopt;
    return dateArg(v, key);
}

Decimal decimalArg(const Value& v, std::string_view key, Decimal fallback) {
    std::string text = trim(v.str(key));
    if (!text.empty() && text.back() == '%') text.pop_back();
    if (text.empty()) return fallback;
    const auto d = Decimal::parse(text);
    if (!d) throw Error("invalid number '" + text + "'");
    return *d;
}

template <typename T>
T enumArg(const Value& v, std::string_view key, std::optional<T> (*parser)(std::string_view), const char* what) {
    const auto parsed = parser(v.str(key));
    if (!parsed) throw Error(std::string("invalid ") + what + " '" + v.str(key) + "'");
    return *parsed;
}

std::vector<DocLine> linesArg(const Value& v) {
    std::vector<DocLine> lines;
    for (const auto& l : v.list("lines")) {
        DocLine line;
        line.itemId = l.id("itemId");
        line.accountId = l.id("accountId");
        line.description = trim(l.str("description"));
        line.quantity = decimalArg(l, "quantity", Decimal::fromInt(1));
        line.rate = moneyArg(l, "rate");
        line.taxable = l.flag("taxable", true);
        lines.push_back(line);
    }
    return lines;
}

// ------------------------------------------------------------------ writers

void writeTable(Writer& w, const Table& t) {
    w.beginObject().field("title", t.title);
    w.key("subtitles").beginArray();
    for (const auto& s : t.subtitles) w.value(s);
    w.endArray();
    w.key("headers").beginArray();
    for (const auto& h : t.headers) w.value(h);
    w.endArray();
    w.key("numeric").beginArray();
    for (std::size_t i = 0; i < t.headers.size(); ++i) w.value(i < t.numeric.size() && t.numeric[i]);
    w.endArray();
    w.key("rows").beginArray();
    for (const auto& r : t.rows) {
        static const char* styles[] = {"normal", "section", "total", "spacer"};
        w.beginObject().field("style", styles[static_cast<int>(r.style)]).field("indent", r.indent);
        w.key("cells").beginArray();
        for (const auto& c : r.cells) w.value(c);
        w.endArray().endObject();
    }
    w.endArray().endObject();
}

Money lookupBalance(const std::map<int, Money>& m, int id) {
    const auto it = m.find(id);
    return it == m.end() ? Money() : it->second;
}

bool isMoneyAccount(const Book& b, const Account& a) {
    return (a.type == AccountType::Asset || a.type == AccountType::Liability) && !b.isSystemAccount(a.id);
}

std::string roleOf(const Book& b, int id) {
    const Company& c = b.company;
    if (id == c.receivablesAccountId) return "receivables";
    if (id == c.payablesAccountId) return "payables";
    if (id == c.salesTaxAccountId) return "salesTax";
    if (id == c.retainedEarningsAccountId) return "retainedEarnings";
    return "";
}

void writeAccount(Writer& w, const Book& b, const Account& a, const std::map<int, Money>& balances) {
    w.beginObject()
        .field("id", a.id)
        .field("number", a.number)
        .field("name", a.name)
        .field("label", accountLabel(a))
        .field("type", toString(a.type))
        .field("description", a.description)
        .field("active", a.active)
        .field("balance", naturalSign(a.type, lookupBalance(balances, a.id)).str())
        .field("role", roleOf(b, a.id))
        .field("money", isMoneyAccount(b, a))
        .field("subledger", b.isSubledgerAccount(a.id))
        .endObject();
}

void writeContact(Writer& w, const Book& b, const Contact& c) {
    w.beginObject()
        .field("id", c.id)
        .field("kind", toString(c.kind))
        .field("name", c.name)
        .field("email", c.email)
        .field("phone", c.phone)
        .field("address", c.address)
        .field("termsDays", c.termsDays)
        .field("active", c.active)
        .field("balance", b.contactBalance(c.id).str())
        .endObject();
}

void writeDocumentSummary(Writer& w, const Book& b, const Document& d, Date today) {
    w.beginObject()
        .field("id", d.id)
        .field("kind", toString(d.kind))
        .field("number", d.number)
        .field("date", d.date.str())
        .field("due", d.dueDate.str())
        .field("contactId", d.contactId)
        .field("contactName", b.contact(d.contactId).name)
        .field("total", (d.voided ? Money() : d.total()).str())
        .field("balance", b.documentBalance(d.id).str())
        .field("status", documentStatus(b, d, today))
        .field("open", documentIsOpen(b, d))
        .field("voided", d.voided)
        .endObject();
}

void writeLines(Writer& w, const Book& b, const std::vector<DocLine>& lines) {
    w.beginArray();
    for (const auto& l : lines) {
        w.beginObject()
            .field("itemId", l.itemId)
            .field("accountId", l.accountId)
            .field("accountName", b.account(l.accountId).name)
            .field("description", l.description)
            .field("quantity", l.quantity.str())
            .field("rate", l.rate.str())
            .field("amount", l.amount.str())
            .field("taxable", l.taxable)
            .endObject();
    }
    w.endArray();
}

void writeRecurring(Writer& w, const Book& b, const RecurringInvoice& r, Date today, bool full) {
    const std::string status = !r.active ? "Paused" : r.finished() ? "Finished" : r.nextDate() <= today ? "Due" : "Active";
    w.beginObject()
        .field("id", r.id)
        .field("name", r.name)
        .field("contactId", r.contactId)
        .field("contactName", b.contact(r.contactId).name)
        .field("frequency", toString(r.frequency))
        .field("interval", r.interval)
        .field("schedule", r.scheduleText())
        .field("start", r.startDate.str())
        .field("end", r.endDate ? r.endDate->str() : std::string())
        .field("next", r.finished() ? std::string() : r.nextDate().str())
        .field("created", r.occurrencesCreated)
        .field("total", r.total().str())
        .field("active", r.active)
        .field("status", status);
    if (full) {
        w.field("taxRate", r.taxRate.str()).field("memo", r.memo);
        w.key("lines");
        writeLines(w, b, r.lines);
    }
    w.endObject();
}

// ------------------------------------------------------------------ operations

using Handler = std::function<void(const Value& req, Writer& out)>;

void opStatus(const Value&, Writer& w) {
    w.beginObject()
        .field("open", g_session.book.has_value())
        .field("path", g_session.path)
        .field("company", g_session.book ? g_session.book->company.name : std::string())
        .field("encrypted", g_session.key != nullptr)
        .field("version", kVersion)
        .field("cryptoBackend", crypto::backendName())
        .endObject();
}

// Known-answer tests: refuse to encrypt anything on a device whose crypto provider
// disagrees with the published vectors (or with the desktop implementations).
void opSelfTest(const Value&, Writer& w) {
    auto hex = [](const std::string& bytes) {
        static const char* digits = "0123456789abcdef";
        std::string out;
        for (char ch : bytes) {
            const auto c = static_cast<unsigned char>(ch);
            out += digits[c >> 4];
            out += digits[c & 15];
        }
        return out;
    };
    auto unhex = [](std::string_view t) {
        std::string out;
        for (std::size_t i = 0; i + 1 < t.size(); i += 2) out += static_cast<char>(std::stoi(std::string(t.substr(i, 2)), nullptr, 16));
        return out;
    };
    auto check = [](bool ok, const char* what) {
        if (!ok) throw Error(std::string("crypto self-test failed: ") + what);
    };
    unsigned char out[32];
    crypto::pbkdf2Sha256("passwd", reinterpret_cast<const unsigned char*>("salt"), 4, 1, out, 32);
    check(hex(std::string(reinterpret_cast<char*>(out), 32)) == "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc",
          "PBKDF2 (RFC 7914)");
    const std::string unicodePassword = unhex("70c3a4737377c3b6726420f09f949220e697a5e69cac");  // "pässwörd 🔒 日本"
    crypto::pbkdf2Sha256(unicodePassword, reinterpret_cast<const unsigned char*>("saltsaltsaltsalt"), 16, 1000, out, 32);
    check(hex(std::string(reinterpret_cast<char*>(out), 32)) == "77ff06f439e514a0ebadd08748d6fa0887b36a9c9627a6663cc8ff9a0b554509",
          "PBKDF2 with a non-ASCII password");
    const std::string zeroKey(32, '\0');
    const std::string zeroNonce(12, '\0');
    const auto* k = reinterpret_cast<const unsigned char*>(zeroKey.data());
    const auto* n = reinterpret_cast<const unsigned char*>(zeroNonce.data());
    check(hex(crypto::aes256GcmEncrypt(k, n, "", std::string(16, '\0'))) ==
              "cea7403d4d606b6e074ec5d3baf39d18d0d1c8a799996bf0265b98b5d48ab919",
          "AES-GCM (McGrew-Viega 14)");
    const std::string k16 = unhex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308");
    const std::string n16 = unhex("cafebabefacedbaddecaf888");
    const std::string p16 = unhex(
        "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39");
    const std::string a16 = unhex("feedfacedeadbeeffeedfacedeadbeefabaddad2");
    const std::string sealed = crypto::aes256GcmEncrypt(reinterpret_cast<const unsigned char*>(k16.data()),
                                                        reinterpret_cast<const unsigned char*>(n16.data()), a16, p16);
    check(hex(sealed.substr(sealed.size() - 16)) == "76fc6ece0f4e1768cddf8853bb2d551b", "AES-GCM with AAD (McGrew-Viega 16)");
    const auto opened = crypto::aes256GcmDecrypt(reinterpret_cast<const unsigned char*>(k16.data()),
                                                 reinterpret_cast<const unsigned char*>(n16.data()), "tampered", sealed);
    check(!opened, "AES-GCM must reject altered data");
    w.beginObject().field("passed", true).field("backend", crypto::backendName()).endObject();
}

void openAt(const std::string& path, const std::string& password) {
    std::unique_ptr<FileKey> key;
    Book loaded;
    try {
        loaded = Book::load(path, password, &key);
    } catch (const PasswordRequiredError& e) {
        throw ErrorWithCode("password_required", e.what());
    } catch (const WrongPasswordError& e) {
        throw ErrorWithCode("wrong_password", e.what());
    }
    g_session.book = std::move(loaded);
    g_session.path = path;
    g_session.key = std::move(key);
}

void opOpen(const Value& r, Writer& w) {
    openAt(r.str("path"), r.str("password"));
    opStatus(r, w);
}

void opCreate(const Value& r, Writer& w) {
    const std::string path = r.str("path");
    if (fs::exists(fs::u8path(path))) throw Error("a books file with that name already exists");
    std::unique_ptr<FileKey> key;
    if (!r.str("password").empty()) key = FileKey::fromNewPassword(r.str("password"));
    Book b;
    b.company.name = trim(r.str("company", "My Company"));
    if (b.company.name.empty()) throw Error("enter a company name");
    if (r.flag("starterChart", true)) b.createDefaultChart();
    b.save(path, key.get());
    g_session.book = std::move(b);
    g_session.path = path;
    g_session.key = std::move(key);
    opStatus(r, w);
}

void opClose(const Value& r, Writer& w) {
    g_session = Session{};
    opStatus(r, w);
}

void opSetPassword(const Value& r, Writer& w) {
    book();
    if (g_session.key && !g_session.key->matches(r.str("current")))
        throw ErrorWithCode("wrong_password", "the current password is not correct");
    auto key = FileKey::fromNewPassword(r.str("password"));
    g_session.book->save(g_session.path, key.get());
    g_session.key = std::move(key);
    opStatus(r, w);
}

void opRemovePassword(const Value& r, Writer& w) {
    book();
    if (!g_session.key) throw Error("these books are not password protected");
    if (!g_session.key->matches(r.str("current")))
        throw ErrorWithCode("wrong_password", "the current password is not correct");
    g_session.book->save(g_session.path, nullptr);
    g_session.key.reset();
    opStatus(r, w);
}

void opCompany(const Value&, Writer& w) {
    const Book& b = book();
    const Company& c = b.company;
    w.beginObject()
        .field("name", c.name)
        .field("address", c.address)
        .field("email", c.email)
        .field("phone", c.phone)
        .field("invoiceFooter", c.invoiceFooter)
        .field("paperSize", c.paperSize == PaperSize::A4 ? "A4" : "Letter")
        .field("fiscalYearStartMonth", c.fiscalYearStartMonth)
        .field("defaultTermsDays", c.defaultTermsDays)
        .field("closedThrough", c.closedThrough ? c.closedThrough->str() : std::string())
        .field("nextInvoiceNumber", c.nextInvoiceNumber)
        .field("receivablesAccountId", c.receivablesAccountId)
        .field("payablesAccountId", c.payablesAccountId)
        .field("salesTaxAccountId", c.salesTaxAccountId)
        .field("retainedEarningsAccountId", c.retainedEarningsAccountId)
        .field("encrypted", g_session.key != nullptr)
        .field("iterations", g_session.key ? static_cast<int>(g_session.key->iterations()) : 0)
        .field("cryptoBackend", crypto::backendName())
        .endObject();
}

void opSetCompany(const Value& r, Writer& w) {
    mutate([&](Book& b) {
        Company& c = b.company;
        c.name = trim(r.str("name", c.name));
        if (c.name.empty()) throw Error("company name is required");
        c.address = r.str("address", c.address);
        c.email = trim(r.str("email", c.email));
        c.phone = trim(r.str("phone", c.phone));
        c.invoiceFooter = r.str("invoiceFooter", c.invoiceFooter);
        c.paperSize = r.str("paperSize") == "A4" ? PaperSize::A4 : PaperSize::Letter;
        const long long fy = r.integer("fiscalYearStartMonth", c.fiscalYearStartMonth);
        if (fy < 1 || fy > 12) throw Error("the fiscal year start month must be 1-12");
        c.fiscalYearStartMonth = static_cast<int>(fy);
        const long long terms = r.integer("defaultTermsDays", c.defaultTermsDays);
        if (terms < 0 || terms > 3650) throw Error("default terms must be 0-3650 days");
        c.defaultTermsDays = static_cast<int>(terms);
        const long long next = r.integer("nextInvoiceNumber", c.nextInvoiceNumber);
        if (next < 1 || next > 2000000000) throw Error("the next invoice number must be positive");
        c.nextInvoiceNumber = static_cast<int>(next);
        c.closedThrough = optionalDate(r, "closedThrough");
        if (r.id("receivablesAccountId")) b.setSystemAccount(SystemAccount::Receivables, r.id("receivablesAccountId"));
        if (r.id("payablesAccountId")) b.setSystemAccount(SystemAccount::Payables, r.id("payablesAccountId"));
        if (r.id("salesTaxAccountId")) b.setSystemAccount(SystemAccount::SalesTax, r.id("salesTaxAccountId"));
        if (r.id("retainedEarningsAccountId"))
            b.setSystemAccount(SystemAccount::RetainedEarnings, r.id("retainedEarningsAccountId"));
    });
    opCompany(r, w);
}

void opDashboard(const Value&, Writer& w) {
    const Book& b = book();
    const Date today = Date::today();
    const auto balances = b.balances(Period{});

    // Bank accounts: assets that money actually moves through, plus obviously named ones.
    std::set<int> bank;
    for (const auto& t : b.transactions()) {
        if (t.voided) continue;
        for (const auto& s : t.splits) {
            bool moneySide = false;
            switch (t.kind) {
                case TxnKind::CustomerPayment:
                case TxnKind::VendorPayment:
                case TxnKind::Import:
                case TxnKind::Transfer: moneySide = true; break;
                case TxnKind::Expense: moneySide = s.amount < Money(); break;
                case TxnKind::Deposit:
                case TxnKind::SalesReceipt: moneySide = s.amount > Money(); break;
                default: break;
            }
            const Account& a = b.account(s.accountId);
            if (moneySide && a.type == AccountType::Asset && isMoneyAccount(b, a)) bank.insert(a.id);
        }
    }
    for (const auto& a : b.accounts()) {
        const std::string n = toLower(a.name);
        if (a.type == AccountType::Asset && (n.find("checking") != std::string::npos || n.find("saving") != std::string::npos ||
                                             n.find("cash") != std::string::npos || n.find("bank") != std::string::npos))
            bank.insert(a.id);
    }
    Money cash;
    for (int id : bank) cash += lookupBalance(balances, id);

    Money receivable, overdue, payable, billsOverdue;
    int overdueCount = 0;
    for (const auto& d : b.documents()) {
        if (d.voided) continue;
        const Money bal = b.documentBalance(d.id);
        if (bal.isZero()) continue;
        if (d.kind == DocKind::Invoice) {
            receivable += bal;
            if (d.dueDate < today) {
                overdue += bal;
                ++overdueCount;
            }
        } else if (d.kind == DocKind::Bill) {
            payable += bal;
            if (d.dueDate < today) billsOverdue += bal;
        } else if (d.kind == DocKind::CreditMemo) {
            receivable -= bal;
        }
    }
    const auto ytd = b.balances(Period{b.fiscalYearStart(today), today});
    Money income, expenses;
    for (const auto& a : b.accounts()) {
        const Money m = lookupBalance(ytd, a.id);
        if (a.type == AccountType::Income) income -= m;
        if (a.type == AccountType::Expense) expenses += m;
    }

    w.beginObject()
        .field("company", b.company.name)
        .field("today", today.str())
        .field("bank", cash.str())
        .field("bankAccountCount", static_cast<int>(bank.size()))
        .field("receivable", receivable.str())
        .field("overdue", overdue.str())
        .field("overdueCount", overdueCount)
        .field("payable", payable.str())
        .field("billsOverdue", billsOverdue.str())
        .field("income", income.str())
        .field("expenses", expenses.str())
        .field("netIncome", (income - expenses).str())
        .field("recurringDue", b.dueRecurringCount(today));
    w.key("accounts").beginArray();
    for (const auto& a : b.accounts()) {
        if (!isMoneyAccount(b, a) || !a.active) continue;
        const Money bal = naturalSign(a.type, lookupBalance(balances, a.id));
        if (bal.isZero() && !bank.count(a.id)) continue;
        w.beginObject().field("id", a.id).field("label", accountLabel(a)).field("balance", bal.str()).endObject();
    }
    w.endArray();
    w.key("overdueInvoices").beginArray();
    for (const auto& d : b.documents()) {
        if (d.kind != DocKind::Invoice || d.voided || d.dueDate >= today) continue;
        const Money bal = b.documentBalance(d.id);
        if (bal.isZero()) continue;
        w.beginObject()
            .field("id", d.id)
            .field("number", d.number)
            .field("contactName", b.contact(d.contactId).name)
            .field("daysLate", today - d.dueDate)
            .field("balance", bal.str())
            .endObject();
    }
    w.endArray();
    std::vector<const Transaction*> recent;
    for (const auto& t : b.transactions()) {
        if (!t.voided) recent.push_back(&t);
    }
    std::stable_sort(recent.begin(), recent.end(), [](const Transaction* x, const Transaction* y) {
        return x->date != y->date ? x->date > y->date : x->id > y->id;
    });
    if (recent.size() > 15) recent.resize(15);
    w.key("recent").beginArray();
    for (const Transaction* t : recent) {
        w.beginObject()
            .field("id", t->id)
            .field("date", t->date.str())
            .field("kind", toString(t->kind))
            .field("ref", t->ref)
            .field("payee", t->payee.empty() ? t->memo : t->payee)
            .field("amount", t->debitTotal().str())
            .endObject();
    }
    w.endArray().endObject();
}

void opAccounts(const Value& r, Writer& w) {
    const Book& b = book();
    const auto balances = b.balances(Period{});
    const bool includeInactive = r.flag("includeInactive");
    std::vector<const Account*> sorted;
    for (const auto& a : b.accounts()) {
        if (a.active || includeInactive) sorted.push_back(&a);
    }
    std::stable_sort(sorted.begin(), sorted.end(), [](const Account* x, const Account* y) {
        if (x->type != y->type) return static_cast<int>(x->type) < static_cast<int>(y->type);
        if (x->number != y->number) {
            if (x->number.empty()) return false;
            if (y->number.empty()) return true;
            return x->number < y->number;
        }
        return toLower(x->name) < toLower(y->name);
    });
    w.beginArray();
    for (const Account* a : sorted) writeAccount(w, b, *a, balances);
    w.endArray();
}

void opAddAccount(const Value& r, Writer& w) {
    int id = 0;
    mutate([&](Book& b) {
        Account a;
        a.name = r.str("name");
        a.number = r.str("number");
        a.description = r.str("description");
        a.type = enumArg<AccountType>(r, "type", parseAccountType, "account type");
        id = b.addAccount(a);
    });
    w.value(id);
}

void opUpdateAccount(const Value& r, Writer& w) {
    mutate([&](Book& b) {
        Account a = b.account(r.id("id"));
        a.name = r.str("name", a.name);
        a.number = r.str("number", a.number);
        a.description = r.str("description", a.description);
        a.active = r.flag("active", a.active);
        b.updateAccount(a);
    });
    w.value(true);
}

void opContacts(const Value& r, Writer& w) {
    const Book& b = book();
    const ContactKind kind = enumArg<ContactKind>(r, "kind", parseContactKind, "contact kind");
    std::vector<const Contact*> list;
    for (const auto& c : b.contacts()) {
        if (c.kind == kind && (c.active || r.flag("includeInactive"))) list.push_back(&c);
    }
    std::stable_sort(list.begin(), list.end(),
                     [](const Contact* x, const Contact* y) { return toLower(x->name) < toLower(y->name); });
    w.beginArray();
    for (const Contact* c : list) writeContact(w, b, *c);
    w.endArray();
}

void opContact(const Value& r, Writer& w) {
    const Book& b = book();
    const Contact& c = b.contact(r.id("id"));
    const Date today = Date::today();
    w.beginObject().key("contact");
    writeContact(w, b, c);
    w.key("documents").beginArray();
    const auto& docs = b.documents();
    for (auto it = docs.rbegin(); it != docs.rend(); ++it) {
        if (it->contactId == c.id) writeDocumentSummary(w, b, *it, today);
    }
    w.endArray().endObject();
}

void readContactFields(const Value& r, Contact& c) {
    c.name = r.str("name", c.name);
    c.email = r.str("email", c.email);
    c.phone = r.str("phone", c.phone);
    c.address = r.str("address", c.address);
    c.termsDays = static_cast<int>(r.integer("termsDays", c.termsDays));
    c.active = r.flag("active", c.active);
}

void opAddContact(const Value& r, Writer& w) {
    int id = 0;
    mutate([&](Book& b) {
        Contact c;
        c.kind = enumArg<ContactKind>(r, "kind", parseContactKind, "contact kind");
        readContactFields(r, c);
        id = b.addContact(c);
    });
    w.value(id);
}

void opUpdateContact(const Value& r, Writer& w) {
    mutate([&](Book& b) {
        Contact c = b.contact(r.id("id"));
        readContactFields(r, c);
        b.updateContact(c);
    });
    w.value(true);
}

void opItems(const Value& r, Writer& w) {
    const Book& b = book();
    w.beginArray();
    for (const auto& i : b.items()) {
        if (!i.active && !r.flag("includeInactive")) continue;
        w.beginObject()
            .field("id", i.id)
            .field("name", i.name)
            .field("description", i.description)
            .field("price", i.price.str())
            .field("incomeAccountId", i.incomeAccountId)
            .field("expenseAccountId", i.expenseAccountId)
            .field("taxable", i.taxable)
            .field("active", i.active)
            .endObject();
    }
    w.endArray();
}

void opAddItem(const Value& r, Writer& w) {
    int id = 0;
    mutate([&](Book& b) {
        Item i;
        i.name = r.str("name");
        i.description = r.str("description");
        i.price = optionalMoney(r, "price").value_or(Money());
        i.incomeAccountId = r.id("incomeAccountId");
        i.expenseAccountId = r.id("expenseAccountId");
        i.taxable = r.flag("taxable", true);
        id = b.addItem(i);
    });
    w.value(id);
}

void opSetItemActive(const Value& r, Writer& w) {
    mutate([&](Book& b) { b.setItemActive(r.id("id"), r.flag("active")); });
    w.value(true);
}

void opDocuments(const Value& r, Writer& w) {
    const Book& b = book();
    const DocKind kind = enumArg<DocKind>(r, "kind", parseDocKind, "document kind");
    const int contactId = r.id("contactId");
    const bool openOnly = r.flag("openOnly");
    const Date today = Date::today();
    std::vector<const Document*> list;
    for (const auto& d : b.documents()) {
        if (d.kind != kind || (contactId && d.contactId != contactId)) continue;
        if (openOnly && !documentIsOpen(b, d)) continue;
        list.push_back(&d);
    }
    std::stable_sort(list.begin(), list.end(), [](const Document* x, const Document* y) {
        return x->date != y->date ? x->date > y->date : x->id > y->id;
    });
    w.beginArray();
    for (const Document* d : list) writeDocumentSummary(w, b, *d, today);
    w.endArray();
}

void opDocument(const Value& r, Writer& w) {
    const Book& b = book();
    const Document& d = b.document(r.id("id"));
    const Date today = Date::today();
    w.beginObject()
        .field("id", d.id)
        .field("kind", toString(d.kind))
        .field("title", docTitle(d.kind))
        .field("number", d.number)
        .field("contactId", d.contactId)
        .field("contactName", b.contact(d.contactId).name)
        .field("date", d.date.str())
        .field("due", d.dueDate.str())
        .field("hasDueDate", docHasDueDate(d.kind))
        .field("memo", d.memo)
        .field("taxRate", d.taxRate.str())
        .field("subtotal", d.subtotal().str())
        .field("tax", d.tax().str())
        .field("total", d.total().str())
        .field("paid", b.documentPaid(d.id).str())
        .field("credited", b.documentCredited(d.id).str())
        .field("applied", d.applied().str())
        .field("balance", b.documentBalance(d.id).str())
        .field("status", documentStatus(b, d, today))
        .field("voided", d.voided)
        .field("depositAccount", d.depositAccountId ? b.account(d.depositAccountId).name : std::string())
        .field("linkedId", d.linkedDocId)
        .field("linkedNumber", d.linkedDocId ? b.document(d.linkedDocId).number : std::string())
        .field("recurringId", d.recurringId)
        .field("estimateStatus", d.kind == DocKind::Estimate ? toString(b.estimateStatus(d.id)) : "")
        .field("text", renderDocument(b, d.id))
        .field("pdfName", documentPdfFileName(b, d.id));
    w.key("lines");
    writeLines(w, b, d.lines);
    // Money applied to this document (payments and credit memos), and by it (credit memos).
    w.key("payments").beginArray();
    for (const auto& p : b.payments()) {
        if (p.voided) continue;
        for (const auto& a : p.applications) {
            if (a.documentId == d.id)
                w.beginObject().field("paymentId", p.id).field("date", p.date.str()).field("amount", a.amount.str()).endObject();
        }
    }
    w.endArray();
    w.key("credits").beginArray();
    for (const auto& cm : b.documents()) {
        if (cm.kind != DocKind::CreditMemo || cm.voided) continue;
        for (const auto& a : cm.applications) {
            if (a.documentId == d.id)
                w.beginObject().field("creditMemoId", cm.id).field("number", cm.number).field("amount", a.amount.str()).endObject();
        }
    }
    w.endArray();
    w.key("appliedTo").beginArray();
    for (const auto& a : d.applications)
        w.beginObject().field("invoiceId", a.documentId).field("number", b.document(a.documentId).number).field("amount", a.amount.str()).endObject();
    w.endArray().endObject();
}

void opCreateDocument(const Value& r, Writer& w) {
    int id = 0;
    mutate([&](Book& b) {
        Document d;
        d.kind = enumArg<DocKind>(r, "kind", parseDocKind, "document kind");
        d.contactId = r.id("contactId");
        d.date = dateArg(r, "date");
        const Contact& c = b.contact(d.contactId);
        const int terms = c.termsDays >= 0 ? c.termsDays : b.company.defaultTermsDays;
        d.dueDate = optionalDate(r, "due").value_or(d.date.addDays(d.kind == DocKind::Estimate ? 30 : terms));
        d.number = r.str("number");
        d.memo = r.str("memo");
        d.taxRate = decimalArg(r, "taxRate", Decimal());
        d.depositAccountId = r.id("depositAccountId");
        d.lines = linesArg(r);
        id = b.createDocument(d);
    });
    w.value(id);
}

void opVoidDocument(const Value& r, Writer& w) {
    mutate([&](Book& b) { b.voidDocument(r.id("id")); });
    w.value(true);
}

void opConvertEstimate(const Value& r, Writer& w) {
    int id = 0;
    mutate([&](Book& b) { id = b.convertEstimate(r.id("id"), optionalDate(r, "date").value_or(Date::today())); });
    w.value(id);
}

void opSetEstimateStatus(const Value& r, Writer& w) {
    mutate([&](Book& b) {
        b.setEstimateStatus(r.id("id"), enumArg<EstimateStatus>(r, "status", parseEstimateStatus, "estimate status"));
    });
    w.value(true);
}

void opApplyCredit(const Value& r, Writer& w) {
    mutate([&](Book& b) { b.applyCredit(r.id("creditMemoId"), r.id("invoiceId"), moneyArg(r, "amount")); });
    w.value(true);
}

void opUnapplyCredit(const Value& r, Writer& w) {
    mutate([&](Book& b) { b.unapplyCredit(r.id("creditMemoId"), r.id("invoiceId")); });
    w.value(true);
}

void opPayments(const Value& r, Writer& w) {
    const Book& b = book();
    const std::string kindText = r.str("kind");
    std::optional<PaymentKind> kind;
    if (!kindText.empty()) kind = enumArg<PaymentKind>(r, "kind", parsePaymentKind, "payment kind");
    w.beginArray();
    const auto& all = b.payments();
    for (auto it = all.rbegin(); it != all.rend(); ++it) {
        const Payment& p = *it;
        if (kind && p.kind != *kind) continue;
        w.beginObject()
            .field("id", p.id)
            .field("kind", toString(p.kind))
            .field("date", p.date.str())
            .field("contactName", b.contact(p.contactId).name)
            .field("accountName", b.account(p.accountId).name)
            .field("amount", p.amount.str())
            .field("unapplied", p.voided ? std::string("0.00") : p.unapplied().str())
            .field("ref", p.ref)
            .field("voided", p.voided);
        w.key("appliedTo").beginArray();
        for (const auto& a : p.applications) w.value(b.document(a.documentId).number + " (" + a.amount.formatted() + ")");
        w.endArray().endObject();
    }
    w.endArray();
}

void opRecordPayment(const Value& r, Writer& w) {
    int id = 0;
    mutate([&](Book& b) {
        Payment p;
        p.kind = enumArg<PaymentKind>(r, "kind", parsePaymentKind, "payment kind");
        p.contactId = r.id("contactId");
        p.date = dateArg(r, "date");
        p.accountId = r.id("accountId");
        p.amount = moneyArg(r, "amount");
        p.ref = r.str("ref");
        p.memo = r.str("memo");
        for (const auto& a : r.list("applications")) {
            const Money amount = moneyArg(a, "amount");
            if (!amount.isZero()) p.applications.push_back(Application{a.id("documentId"), amount});
        }
        id = b.recordPayment(p);
    });
    w.value(id);
}

void opVoidPayment(const Value& r, Writer& w) {
    mutate([&](Book& b) { b.voidPayment(r.id("id")); });
    w.value(true);
}

void opRegister(const Value& r, Writer& w) {
    const Book& b = book();
    const Account& account = b.account(r.id("accountId"));
    const auto from = optionalDate(r, "from");
    const auto to = optionalDate(r, "to");
    std::vector<const Transaction*> txns;
    for (const auto& t : b.transactions()) txns.push_back(&t);
    std::stable_sort(txns.begin(), txns.end(), [](const Transaction* x, const Transaction* y) {
        return x->date != y->date ? x->date < y->date : x->id < y->id;
    });
    Money running;
    if (from) running = naturalSign(account.type, b.balance(account.id, Period{std::nullopt, from->addDays(-1)}));
    w.beginObject().field("account", accountLabel(account)).field("opening", running.str());
    w.key("rows").beginArray();
    for (const Transaction* t : txns) {
        if (t->voided || (from && t->date < *from) || (to && t->date > *to)) continue;
        for (const auto& s : t->splits) {
            if (s.accountId != account.id) continue;
            const Money amount = naturalSign(account.type, s.amount);
            running += amount;
            std::set<int> others;
            for (const auto& o : t->splits) {
                if (o.accountId != account.id) others.insert(o.accountId);
            }
            const std::string offset =
                others.empty() ? "" : others.size() > 1 ? "-split-" : b.account(*others.begin()).name;
            w.beginObject()
                .field("txnId", t->id)
                .field("date", t->date.str())
                .field("kind", toString(t->kind))
                .field("manual", isManualKind(t->kind))
                .field("ref", t->ref)
                .field("payee", t->payee.empty() ? t->memo : t->payee)
                .field("offset", offset)
                .field("offsetAccountId", others.size() == 1 ? *others.begin() : 0)
                .field("amount", amount.str())
                .field("balance", running.str())
                .field("cleared", s.state == SplitState::Cleared ? "c" : s.state == SplitState::Reconciled ? "R" : "")
                .endObject();
        }
    }
    w.endArray().field("ending", running.str()).field("cleared", naturalSign(account.type, b.clearedBalance(account.id)).str()).endObject();
}

void opTransaction(const Value& r, Writer& w) {
    const Book& b = book();
    const Transaction& t = b.transaction(r.id("id"));
    w.beginObject().field("id", t.id).field("manual", isManualKind(t.kind)).field("voided", t.voided);
    w.key("splits").beginArray();
    for (const auto& s : t.splits) w.beginObject().field("accountId", s.accountId).field("accountName", b.account(s.accountId).name).endObject();
    w.endArray();
    w.key("table");
    writeTable(w, transactionDetail(b, t.id));
    w.endObject();
}

void opPostTransaction(const Value& r, Writer& w) {
    int id = 0;
    mutate([&](Book& b) {
        Transaction t;
        t.kind = enumArg<TxnKind>(r, "kind", parseTxnKind, "transaction kind");
        t.date = dateArg(r, "date");
        t.ref = r.str("ref");
        t.payee = r.str("payee");
        t.memo = r.str("memo");
        for (const auto& s : r.list("splits"))
            t.splits.push_back(Split{s.id("accountId"), moneyArg(s, "amount"), s.str("memo"), SplitState::Uncleared});
        id = b.postTransaction(t);
    });
    w.value(id);
}

void opVoidTransaction(const Value& r, Writer& w) {
    mutate([&](Book& b) { b.voidTransaction(r.id("id")); });
    w.value(true);
}

void opRecategorize(const Value& r, Writer& w) {
    mutate([&](Book& b) { b.recategorize(r.id("id"), r.id("fromAccountId"), r.id("toAccountId")); });
    w.value(true);
}

void opReconcile(const Value& r, Writer& w) {
    const Book& b = book();
    const Account& account = b.account(r.id("accountId"));
    const Date through = optionalDate(r, "through").value_or(Date::today());
    Money reconciled, cleared;
    w.beginObject().key("items").beginArray();
    for (const auto& t : b.transactions()) {
        if (t.voided) continue;
        Money amount;
        bool involved = false, isCleared = false, isReconciled = false;
        for (const auto& s : t.splits) {
            if (s.accountId != account.id) continue;
            involved = true;
            amount += naturalSign(account.type, s.amount);
            isCleared = s.state == SplitState::Cleared;
            isReconciled = s.state == SplitState::Reconciled;
        }
        if (!involved) continue;
        if (isReconciled) {
            reconciled += amount;
            continue;
        }
        if (isCleared) cleared += amount;
        if (t.date > through && !isCleared) continue;
        w.beginObject()
            .field("txnId", t.id)
            .field("date", t.date.str())
            .field("payee", t.payee.empty() ? t.memo : t.payee)
            .field("amount", amount.str())
            .field("cleared", isCleared)
            .endObject();
    }
    w.endArray()
        .field("reconciled", reconciled.str())
        .field("clearedBalance", (reconciled + cleared).str())
        .endObject();
}

void opSetCleared(const Value& r, Writer& w) {
    mutate([&](Book& b) {
        b.setSplitState(r.id("txnId"), r.id("accountId"), r.flag("cleared") ? SplitState::Cleared : SplitState::Uncleared);
    });
    w.value(true);
}

void opFinishReconciliation(const Value& r, Writer& w) {
    int count = 0;
    mutate([&](Book& b) { count = b.finishReconciliation(r.id("accountId")); });
    w.value(count);
}

void opRecurringList(const Value&, Writer& w) {
    const Book& b = book();
    const Date today = Date::today();
    w.beginArray();
    for (const auto& r : b.recurringInvoices()) writeRecurring(w, b, r, today, false);
    w.endArray();
}

void opRecurring(const Value& r, Writer& w) {
    const Book& b = book();
    writeRecurring(w, b, b.recurringInvoice(r.id("id")), Date::today(), true);
}

void readRecurringFields(const Value& r, RecurringInvoice& t) {
    t.name = r.str("name", t.name);
    t.contactId = r.id("contactId");
    t.frequency = enumArg<Frequency>(r, "frequency", parseFrequency, "frequency");
    t.interval = static_cast<int>(r.integer("interval", 1));
    t.startDate = dateArg(r, "start");
    t.endDate = optionalDate(r, "end");
    t.taxRate = decimalArg(r, "taxRate", Decimal());
    t.memo = r.str("memo");
    t.active = r.flag("active", true);
    t.lines = linesArg(r);
}

void opAddRecurring(const Value& r, Writer& w) {
    int id = 0;
    mutate([&](Book& b) {
        RecurringInvoice t;
        readRecurringFields(r, t);
        id = b.addRecurring(t);
    });
    w.value(id);
}

void opUpdateRecurring(const Value& r, Writer& w) {
    mutate([&](Book& b) {
        RecurringInvoice t = b.recurringInvoice(r.id("id"));
        readRecurringFields(r, t);
        b.updateRecurring(t);
    });
    w.value(true);
}

void opSetRecurringActive(const Value& r, Writer& w) {
    mutate([&](Book& b) {
        RecurringInvoice t = b.recurringInvoice(r.id("id"));
        t.active = r.flag("active");
        b.updateRecurring(t);
    });
    w.value(true);
}

void opDeleteRecurring(const Value& r, Writer& w) {
    mutate([&](Book& b) { b.deleteRecurring(r.id("id")); });
    w.value(true);
}

void opRunRecurring(const Value& r, Writer& w) {
    std::vector<int> created;
    mutate([&](Book& b) { created = b.createDueRecurringInvoices(optionalDate(r, "through").value_or(Date::today())); });
    w.beginArray();
    for (int id : created) w.value(id);
    w.endArray();
}

void opReport(const Value& r, Writer& w) {
    const Book& b = book();
    const std::string name = r.str("name");
    const Date today = Date::today();
    const Date to = optionalDate(r, "to").value_or(today);
    const Date from = optionalDate(r, "from").value_or(b.fiscalYearStart(to));
    const Date asOf = optionalDate(r, "asOf").value_or(today);
    Table t;
    if (name == "profit-loss") t = profitAndLoss(b, from, to);
    else if (name == "balance-sheet") t = balanceSheet(b, asOf);
    else if (name == "trial-balance") t = trialBalance(b, asOf);
    else if (name == "ar-aging") t = agingReport(b, DocKind::Invoice, asOf);
    else if (name == "ap-aging") t = agingReport(b, DocKind::Bill, asOf);
    else if (name == "journal") t = journalReport(b, Period{from, to});
    else if (name == "account") t = accountRegister(b, r.id("accountId"), Period{from, to});
    else throw Error("unknown report '" + name + "'");
    w.beginObject().key("table");
    writeTable(w, t);
    w.field("csv", renderCsv(t)).field("text", renderText(t)).endObject();
}

void opPdf(const Value& r, Writer& w) {
    const Book& b = book();
    const std::string bytes = documentPdf(b, r.id("id"));
    const fs::path target = fs::u8path(r.str("path"));
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) throw Error("could not write the PDF");
    w.beginObject().field("path", r.str("path")).field("size", static_cast<long long>(bytes.size())).endObject();
}

CsvImportOptions csvOptions(const Value& r) {
    CsvImportOptions o;
    o.dateColumn = static_cast<int>(r.integer("dateColumn", 1));
    o.descriptionColumn = static_cast<int>(r.integer("descriptionColumn", 2));
    o.amountColumn = static_cast<int>(r.integer("amountColumn", 3));
    o.hasHeader = r.flag("hasHeader", true);
    o.negate = r.flag("negate");
    if (r.id("offsetAccountId")) o.offsetAccountId = r.id("offsetAccountId");
    return o;
}

void opCsvPreview(const Value& r, Writer& w) {
    const auto rows = parseCsv(r.str("text"));
    const CsvImportOptions o = csvOptions(r);
    auto cell = [](const std::vector<std::string>& row, int col) {
        const auto i = static_cast<std::size_t>(col - 1);
        return i < row.size() ? row[i] : std::string();
    };
    w.beginArray();
    int shown = 0;
    for (std::size_t i = o.hasHeader ? 1 : 0; i < rows.size() && shown < 100; ++i) {
        const auto& row = rows[i];
        if (row.empty() || (row.size() == 1 && trim(row[0]).empty())) continue;
        ++shown;
        const auto date = Date::parse(cell(row, o.dateColumn));
        const auto amount = Money::parse(cell(row, o.amountColumn));
        w.beginObject()
            .field("date", cell(row, o.dateColumn))
            .field("dateValid", date.has_value())
            .field("description", cell(row, o.descriptionColumn))
            .field("amount", amount ? (o.negate ? -*amount : *amount).str() : cell(row, o.amountColumn))
            .field("amountValid", amount.has_value())
            .endObject();
    }
    w.endArray();
}

void opImportCsv(const Value& r, Writer& w) {
    CsvImportResult result;
    mutate([&](Book& b) { result = importCsv(b, r.id("accountId"), r.str("text"), csvOptions(r)); });
    w.beginObject().field("imported", result.imported).field("skipped", result.skipped).endObject();
}

const std::map<std::string, Handler>& handlers() {
    static const std::map<std::string, Handler> table = {
        {"status", opStatus},
        {"selfTest", opSelfTest},
        {"open", opOpen},
        {"create", opCreate},
        {"close", opClose},
        {"setPassword", opSetPassword},
        {"removePassword", opRemovePassword},
        {"company", opCompany},
        {"setCompany", opSetCompany},
        {"dashboard", opDashboard},
        {"accounts", opAccounts},
        {"addAccount", opAddAccount},
        {"updateAccount", opUpdateAccount},
        {"contacts", opContacts},
        {"contact", opContact},
        {"addContact", opAddContact},
        {"updateContact", opUpdateContact},
        {"items", opItems},
        {"addItem", opAddItem},
        {"setItemActive", opSetItemActive},
        {"documents", opDocuments},
        {"document", opDocument},
        {"createDocument", opCreateDocument},
        {"voidDocument", opVoidDocument},
        {"convertEstimate", opConvertEstimate},
        {"setEstimateStatus", opSetEstimateStatus},
        {"applyCredit", opApplyCredit},
        {"unapplyCredit", opUnapplyCredit},
        {"payments", opPayments},
        {"recordPayment", opRecordPayment},
        {"voidPayment", opVoidPayment},
        {"register", opRegister},
        {"transaction", opTransaction},
        {"postTransaction", opPostTransaction},
        {"voidTransaction", opVoidTransaction},
        {"recategorize", opRecategorize},
        {"reconcile", opReconcile},
        {"setCleared", opSetCleared},
        {"finishReconciliation", opFinishReconciliation},
        {"recurringList", opRecurringList},
        {"recurring", opRecurring},
        {"addRecurring", opAddRecurring},
        {"updateRecurring", opUpdateRecurring},
        {"setRecurringActive", opSetRecurringActive},
        {"deleteRecurring", opDeleteRecurring},
        {"runRecurring", opRunRecurring},
        {"report", opReport},
        {"pdf", opPdf},
        {"csvPreview", opCsvPreview},
        {"importCsv", opImportCsv},
    };
    return table;
}

std::string errorReply(const std::string& code, const std::string& message) {
    Writer w;
    w.beginObject().field("ok", false).field("code", code).field("error", message).endObject();
    return w.str();
}

}  // namespace

// Handles one request. Exposed for the desktop test harness as well as JNI.
std::string openbooksHandle(const std::string& request) {
    std::lock_guard<std::mutex> lock(g_mutex);
    try {
        const Value req = json::parse(request);
        const std::string op = req.str("op");
        const auto& table = handlers();
        const auto it = table.find(op);
        if (it == table.end()) return errorReply("error", "unknown operation '" + op + "'");
        Writer w;
        w.beginObject().field("ok", true).key("data");
        it->second(req, w);
        w.endObject();
        return w.str();
    } catch (const ErrorWithCode& e) {
        return errorReply(e.code(), e.what());
    } catch (const std::exception& e) {
        return errorReply("error", e.what());
    }
}

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    if (!jx::initialize(vm, env)) return JNI_ERR;
    return JNI_VERSION_1_6;
}

JNIEXPORT jbyteArray JNICALL Java_org_openbooks_core_NativeBooks_callBytes(JNIEnv* env, jclass, jbyteArray request) {
    const jsize n = env->GetArrayLength(request);
    std::string text(static_cast<std::size_t>(n), '\0');
    if (n) env->GetByteArrayRegion(request, 0, n, reinterpret_cast<jbyte*>(&text[0]));
    const std::string reply = openbooksHandle(text);
    ob::crypto::wipe(text);  // requests can carry passwords
    jbyteArray out = env->NewByteArray(static_cast<jsize>(reply.size()));
    if (out && !reply.empty())
        env->SetByteArrayRegion(out, 0, static_cast<jsize>(reply.size()), reinterpret_cast<const jbyte*>(reply.data()));
    return out;
}

}  // extern "C"

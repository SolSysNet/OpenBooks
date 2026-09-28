// Self-contained test runner (no external framework needed).

#include "openbooks/book.hpp"
#include "openbooks/cli.hpp"
#include "openbooks/crypto.hpp"
#include "openbooks/invoice_pdf.hpp"
#include "openbooks/pdf.hpp"
#include "openbooks/reports.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using namespace ob;

namespace {

int g_checks = 0;
int g_failures = 0;

struct TestCase {
    const char* name;
    void (*fn)();
};

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

#define TEST(name)                                   \
    void name();                                     \
    const Registrar registrar_##name(#name, name);   \
    void name()

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        ++g_checks;                                                                        \
        if (!(cond)) {                                                                     \
            ++g_failures;                                                                  \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n";     \
        }                                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                                                     \
    do {                                                                                                   \
        ++g_checks;                                                                                        \
        const auto va = (a);                                                                               \
        const auto vb = (b);                                                                               \
        if (!(va == vb)) {                                                                                 \
            ++g_failures;                                                                                  \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK_EQ failed: " #a " == " #b "  (" << va    \
                      << " vs " << vb << ")\n";                                                            \
        }                                                                                                  \
    } while (0)

#define CHECK_THROWS(expr)                                                                         \
    do {                                                                                           \
        ++g_checks;                                                                                \
        bool threw = false;                                                                        \
        try {                                                                                      \
            expr;                                                                                  \
        } catch (const ob::Error&) {                                                               \
            threw = true;                                                                          \
        }                                                                                          \
        if (!threw) {                                                                              \
            ++g_failures;                                                                          \
            std::cerr << __FILE__ << ":" << __LINE__ << ": expected ob::Error from: " #expr "\n";  \
        }                                                                                          \
    } while (0)

Money M(const char* s) { return *Money::parse(s); }
Date D(const char* s) { return *Date::parse(s); }
Decimal Q(const char* s) { return *Decimal::parse(s); }

Money sumOfAllBalances(const Book& b) {
    Money total;
    for (const auto& [id, m] : b.balances(Period{})) total += m;
    return total;
}

struct Fixture {
    Book b;
    int checking, sales, service, rent, ar, ap, tax;
    int acme, landlord;

    Fixture() {
        b.createDefaultChart();
        checking = b.findAccount("Checking").id;
        sales = b.findAccount("Sales").id;
        service = b.findAccount("Service Revenue").id;
        rent = b.findAccount("Rent").id;
        ar = b.company.receivablesAccountId;
        ap = b.company.payablesAccountId;
        tax = b.company.salesTaxAccountId;
        Contact c;
        c.kind = ContactKind::Customer;
        c.name = "Acme Corp";
        acme = b.addContact(c);
        Contact v;
        v.kind = ContactKind::Vendor;
        v.name = "Landlord LLC";
        landlord = b.addContact(v);
    }

    int invoice(const char* date, Money rate, Decimal qty = Decimal::fromInt(1), Decimal taxRate = Decimal()) {
        Document d;
        d.kind = DocKind::Invoice;
        d.contactId = acme;
        d.date = D(date);
        d.dueDate = d.date.addDays(30);
        d.taxRate = taxRate;
        DocLine l;
        l.accountId = service;
        l.description = "Consulting";
        l.quantity = qty;
        l.rate = rate;
        d.lines.push_back(l);
        return b.createDocument(d);
    }

    int bill(const char* date, Money amount) {
        Document d;
        d.kind = DocKind::Bill;
        d.contactId = landlord;
        d.date = D(date);
        d.dueDate = d.date.addDays(15);
        DocLine l;
        l.accountId = rent;
        l.rate = amount;
        d.lines.push_back(l);
        return b.createDocument(d);
    }

    int receive(const char* date, Money amount, std::vector<Application> apps = {}) {
        Payment p;
        p.kind = PaymentKind::Received;
        p.contactId = acme;
        p.date = D(date);
        p.accountId = checking;
        p.amount = amount;
        p.applications = std::move(apps);
        return b.recordPayment(p);
    }

    int journal(const char* date, int debit, int credit, Money amount) {
        Transaction t;
        t.date = D(date);
        t.splits = {Split{debit, amount, "", SplitState::Uncleared}, Split{credit, -amount, "", SplitState::Uncleared}};
        return b.postTransaction(t);
    }
};

// ------------------------------------------------------------------ money

TEST(money_parsing) {
    CHECK_EQ(M("1234.56").cents(), 123456);
    CHECK_EQ(M("1,234.5").cents(), 123450);
    CHECK_EQ(M("$12").cents(), 1200);
    CHECK_EQ(M("-0.07").cents(), -7);
    CHECK_EQ(M("(45.00)").cents(), -4500);
    CHECK_EQ(M("1.500").cents(), 150);
    CHECK(!Money::parse("1.234"));
    CHECK(!Money::parse("abc"));
    CHECK(!Money::parse(""));
    CHECK(!Money::parse("1.2.3"));
    CHECK(!Money::parse("$"));
}

TEST(money_formatting) {
    CHECK_EQ(Money::fromCents(-123456789).formatted(), std::string("-1,234,567.89"));
    CHECK_EQ(Money::fromCents(5).str(), std::string("0.05"));
    CHECK_EQ(Money::fromCents(-5).str(), std::string("-0.05"));
    CHECK_EQ(Money::fromCents(100000).formatted(), std::string("1,000.00"));
    CHECK_EQ(Money::fromCents(99999).formatted(), std::string("999.99"));
    CHECK_EQ(Q("8.2500").str(), std::string("8.25"));
    CHECK_EQ(Q("3").str(), std::string("3"));
    CHECK_EQ(Q("0").str(), std::string("0"));
}

TEST(money_rounding) {
    CHECK_EQ(multiply(M("3.33"), Q("1.5")), M("5.00"));        // 4.995 rounds up
    CHECK_EQ(multiply(M("-3.33"), Q("1.5")), M("-5.00"));      // symmetric
    CHECK_EQ(multiply(M("19.99"), Q("3")), M("59.97"));
    CHECK_EQ(percentOf(M("100.00"), Q("8.25")), M("8.25"));
    CHECK_EQ(percentOf(M("10.01"), Q("8.25")), M("0.83"));     // 0.825825
    CHECK_EQ(percentOf(M("10.00"), Q("0.05")), M("0.01"));     // 0.005 rounds half up
}

// ------------------------------------------------------------------ dates

TEST(date_parsing_and_math) {
    CHECK(Date::parse("2024-02-29").has_value());
    CHECK(!Date::parse("2023-02-29").has_value());
    CHECK(!Date::parse("2023-13-01").has_value());
    CHECK(!Date::parse("yesterday").has_value());
    CHECK_EQ(D("03/15/2025").str(), std::string("2025-03-15"));
    CHECK_EQ(D("2025-01-31").addDays(1).str(), std::string("2025-02-01"));
    CHECK_EQ(D("2024-12-31").addDays(1).str(), std::string("2025-01-01"));
    CHECK_EQ(D("2025-03-01") - D("2025-02-01"), 28);
    CHECK_EQ(D("1970-01-01").serial(), 0);
    // Round trip every day across four centuries of leap-year rules.
    bool ok = true;
    for (int day = -150000; day < 150000; day += 7) {
        const Date d = Date().addDays(day);
        const auto again = Date::parse(d.str());
        if (!again || *again != d) ok = false;
    }
    CHECK(ok);
}

// ----------------------------------------------------------------- ledger

TEST(journal_must_balance) {
    Fixture f;
    Transaction t;
    t.date = D("2025-01-05");
    t.splits = {Split{f.checking, M("100"), "", SplitState::Uncleared}, Split{f.sales, M("-90"), "", SplitState::Uncleared}};
    CHECK_THROWS(f.b.postTransaction(t));
    t.splits = {Split{f.checking, M("100"), "", SplitState::Uncleared}};
    CHECK_THROWS(f.b.postTransaction(t));
    CHECK(f.b.transactions().empty());
}

TEST(manual_entries_cannot_touch_ar_ap) {
    Fixture f;
    CHECK_THROWS(f.journal("2025-01-05", f.ar, f.sales, M("50")));
    CHECK_THROWS(f.journal("2025-01-05", f.rent, f.ap, M("50")));
    Transaction t;
    t.kind = TxnKind::Invoice;
    t.date = D("2025-01-05");
    t.splits = {Split{f.checking, M("1"), "", SplitState::Uncleared}, Split{f.sales, M("-1"), "", SplitState::Uncleared}};
    CHECK_THROWS(f.b.postTransaction(t));
}

TEST(invoice_posts_to_ar_income_and_tax) {
    Fixture f;
    const int id = f.invoice("2025-02-01", M("150.00"), Q("10"), Q("8.25"));
    const Document& d = f.b.document(id);
    CHECK_EQ(d.number, std::string("1001"));
    CHECK_EQ(d.subtotal(), M("1500.00"));
    CHECK_EQ(d.tax(), M("123.75"));
    CHECK_EQ(d.total(), M("1623.75"));
    CHECK_EQ(f.b.balance(f.ar), M("1623.75"));
    CHECK_EQ(f.b.balance(f.service), M("-1500.00"));
    CHECK_EQ(f.b.balance(f.tax), M("-123.75"));
    CHECK_EQ(f.b.company.nextInvoiceNumber, 1002);
    CHECK_EQ(f.b.document(f.invoice("2025-02-02", M("1"))).number, std::string("1002"));
    CHECK(sumOfAllBalances(f.b).isZero());
}

TEST(invoice_validation) {
    Fixture f;
    Document d;
    d.kind = DocKind::Invoice;
    d.contactId = f.landlord;  // a vendor
    d.date = D("2025-01-01");
    d.dueDate = d.date;
    DocLine l;
    l.accountId = f.service;
    l.rate = M("10");
    d.lines.push_back(l);
    CHECK_THROWS(f.b.createDocument(d));
    d.contactId = f.acme;
    d.dueDate = D("2024-12-01");
    CHECK_THROWS(f.b.createDocument(d));  // due before date
    d.dueDate = d.date;
    d.lines[0].rate = M("0");
    CHECK_THROWS(f.b.createDocument(d));  // zero total
    d.lines[0].rate = M("10");
    d.number = "A-1";
    f.b.createDocument(d);
    CHECK_THROWS(f.b.createDocument(d));  // duplicate number
    CHECK_EQ(f.b.company.nextInvoiceNumber, 1001);  // explicit non-numeric numbers don't consume the sequence
}

TEST(payments_apply_oldest_first_and_track_balances) {
    Fixture f;
    const int inv1 = f.invoice("2025-01-01", M("100"));
    const int inv2 = f.invoice("2025-02-01", M("200"));
    f.receive("2025-02-10", M("150"));
    CHECK_EQ(f.b.documentBalance(inv1), M("0"));
    CHECK_EQ(f.b.documentBalance(inv2), M("150"));
    CHECK_EQ(f.b.balance(f.ar), M("150"));
    CHECK_EQ(f.b.balance(f.checking), M("150"));
    CHECK_EQ(f.b.contactBalance(f.acme), M("150"));

    // Overpayment leaves an unapplied credit that nets against the customer balance.
    const int pay = f.receive("2025-02-20", M("200"));
    CHECK_EQ(f.b.payment(pay).unapplied(), M("50"));
    CHECK_EQ(f.b.contactBalance(f.acme), M("-50"));
    CHECK_EQ(f.b.balance(f.ar), M("-50"));
    CHECK(sumOfAllBalances(f.b).isZero());
}

TEST(explicit_payment_applications_are_validated) {
    Fixture f;
    const int inv = f.invoice("2025-01-01", M("100"));
    CHECK_THROWS(f.receive("2025-01-05", M("100"), {Application{inv, M("150")}}));  // more than owed
    CHECK_THROWS(f.receive("2025-01-05", M("50"), {Application{inv, M("60")}}));    // more than paid
    CHECK_THROWS(f.receive("2025-01-05", M("0")));
    const int bill = f.bill("2025-01-01", M("40"));
    CHECK_THROWS(f.receive("2025-01-05", M("40"), {Application{bill, M("40")}}));  // wrong kind
    f.receive("2025-01-05", M("60"), {Application{inv, M("60")}});
    CHECK_EQ(f.b.documentBalance(inv), M("40"));
    CHECK(f.b.payments().size() == 1);
}

TEST(bills_and_bill_payments) {
    Fixture f;
    const int bill = f.bill("2025-03-01", M("1200"));
    CHECK_EQ(f.b.balance(f.ap), M("-1200"));
    CHECK_EQ(f.b.balance(f.rent), M("1200"));
    Payment p;
    p.kind = PaymentKind::Paid;
    p.contactId = f.landlord;
    p.date = D("2025-03-05");
    p.accountId = f.checking;
    p.amount = M("1200");
    f.b.recordPayment(p);
    CHECK_EQ(f.b.documentBalance(bill), M("0"));
    CHECK_EQ(f.b.balance(f.ap), M("0"));
    CHECK_EQ(f.b.balance(f.checking), M("-1200"));

    Document taxed = f.b.document(bill);
    taxed.number.clear();
    taxed.taxRate = Q("5");
    CHECK_THROWS(f.b.createDocument(taxed));  // bills take no tax rate
}

TEST(voiding_rules) {
    Fixture f;
    const int inv = f.invoice("2025-01-01", M("100"));
    const int pay = f.receive("2025-01-10", M("100"));
    CHECK_THROWS(f.b.voidDocument(inv));                         // payment applied
    CHECK_THROWS(f.b.voidTransaction(f.b.document(inv).txnId));  // owned by the invoice
    f.b.voidPayment(pay);
    CHECK_EQ(f.b.documentBalance(inv), M("100"));
    f.b.voidDocument(inv);
    CHECK_THROWS(f.b.voidDocument(inv));
    CHECK_EQ(f.b.balance(f.ar), M("0"));
    CHECK_EQ(f.b.balance(f.checking), M("0"));
    CHECK_EQ(f.b.documentBalance(inv), M("0"));
}

TEST(closed_period_is_locked) {
    Fixture f;
    const int early = f.journal("2025-01-15", f.checking, f.sales, M("10"));
    f.b.company.closedThrough = D("2025-01-31");
    CHECK_THROWS(f.journal("2025-01-20", f.checking, f.sales, M("10")));
    CHECK_THROWS(f.b.voidTransaction(early));
    CHECK_THROWS(f.invoice("2025-01-31", M("10")));
    f.journal("2025-02-01", f.checking, f.sales, M("10"));
    CHECK_EQ(f.b.balance(f.checking), M("20"));
}

TEST(reconciliation) {
    Fixture f;
    const int t1 = f.journal("2025-01-02", f.checking, f.sales, M("500"));
    const int t2 = f.journal("2025-01-03", f.rent, f.checking, M("200"));
    f.journal("2025-01-04", f.rent, f.checking, M("50"));
    f.b.setSplitState(t1, f.checking, SplitState::Cleared);
    f.b.setSplitState(t2, f.checking, SplitState::Cleared);
    CHECK_EQ(f.b.clearedBalance(f.checking), M("300"));
    CHECK_EQ(f.b.finishReconciliation(f.checking), 2);
    CHECK_THROWS(f.b.voidTransaction(t1));
    CHECK_THROWS(f.b.setSplitState(t1, f.checking, SplitState::Uncleared));
    CHECK_THROWS(f.b.setSplitState(t1, f.rent, SplitState::Cleared));  // no line in that account
}

TEST(recategorize_moves_lines) {
    Fixture f;
    const int office = f.b.findAccount("Office Supplies").id;
    const int uncategorized = f.b.findAccount("Uncategorized Expense").id;
    const int t = f.journal("2025-01-02", uncategorized, f.checking, M("42"));
    f.b.recategorize(t, uncategorized, office);
    CHECK_EQ(f.b.balance(office), M("42"));
    CHECK_EQ(f.b.balance(uncategorized), M("0"));
    CHECK_THROWS(f.b.recategorize(t, uncategorized, office));
    CHECK_THROWS(f.b.recategorize(t, office, f.ar));
}

TEST(account_lookup_and_management) {
    Fixture f;
    CHECK_EQ(f.b.findAccount("1000").id, f.checking);
    CHECK_EQ(f.b.findAccount("checking").id, f.checking);
    CHECK_EQ(f.b.findAccount("Check").id, f.checking);  // unique prefix
    CHECK_EQ(f.b.findAccount("#" + std::to_string(f.rent)).id, f.rent);
    CHECK_THROWS(f.b.findAccount("S"));  // ambiguous: Savings, Sales, ...
    CHECK_THROWS(f.b.findAccount("Nope"));
    CHECK_THROWS(f.b.addAccount(Account{0, "1000", "Another", AccountType::Asset, "", true}));
    CHECK_THROWS(f.b.addAccount(Account{0, "", "CHECKING", AccountType::Asset, "", true}));

    f.journal("2025-01-02", f.checking, f.sales, M("5"));
    Account a = f.b.account(f.checking);
    a.active = false;
    CHECK_THROWS(f.b.updateAccount(a));  // non-zero balance
    a = f.b.account(f.ar);
    a.active = false;
    CHECK_THROWS(f.b.updateAccount(a));  // system account
    a = f.b.account(f.checking);
    a.type = AccountType::Expense;
    CHECK_THROWS(f.b.updateAccount(a));  // type change on a used account
}

// ---------------------------------------------------------------- reports

TEST(balance_sheet_balances_across_fiscal_years) {
    Fixture f;
    const int equity = f.b.findAccount("Owner's Equity").id;
    f.journal("2024-01-01", f.checking, equity, M("10000"));
    f.invoice("2024-06-01", M("3000"));
    f.receive("2024-06-15", M("3000"));
    f.bill("2024-07-01", M("1000"));
    f.invoice("2025-02-01", M("500"), Decimal::fromInt(1), Q("10"));
    f.journal("2025-02-03", f.rent, f.checking, M("200"));

    const Table bs = balanceSheet(f.b, D("2025-03-31"));
    const Row* assets = bs.find("Total Assets");
    const Row* both = bs.find("Total Liabilities & Equity");
    CHECK(assets && both);
    if (assets && both) CHECK_EQ(assets->cells[1], both->cells[1]);
    CHECK_EQ(assets ? assets->cells[1] : std::string(), std::string("13,350.00"));
    // 2024 profit (3000 - 1000) rolled into retained earnings; 2025 net income = 500 - 200.
    const Row* re = bs.find("3900 Retained Earnings");
    CHECK(re && re->cells[1] == "2,000.00");
    const Row* ni = bs.find("Net Income");
    CHECK(ni && ni->cells[1] == "300.00");

    const Table tb = trialBalance(f.b, D("2025-03-31"));
    const Row* total = tb.find("Total");
    CHECK(total && total->cells[1] == total->cells[2]);

    const Table pl = profitAndLoss(f.b, D("2024-01-01"), D("2024-12-31"));
    const Row* net = pl.find("Net Income");
    CHECK(net && net->cells[1] == "2,000.00");
}

TEST(aging_buckets) {
    Fixture f;
    f.invoice("2025-01-01", M("100"));  // due 01-31
    f.invoice("2025-03-01", M("200"));  // due 03-31
    f.invoice("2025-04-10", M("300"));  // due 05-10
    const Table aging = agingReport(f.b, DocKind::Invoice, D("2025-04-15"));
    const Row* row = aging.find("Acme Corp");
    CHECK(row != nullptr);
    if (row) {
        CHECK_EQ(row->cells[1], std::string("300.00"));  // current
        CHECK_EQ(row->cells[2], std::string("200.00"));  // 1-30 (15 days late)
        CHECK_EQ(row->cells[4], std::string("100.00"));  // 61-90 (74 days late)
        CHECK_EQ(row->cells[6], std::string("600.00"));
    }
}

TEST(text_and_csv_rendering) {
    Table t;
    t.headers = {"Name", "Amount"};
    t.numeric = {false, true};
    t.add({"Coffee, beans", "1.00"});
    t.add({"Total", "1,001.00"}, RowStyle::Total);
    const std::string csv = renderCsv(t);
    CHECK(csv.find("\"Coffee, beans\",1.00") != std::string::npos);
    CHECK(csv.find("Total,1001.00") != std::string::npos);
    const std::string text = renderText(t);
    CHECK(text.find("Coffee, beans      1.00") != std::string::npos);
}

// ---------------------------------------------------------------- storage

TEST(storage_round_trip) {
    Fixture f;
    f.b.company.name = "Tab\tand\nnewline \\ Co";
    f.b.company.closedThrough = D("2024-12-31");
    const int inv = f.invoice("2025-01-01", M("100"), Q("2.5"), Q("7.5"));
    f.receive("2025-01-02", M("50"));
    f.bill("2025-01-03", M("75"));
    const int t = f.journal("2025-01-04", f.rent, f.checking, M("10"));
    f.b.setSplitState(t, f.checking, SplitState::Cleared);

    std::stringstream first;
    f.b.write(first);
    std::stringstream in(first.str());
    const Book loaded = Book::read(in);
    std::stringstream second;
    loaded.write(second);
    CHECK_EQ(first.str(), second.str());
    CHECK_EQ(loaded.company.name, f.b.company.name);
    CHECK_EQ(loaded.documentBalance(inv), f.b.documentBalance(inv));
    CHECK_EQ(loaded.clearedBalance(f.checking), M("-10"));

    // New ids continue after the loaded ones.
    Book copy = loaded;
    Contact c;
    c.kind = ContactKind::Customer;
    c.name = "Newco";
    CHECK_EQ(copy.addContact(c), 3);
}

TEST(storage_rejects_corruption) {
    std::stringstream notOurs("hello\n");
    CHECK_THROWS(Book::read(notOurs));
    std::stringstream unbalanced(
        "OPENBOOKS\t1\nACCOUNT\t1\t\tA\tAsset\t\t1\nACCOUNT\t2\t\tB\tIncome\t\t1\n"
        "TXN\t1\t2025-01-01\tJournal\t\t\t\t0\nSPLIT\t1\t1\t10.00\t\t\nSPLIT\t1\t2\t-9.00\t\t\n");
    CHECK_THROWS(Book::read(unbalanced));
    std::stringstream unknown("OPENBOOKS\t1\nWIDGET\t1\n");
    CHECK_THROWS(Book::read(unknown));
}

// -------------------------------------------------------------------- PDF

// Checks the file skeleton: header, trailer, and that every xref offset points at its object.
bool pdfStructureValid(const std::string& pdf, std::string* why) {
    auto fail = [&](const std::string& m) {
        if (why) *why = m;
        return false;
    };
    if (pdf.rfind("%PDF-1.4\n", 0) != 0) return fail("bad header");
    if (pdf.size() < 6 || pdf.compare(pdf.size() - 6, 6, "%%EOF\n") != 0) return fail("bad trailer");
    const auto sx = pdf.rfind("startxref\n");
    if (sx == std::string::npos) return fail("no startxref");
    const std::size_t xref = std::stoul(pdf.substr(sx + 10));
    if (pdf.compare(xref, 5, "xref\n") != 0) return fail("startxref does not point at xref");
    std::istringstream in(pdf.substr(xref + 5));
    std::size_t first = 0, count = 0;
    in >> first >> count;
    std::string line;
    std::getline(in, line);
    std::getline(in, line);  // free entry 0
    for (std::size_t n = 1; n < count; ++n) {
        std::getline(in, line);
        if (line.size() != 19) return fail("xref entry has wrong length");
        const std::size_t offset = std::stoul(line.substr(0, 10));
        const std::string expect = std::to_string(n) + " 0 obj\n";
        if (pdf.compare(offset, expect.size(), expect) != 0) return fail("xref offset wrong for object " + std::to_string(n));
    }
    // Every stream's /Length must match its data.
    std::size_t pos = 0;
    while ((pos = pdf.find("/Length ", pos)) != std::string::npos) {
        const std::size_t length = std::stoul(pdf.substr(pos + 8));
        const std::size_t data = pdf.find("stream\n", pos) + 7;
        if (pdf.compare(data + length, 10, "\nendstream") != 0) return fail("stream length mismatch");
        pos = data + length;
    }
    return true;
}

bool noActiveContent(const std::string& pdf) {
    for (const char* key : {"/JavaScript", "/JS", "/URI", "/Launch", "/EmbeddedFile", "/OpenAction", "/AA", "/AcroForm",
                            "/SubmitForm", "/GoToR", "/FontFile"}) {
        if (pdf.find(key) != std::string::npos) return false;
    }
    return true;
}

TEST(pdf_text_encoding_and_escaping) {
    CHECK_EQ(pdf::escapeString("a(b)c\\"), std::string("(a\\(b\\)c\\\\)"));
    CHECK_EQ(pdf::escapeString("\xE9\n"), std::string("(\\351\\012)"));
    CHECK_EQ(pdf::toWinAnsi("caf\xC3\xA9 \xE2\x82\xAC" "5 \xE2\x80\x94 ok"), std::string("caf\xE9 \x80" "5 \x97 ok"));
    CHECK_EQ(pdf::toWinAnsi("\xFF"), std::string("?"));
    CHECK_EQ(pdf::toWinAnsi("\xC3"), std::string("?"));                  // truncated sequence
    CHECK_EQ(pdf::toWinAnsi("\xF0\x9F\x98\x80!"), std::string("?!"));    // emoji is not in WinAnsi
    CHECK_EQ(pdf::toWinAnsi("a\tb"), std::string("a b"));
    CHECK(std::abs(pdf::textWidth("Hello", pdf::Font::Regular, 10) - 22.78) < 1e-9);
    CHECK(std::abs(pdf::textWidth("Hello", pdf::Font::Bold, 10) - 24.45) < 1e-9);  // H722 e556 l278 l278 o611
}

TEST(pdf_wrapping) {
    const std::string text = "The quick brown fox jumps over the lazy dog and keeps running across the field";
    const auto lines = pdf::wrapText(text, pdf::Font::Regular, 10, 120);
    CHECK(lines.size() > 1);
    bool fits = true;
    for (const auto& l : lines) fits = fits && pdf::textWidth(l, pdf::Font::Regular, 10) <= 120;
    CHECK(fits);
    const auto broken = pdf::wrapText(std::string(80, 'W'), pdf::Font::Regular, 10, 100);
    CHECK(broken.size() > 1);
    CHECK_EQ(pdf::wrapText("", pdf::Font::Regular, 10, 100).size(), std::size_t(1));
    CHECK_EQ(pdf::wrapText("one\ntwo", pdf::Font::Regular, 10, 500).size(), std::size_t(2));
}

TEST(pdf_document_structure) {
    pdf::Document doc(pdf::kA4);
    doc.setTitle("Test (1)");
    doc.addPage().text(50, 700, "Hello", pdf::Font::Bold, 12);
    doc.addPage().line(0, 0, 100, 100);
    const std::string out = doc.build();
    std::string why;
    CHECK(pdfStructureValid(out, &why));
    if (!why.empty()) std::cerr << "  " << why << '\n';
    CHECK(out.find("/Count 2") != std::string::npos);
    CHECK(out.find("/MediaBox [0 0 595.28 841.89]") != std::string::npos);
    CHECK(out.find("/Title (Test \\(1\\))") != std::string::npos);
    CHECK(noActiveContent(out));
    CHECK_EQ(out, doc.build());  // deterministic
}

TEST(invoice_pdf_rendering) {
    Fixture f;
    f.b.company.name = "Blue Door (Design)";
    f.b.company.invoiceFooter = "Pay by bank transfer.";
    Contact evil = f.b.contact(f.acme);
    evil.name = "Acme ) Tj /F2 99 Tf (Corp";  // attempted content-stream injection
    f.b.updateContact(evil);

    Document d;
    d.kind = DocKind::Invoice;
    d.contactId = f.acme;
    d.date = D("2026-01-05");
    d.dueDate = D("2026-02-04");
    d.memo = "Thanks!";
    for (int i = 0; i < 45; ++i) {
        DocLine l;
        l.accountId = f.service;
        l.description = "Line " + std::to_string(i) + " with a fairly long description that wraps onto two lines";
        l.rate = M("10");
        d.lines.push_back(l);
    }
    const int id = f.b.createDocument(d);
    const std::string out = documentPdf(f.b, id);
    std::string why;
    CHECK(pdfStructureValid(out, &why));
    if (!why.empty()) std::cerr << "  " << why << '\n';
    CHECK(noActiveContent(out));
    CHECK(out.find("Tf (Corp") == std::string::npos);        // the injection stayed inside a string
    CHECK(out.find("Acme \\) Tj /F2 99 Tf \\(Corp") != std::string::npos);
    CHECK(out.find("/Count 1 ") == std::string::npos);        // multi-page
    CHECK(out.find("(Page 1 of ") != std::string::npos);
    CHECK(out.find("(450.00)") != std::string::npos);         // total
    CHECK(out.find("(PAID)") == std::string::npos);

    f.receive("2026-01-10", M("450"));
    CHECK(documentPdf(f.b, id).find("(PAID)") != std::string::npos);

    const int small = f.invoice("2026-01-06", M("5"));
    f.b.voidDocument(small);
    const std::string voided = documentPdf(f.b, small);
    CHECK(voided.find("(VOID)") != std::string::npos);
    CHECK(pdfStructureValid(voided, nullptr));
}

TEST(invoice_pdf_file_name_is_safe) {
    Fixture f;
    Contact c = f.b.contact(f.acme);
    c.name = "..\\..\\Windows/System32:evil*?";
    f.b.updateContact(c);
    Document d;
    d.kind = DocKind::Invoice;
    d.contactId = f.acme;
    d.date = D("2026-01-05");
    d.dueDate = d.date;
    d.number = "../../x";
    DocLine l;
    l.accountId = f.service;
    l.rate = M("1");
    d.lines.push_back(l);
    const std::string name = documentPdfFileName(f.b, f.b.createDocument(d));
    CHECK(name.find('/') == std::string::npos);
    CHECK(name.find('\\') == std::string::npos);
    CHECK(name.find(':') == std::string::npos);
    CHECK(name.find('*') == std::string::npos);
    CHECK(name.front() != '.');
    CHECK(name.size() > 4 && name.compare(name.size() - 4, 4, ".pdf") == 0);
}

TEST(company_fields_round_trip_and_old_files_load) {
    Book b;
    b.company.email = "a@b.example";
    b.company.phone = "555";
    b.company.invoiceFooter = "Line one\nLine two";
    b.company.paperSize = PaperSize::A4;
    std::stringstream out;
    b.write(out);
    std::stringstream in(out.str());
    const Book loaded = Book::read(in);
    CHECK_EQ(loaded.company.invoiceFooter, b.company.invoiceFooter);
    CHECK(loaded.company.paperSize == PaperSize::A4);
    CHECK_EQ(loaded.company.email, std::string("a@b.example"));

    // A 0.2 file has only the first eleven COMPANY fields.
    std::stringstream old("OPENBOOKS\t1\nCOMPANY\tOld Co\t\t1\t30\t\t1001\t0\t0\t0\t0\n");
    const Book legacy = Book::read(old);
    CHECK_EQ(legacy.company.name, std::string("Old Co"));
    CHECK(legacy.company.email.empty());
    CHECK(legacy.company.paperSize == PaperSize::Letter);
}

// ------------------------------------------------------------- encryption

std::string hex(std::string_view bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (char ch : bytes) {
        const auto c = static_cast<unsigned char>(ch);
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}

std::string unhex(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) out += static_cast<char>(std::stoi(std::string(text.substr(i, 2)), nullptr, 16));
    return out;
}

TEST(crypto_pbkdf2_known_answers) {
    unsigned char out[32];
    // RFC 7914 section 11 (first 32 bytes of the 64-byte result).
    crypto::pbkdf2Sha256("passwd", reinterpret_cast<const unsigned char*>("salt"), 4, 1, out, 32);
    CHECK_EQ(hex(std::string_view(reinterpret_cast<char*>(out), 32)),
             std::string("55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"));
    // Real-world settings, cross-checked against Python's hashlib (backed by OpenSSL).
    unsigned char salt[16];
    for (int i = 0; i < 16; ++i) salt[i] = static_cast<unsigned char>(i);
    crypto::pbkdf2Sha256("correct horse battery staple", salt, 16, 600000, out, 32);
    CHECK_EQ(hex(std::string_view(reinterpret_cast<char*>(out), 32)),
             std::string("ef177144eec9420cbc1093d2a8b344a92bc506d0d4ec9c028dd19f8324d8c1e6"));
}

TEST(crypto_aes_gcm_known_answers) {
    // McGrew & Viega, "The Galois/Counter Mode of Operation", test cases 13, 14 and 16.
    const std::string zeroKey(32, '\0');
    const std::string zeroNonce(12, '\0');
    auto key = [](const std::string& k) { return reinterpret_cast<const unsigned char*>(k.data()); };
    CHECK_EQ(hex(crypto::aes256GcmEncrypt(key(zeroKey), key(zeroNonce), "", "")),
             std::string("530f8afbc74536b9a963b4f1c4cb738b"));
    CHECK_EQ(hex(crypto::aes256GcmEncrypt(key(zeroKey), key(zeroNonce), "", std::string(16, '\0'))),
             std::string("cea7403d4d606b6e074ec5d3baf39d18d0d1c8a799996bf0265b98b5d48ab919"));

    const std::string k16 = unhex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308");
    const std::string n16 = unhex("cafebabefacedbaddecaf888");
    const std::string p16 = unhex(
        "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657"
        "ba637b39");
    const std::string a16 = unhex("feedfacedeadbeeffeedfacedeadbeefabaddad2");
    const std::string sealed = crypto::aes256GcmEncrypt(key(k16), key(n16), a16, p16);
    CHECK_EQ(hex(sealed.substr(sealed.size() - 16)), std::string("76fc6ece0f4e1768cddf8853bb2d551b"));
    const auto opened = crypto::aes256GcmDecrypt(key(k16), key(n16), a16, sealed);
    CHECK(opened && *opened == p16);
    CHECK(!crypto::aes256GcmDecrypt(key(k16), key(n16), "other aad", sealed));
}

TEST(file_key_round_trip_and_tamper_detection) {
    const std::string text = "OPENBOOKS\t2\nCOMPANY\tSecret Co\n";
    auto key = FileKey::fromNewPassword("a long passphrase", crypto::kMinIterations);
    const std::string file = key->encryptFile(text);
    CHECK(FileKey::isEncrypted(file));
    CHECK(file.find("Secret") == std::string::npos);
    CHECK(file != key->encryptFile(text));  // fresh nonce every time
    CHECK(key->matches("a long passphrase"));
    CHECK(!key->matches("a long passphrasE"));

    std::unique_ptr<FileKey> reopened;
    CHECK_EQ(FileKey::decryptFile(file, "a long passphrase", &reopened), text);
    CHECK(reopened && reopened->matches("a long passphrase"));
    // The reopened key encrypts files the original password still opens.
    CHECK_EQ(FileKey::decryptFile(reopened->encryptFile("x"), "a long passphrase"), std::string("x"));

    bool wrongPassword = false;
    try {
        FileKey::decryptFile(file, "not the password");
    } catch (const WrongPasswordError&) {
        wrongPassword = true;
    }
    CHECK(wrongPassword);

    // Flipping any byte - header (iterations, salt, nonce), ciphertext or tag - is detected.
    int detected = 0;
    const std::size_t positions[] = {10, 14, 31, crypto::kHeaderSize, file.size() - 1};
    for (std::size_t pos : positions) {
        std::string bad = file;
        bad[pos] = static_cast<char>(bad[pos] ^ 0x01);
        try {
            FileKey::decryptFile(bad, "a long passphrase");
        } catch (const Error&) {
            ++detected;
        }
    }
    CHECK_EQ(detected, 5);
    CHECK_THROWS(FileKey::decryptFile(file.substr(0, 30), "a long passphrase"));  // truncated
    CHECK_THROWS(FileKey::fromNewPassword("short"));                              // too short

    // A crafted header asking for billions of iterations is refused, not ground through.
    std::string greedy = file;
    greedy[10] = greedy[11] = greedy[12] = greedy[13] = '\xff';
    CHECK_THROWS(FileKey::decryptFile(greedy, "a long passphrase"));
}

TEST(encrypted_books_on_disk) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "openbooks-crypto-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string path = (dir / "books.obk").string();

    Fixture f;
    f.b.company.name = "Very Private LLC";
    f.invoice("2026-01-05", M("123.45"));
    f.b.save(path);  // plaintext first, which also leaves a plaintext .bak on the next save
    f.b.save(path);
    CHECK(fs::exists(path + ".bak"));

    auto key = FileKey::fromNewPassword("hunter2hunter2", crypto::kMinIterations);
    f.b.save(path, key.get());
    CHECK(Book::isEncryptedFile(path));
    CHECK(!fs::exists(path + ".bak"));  // the old plaintext backup is gone
    CHECK(!fs::exists(path + ".tmp"));
    std::ifstream raw(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(raw)), std::istreambuf_iterator<char>());
    raw.close();
    CHECK(bytes.find("Very Private") == std::string::npos);
    CHECK(bytes.find("123.45") == std::string::npos);

    bool required = false;
    try {
        Book::load(path);
    } catch (const PasswordRequiredError&) {
        required = true;
    }
    CHECK(required);
    CHECK_THROWS(Book::load(path, "wrong password!"));
    std::unique_ptr<FileKey> loadedKey;
    const Book loaded = Book::load(path, "hunter2hunter2", &loadedKey);
    CHECK_EQ(loaded.company.name, std::string("Very Private LLC"));
    CHECK_EQ(loaded.balance(f.ar), M("123.45"));

    // Saving again keeps it encrypted, and the backup is now the encrypted previous version.
    loaded.save(path, loadedKey.get());
    CHECK(Book::isEncryptedFile(path) && Book::isEncryptedFile(path + ".bak"));
    // Removing the password.
    loaded.save(path);
    CHECK(!Book::isEncryptedFile(path));
    CHECK_EQ(Book::load(path).company.name, std::string("Very Private LLC"));
    fs::remove_all(dir);
}

// ------------------------------------------------ estimates, credits, receipts

Document salesDoc(const Fixture& f, DocKind kind, const char* date, Money rate, Decimal taxRate = Decimal()) {
    Document d;
    d.kind = kind;
    d.contactId = f.acme;
    d.date = D(date);
    d.dueDate = d.date.addDays(30);
    d.taxRate = taxRate;
    DocLine l;
    l.accountId = f.service;
    l.description = "Work";
    l.rate = rate;
    d.lines.push_back(l);
    return d;
}

TEST(date_add_months_clamps) {
    CHECK_EQ(D("2025-01-31").addMonths(1).str(), std::string("2025-02-28"));
    CHECK_EQ(D("2024-01-31").addMonths(1).str(), std::string("2024-02-29"));
    CHECK_EQ(D("2025-01-31").addMonths(2).str(), std::string("2025-03-31"));
    CHECK_EQ(D("2025-11-15").addMonths(3).str(), std::string("2026-02-15"));
    CHECK_EQ(D("2025-03-15").addMonths(-4).str(), std::string("2024-11-15"));
    CHECK_EQ(D("2024-02-29").addMonths(12).str(), std::string("2025-02-28"));
}

TEST(estimates_do_not_post_and_convert_to_invoices) {
    Fixture f;
    const int est = f.b.createDocument(salesDoc(f, DocKind::Estimate, "2026-03-01", M("500"), Q("10")));
    const Document& e = f.b.document(est);
    CHECK_EQ(e.number, std::string("EST-1"));
    CHECK_EQ(e.txnId, 0);
    CHECK(f.b.transactions().empty());
    CHECK(f.b.documentBalance(est).isZero());
    CHECK(f.b.contactBalance(f.acme).isZero());
    CHECK_EQ(documentStatus(f.b, f.b.document(est), D("2026-03-05")), std::string("Pending"));
    CHECK_EQ(documentStatus(f.b, f.b.document(est), D("2026-05-01")), std::string("Expired"));

    f.b.setEstimateStatus(est, EstimateStatus::Accepted);
    const int inv = f.b.convertEstimate(est, D("2026-03-10"));
    CHECK_EQ(f.b.document(inv).total(), M("550.00"));
    CHECK_EQ(f.b.document(inv).linkedDocId, est);
    CHECK(f.b.estimateStatus(est) == EstimateStatus::Converted);
    CHECK_EQ(f.b.balance(f.ar), M("550.00"));
    CHECK_THROWS(f.b.convertEstimate(est, D("2026-03-11")));                 // only once
    CHECK_THROWS(f.b.setEstimateStatus(est, EstimateStatus::Declined));      // already converted

    // Voiding the invoice frees the estimate to be converted again.
    f.b.voidDocument(inv);
    CHECK(f.b.estimateStatus(est) == EstimateStatus::Accepted);
    f.b.convertEstimate(est, D("2026-03-12"));
    CHECK(f.b.estimateStatus(est) == EstimateStatus::Converted);
}

TEST(credit_memos_reduce_receivables_and_apply_to_invoices) {
    Fixture f;
    const int inv = f.invoice("2026-01-05", M("300"), Decimal::fromInt(1), Q("10"));  // 330.00
    const int cm = f.b.createDocument(salesDoc(f, DocKind::CreditMemo, "2026-01-10", M("100"), Q("10")));  // 110.00
    CHECK_EQ(f.b.document(cm).number, std::string("CM-1"));
    CHECK_EQ(f.b.balance(f.ar), M("220.00"));
    CHECK_EQ(f.b.balance(f.service), M("-200.00"));   // income reduced
    CHECK_EQ(f.b.balance(f.tax), M("-20.00"));        // tax reduced
    CHECK_EQ(f.b.documentBalance(cm), M("110.00"));   // credit available
    CHECK_EQ(f.b.contactBalance(f.acme), M("220.00"));

    CHECK_THROWS(f.b.applyCredit(cm, inv, M("120")));  // more than the credit
    f.b.applyCredit(cm, inv, M("60"));
    f.b.applyCredit(cm, inv, M("50"));
    CHECK_EQ(f.b.documentBalance(inv), M("220.00"));
    CHECK_EQ(f.b.documentBalance(cm), M("0.00"));
    CHECK_EQ(f.b.document(cm).applications.size(), std::size_t(1));  // merged
    CHECK_EQ(f.b.contactBalance(f.acme), M("220.00"));                // unchanged by applying
    CHECK_THROWS(f.b.voidDocument(inv));                               // credit applied

    // A payment now only needs to cover what is left.
    f.receive("2026-01-20", M("220"));
    CHECK(f.b.documentBalance(inv).isZero());
    CHECK_EQ(documentStatus(f.b, f.b.document(inv), D("2026-01-20")), std::string("Paid"));
    CHECK(sumOfAllBalances(f.b).isZero());

    // Another customer's invoice can't take this credit.
    Contact other;
    other.kind = ContactKind::Customer;
    other.name = "Other";
    const int otherId = f.b.addContact(other);
    Document od = salesDoc(f, DocKind::Invoice, "2026-01-05", M("10"));
    od.contactId = otherId;
    const int otherInv = f.b.createDocument(od);
    const int cm2 = f.b.createDocument(salesDoc(f, DocKind::CreditMemo, "2026-01-11", M("5")));
    CHECK_THROWS(f.b.applyCredit(cm2, otherInv, M("5")));
}

TEST(voiding_or_unapplying_a_credit_reopens_the_invoice) {
    Fixture f;
    const int inv = f.invoice("2026-01-05", M("100"));
    const int cm = f.b.createDocument(salesDoc(f, DocKind::CreditMemo, "2026-01-06", M("40")));
    f.b.applyCredit(cm, inv, M("40"));
    CHECK_EQ(f.b.documentBalance(inv), M("60"));
    f.b.unapplyCredit(cm, inv);
    CHECK_EQ(f.b.documentBalance(inv), M("100"));
    CHECK_THROWS(f.b.unapplyCredit(cm, inv));
    f.b.applyCredit(cm, inv, M("40"));
    f.b.voidDocument(cm);
    CHECK_EQ(f.b.documentBalance(inv), M("100"));
    CHECK_EQ(f.b.balance(f.ar), M("100"));
    f.b.voidDocument(inv);  // nothing applied any more
}

TEST(unapplied_credits_show_in_ar_aging) {
    Fixture f;
    f.invoice("2026-01-01", M("100"));
    f.b.createDocument(salesDoc(f, DocKind::CreditMemo, "2026-01-02", M("30")));
    const Table aging = agingReport(f.b, DocKind::Invoice, D("2026-01-15"));
    const Row* row = aging.find("Acme Corp");
    CHECK(row && row->cells[1] == "70.00");  // 100 current - 30 credit
    CHECK(row && row->cells[6] == "70.00");
    CHECK_EQ(f.b.balance(f.ar), M("70.00"));
}

TEST(sales_receipts_deposit_directly) {
    Fixture f;
    Document d = salesDoc(f, DocKind::SalesReceipt, "2026-02-01", M("80"), Q("5"));
    d.depositAccountId = f.ar;
    CHECK_THROWS(f.b.createDocument(d));  // must be a bank account
    d.depositAccountId = 0;
    CHECK_THROWS(f.b.createDocument(d));  // and one must be chosen
    d.depositAccountId = f.checking;
    const int id = f.b.createDocument(d);
    CHECK_EQ(f.b.document(id).number, std::string("SR-1"));
    CHECK_EQ(f.b.balance(f.checking), M("84.00"));
    CHECK_EQ(f.b.balance(f.service), M("-80.00"));
    CHECK_EQ(f.b.balance(f.tax), M("-4.00"));
    CHECK(f.b.balance(f.ar).isZero());
    CHECK(f.b.documentBalance(id).isZero());
    CHECK(f.b.document(id).dueDate == f.b.document(id).date);
    f.b.voidDocument(id);
    CHECK(f.b.balance(f.checking).isZero());
}

TEST(recurring_invoices_follow_their_schedule) {
    Fixture f;
    RecurringInvoice r;
    r.name = "Monthly retainer";
    r.contactId = f.acme;
    r.startDate = D("2026-01-31");
    r.endDate = D("2026-05-15");
    DocLine l;
    l.accountId = f.service;
    l.description = "Retainer";
    l.rate = M("1000");
    r.lines.push_back(l);
    const int id = f.b.addRecurring(r);
    CHECK_EQ(f.b.recurringInvoice(id).total(), M("1000.00"));
    CHECK_EQ(f.b.dueRecurringCount(D("2026-03-31")), 3);

    const auto first = f.b.createDueRecurringInvoices(D("2026-03-31"));
    CHECK_EQ(first.size(), std::size_t(3));
    if (first.size() == 3) {
        CHECK_EQ(f.b.document(first[0]).date.str(), std::string("2026-01-31"));
        CHECK_EQ(f.b.document(first[1]).date.str(), std::string("2026-02-28"));
        CHECK_EQ(f.b.document(first[2]).date.str(), std::string("2026-03-31"));
        CHECK_EQ(f.b.document(first[2]).recurringId, id);
    }
    CHECK(f.b.createDueRecurringInvoices(D("2026-03-31")).empty());  // idempotent
    CHECK_EQ(f.b.createDueRecurringInvoices(D("2026-12-31")).size(), std::size_t(1));  // Apr 30 only; ends May 15
    CHECK(f.b.recurringInvoice(id).finished());
    CHECK_EQ(f.b.balance(f.ar), M("4000.00"));

    RecurringInvoice weekly = r;
    weekly.name = "Weekly cleaning";
    weekly.frequency = Frequency::Weekly;
    weekly.interval = 2;
    weekly.startDate = D("2026-06-01");
    weekly.endDate.reset();
    const int w = f.b.addRecurring(weekly);
    CHECK_EQ(f.b.recurringInvoice(w).occurrence(3).str(), std::string("2026-07-13"));
    RecurringInvoice paused = f.b.recurringInvoice(w);
    paused.active = false;
    f.b.updateRecurring(paused);
    CHECK_EQ(f.b.dueRecurringCount(D("2026-12-31")), 0);
    CHECK_THROWS(f.b.addRecurring(r));  // duplicate name
}

TEST(recurring_run_is_all_or_nothing) {
    Fixture f;
    RecurringInvoice r;
    r.name = "Retainer";
    r.contactId = f.acme;
    r.startDate = D("2026-01-01");
    DocLine l;
    l.accountId = f.service;
    l.rate = M("10");
    r.lines.push_back(l);
    f.b.addRecurring(r);
    f.b.company.closedThrough = D("2026-01-31");  // the January invoice can't be created
    const std::size_t before = f.b.documents().size();
    CHECK_THROWS(f.b.createDueRecurringInvoices(D("2026-03-31")));
    CHECK_EQ(f.b.documents().size(), before);
    CHECK_EQ(f.b.recurringInvoices().front().occurrencesCreated, 0);

    RecurringInvoice ancient = r;
    ancient.name = "Ancient";
    ancient.frequency = Frequency::Weekly;
    ancient.startDate = D("1990-01-01");
    f.b.company.closedThrough.reset();
    f.b.addRecurring(ancient);
    CHECK_THROWS(f.b.createDueRecurringInvoices(D("2026-03-31")));  // runaway guard
}

TEST(new_document_kinds_round_trip_and_render) {
    Fixture f;
    const int inv = f.invoice("2026-01-05", M("100"));
    const int est = f.b.createDocument(salesDoc(f, DocKind::Estimate, "2026-01-01", M("100")));
    f.b.setEstimateStatus(est, EstimateStatus::Declined);
    const int cm = f.b.createDocument(salesDoc(f, DocKind::CreditMemo, "2026-01-06", M("25")));
    f.b.applyCredit(cm, inv, M("25"));
    Document sr = salesDoc(f, DocKind::SalesReceipt, "2026-01-07", M("12"));
    sr.depositAccountId = f.checking;
    const int receipt = f.b.createDocument(sr);
    RecurringInvoice r;
    r.name = "Tab\there";
    r.contactId = f.acme;
    r.startDate = D("2026-01-01");
    r.endDate = D("2026-12-31");
    r.lines = f.b.document(inv).lines;
    f.b.addRecurring(r);
    f.b.createDueRecurringInvoices(D("2026-02-01"));

    std::stringstream first;
    f.b.write(first);
    CHECK(first.str().rfind("OPENBOOKS\t2\n", 0) == 0);
    std::stringstream in(first.str());
    const Book loaded = Book::read(in);
    std::stringstream second;
    loaded.write(second);
    CHECK_EQ(first.str(), second.str());
    CHECK_EQ(loaded.documentBalance(inv), M("75.00"));
    CHECK(loaded.estimateStatus(est) == EstimateStatus::Declined);
    CHECK_EQ(loaded.document(receipt).depositAccountId, f.checking);
    CHECK_EQ(loaded.recurringInvoices().front().occurrencesCreated, 2);
    CHECK_EQ(loaded.company.nextCreditMemoNumber, 2);

    for (int id : {est, cm, receipt}) {
        const std::string pdfOut = documentPdf(loaded, id);
        CHECK(pdfStructureValid(pdfOut, nullptr));
        CHECK(noActiveContent(pdfOut));
    }
    CHECK(documentPdf(loaded, est).find("(ESTIMATE)") != std::string::npos);
    CHECK(documentPdf(loaded, cm).find("(CREDIT REMAINING)") != std::string::npos);
    CHECK(documentPdf(loaded, receipt).find("(PAID)") != std::string::npos);
    CHECK(documentPdfFileName(loaded, cm).rfind("Credit Memo CM-1", 0) == 0);
}

// -------------------------------------------------------------------- CLI

struct CliRun {
    int code;
    std::string out;
    std::string err;
};

CliRun cli(const std::string& file, const std::vector<std::string>& args, const std::string& input = "") {
    std::vector<std::string> full = {"-f", file};
    full.insert(full.end(), args.begin(), args.end());
    std::ostringstream out;
    std::ostringstream err;
    std::istringstream in(input);
    const int code = runCli(full, out, err, in);
    return {code, out.str(), err.str()};
}

TEST(tokenizer) {
    const auto t = tokenize(R"(invoice create --customer "Acme Corp" --line 'desc=A "quoted";amount=5' x\y)");
    CHECK_EQ(t.size(), std::size_t(7));
    if (t.size() == 7) {
        CHECK_EQ(t[3], std::string("Acme Corp"));
        CHECK_EQ(t[5], std::string("desc=A \"quoted\";amount=5"));
        CHECK_EQ(t[6], std::string("x\\y"));
    }
    CHECK_THROWS(tokenize("say \"unterminated"));
}

TEST(cli_end_to_end) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "openbooks-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string file = (dir / "books.obk").string();

    CHECK_EQ(cli(file, {"init", "--company", "Test Co"}).code, 0);
    CHECK_EQ(cli(file, {"init"}).code, 1);  // refuses to overwrite
    CHECK_EQ(cli(file, {"customer", "add", "Acme Corp", "--email", "ap@acme.test"}).code, 0);
    CHECK_EQ(cli(file, {"item", "add", "Consulting", "--price", "150", "--income", "Service Revenue"}).code, 0);
    auto r = cli(file, {"invoice", "create", "--customer", "acme", "--date", "2025-01-10", "--tax", "10%",
                        "--line", "item=Consulting;qty=4", "--line", "desc=Travel;amount=50;account=Other Income;taxable=no"});
    CHECK_EQ(r.code, 0);
    CHECK(r.out.find("total 710.00") != std::string::npos);  // 600 + 60 tax + 50

    r = cli(file, {"receive-payment", "--customer", "Acme", "--amount", "700", "--to", "Checking", "--date", "2025-01-20"});
    CHECK_EQ(r.code, 0);
    r = cli(file, {"invoice", "show", "1001"});
    CHECK(r.out.find("10.00") != std::string::npos);  // balance due

    CHECK_EQ(cli(file, {"expense", "--from", "Checking", "--account", "Office Supplies", "--amount", "25", "--date", "2025-01-21"}).code, 0);
    r = cli(file, {"report", "balance-sheet", "--as-of", "2025-01-31"});
    CHECK_EQ(r.code, 0);
    CHECK(r.out.find("WARNING") == std::string::npos);
    CHECK(r.out.find("685.00") != std::string::npos);  // total assets: checking 675 + A/R 10

    // Errors are reported, exit non-zero, and never corrupt the saved file.
    r = cli(file, {"journal", "--debit", "Checking=10", "--credit", "Sales=9"});
    CHECK_EQ(r.code, 1);
    CHECK(r.err.find("out of balance") != std::string::npos);
    CHECK_EQ(cli(file, {"expense", "--from", "Checking", "--amount", "5", "--bogus", "x", "--account", "Rent"}).code, 1);

    // CSV import with duplicate detection.
    {
        std::ofstream csv(dir / "bank.csv");
        csv << "Date,Description,Amount\n2025-01-22,\"Coffee, Inc\",-4.50\n2025-01-23,Refund,12.00\n";
    }
    const std::string csvPath = (dir / "bank.csv").string();
    r = cli(file, {"import-csv", "Checking", csvPath});
    CHECK(r.out.find("Imported 2") != std::string::npos);
    r = cli(file, {"import-csv", "Checking", csvPath});
    CHECK(r.out.find("Imported 0") != std::string::npos);

    // Shell mode runs several commands against the same file.
    r = cli(file, {"shell"}, "vendor add \"Paper Co\"\nbill create --vendor paper --line \"account=Office Supplies;amount=80\" --date 2025-01-25\nbills --open\nexit\n");
    CHECK_EQ(r.code, 0);
    CHECK(r.out.find("Paper Co") != std::string::npos);
    CHECK(r.out.find("80.00") != std::string::npos);

    r = cli(file, {"report", "trial-balance", "--csv"});
    CHECK(r.out.find("Account,Debit,Credit") == 0);

    // Estimates, credit memos, sales receipts and recurring invoices.
    r = cli(file, {"estimate", "create", "--customer", "Acme", "--date", "2025-02-01", "--line", "item=Consulting;qty=2"});
    CHECK(r.out.find("EST-1") != std::string::npos);
    CHECK_EQ(cli(file, {"estimate", "accept", "EST-1"}).code, 0);
    r = cli(file, {"estimate", "convert", "EST-1", "--date", "2025-02-02"});
    CHECK(r.out.find("to invoice 1002") != std::string::npos);
    r = cli(file, {"credit-memo", "create", "--customer", "Acme", "--date", "2025-02-03", "--line",
                   "desc=Discount;amount=50;account=Service Revenue", "--apply", "1002"});
    CHECK_EQ(r.code, 0);
    CHECK(r.out.find("applied 50.00 to invoice 1002") != std::string::npos);
    r = cli(file, {"sales-receipt", "create", "--customer", "Acme", "--deposit-to", "Checking", "--date", "2025-02-04",
                   "--line", "desc=Walk-in;amount=20;account=Sales"});
    CHECK(r.out.find("SR-1") != std::string::npos);
    CHECK_EQ(cli(file, {"recurring", "add", "Monthly", "--customer", "Acme", "--start", "2025-01-15", "--end",
                        "2025-03-31", "--line", "desc=Hosting;amount=30;account=Sales"}).code, 0);
    r = cli(file, {"recurring", "run", "--through", "2025-06-30"});
    CHECK(r.out.find("3 invoice(s) created") != std::string::npos);
    CHECK(cli(file, {"recurring", "list"}).out.find("Finished") != std::string::npos);
    r = cli(file, {"report", "balance-sheet", "--as-of", "2025-12-31"});
    CHECK(r.out.find("WARNING") == std::string::npos);

    // Password protection: set, reopen with the password, wrong password, remove.
    // (The byte-order mark is what Windows PowerShell puts in front of piped input.)
    r = cli(file, {"password", "set"}, "\xEF\xBB\xBFtrustno1trustno1\r\ntrustno1trustno1\r\n");
    CHECK_EQ(r.code, 0);
    CHECK(Book::isEncryptedFile(file));
    CHECK_EQ(cli(file, {"password", "set"}, "trustno1trustno1\nmismatch1\nmismatch2\n").code, 1);  // repeat differs
    r = cli(file, {"report", "trial-balance"}, "trustno1trustno1\n");
    CHECK_EQ(r.code, 0);
    CHECK(r.out.find("Total") != std::string::npos);
    r = cli(file, {"report", "trial-balance"}, "not it at all\n");
    CHECK_EQ(r.code, 1);
    CHECK(r.err.find("wrong password") != std::string::npos);
    CHECK(cli(file, {"password", "status"}, "trustno1trustno1\n").out.find("AES-256-GCM") != std::string::npos);
    // In the shell the password is asked once, then reused, even after a failing command.
    r = cli(file, {"shell"}, "journal --debit Checking=1 --credit Sales=2\ntrustno1trustno1\ncustomers\nexit\n");
    CHECK(r.err.find("out of balance") != std::string::npos);
    CHECK(r.out.find("Acme Corp") != std::string::npos);
    CHECK(r.err.find("Password for") == r.err.rfind("Password for"));  // asked exactly once
    CHECK_EQ(cli(file, {"password", "remove"}, "trustno1trustno1\n").code, 0);
    CHECK(!Book::isEncryptedFile(file));

    // PDF export refuses to overwrite unless forced.
    const std::string pdfPath = (dir / "inv.pdf").string();
    CHECK_EQ(cli(file, {"invoice", "pdf", "1001", "--out", pdfPath}).code, 0);
    CHECK(fs::file_size(dir / "inv.pdf") > 1000);
    CHECK_EQ(cli(file, {"invoice", "pdf", "1001", "--out", pdfPath}).code, 1);
    CHECK_EQ(cli(file, {"invoice", "pdf", "1001", "--out", pdfPath, "--force"}).code, 0);

    fs::remove_all(dir);
}

}  // namespace

int main() {
    for (const auto& t : registry()) {
        const int before = g_failures;
        try {
            t.fn();
        } catch (const std::exception& e) {
            ++g_failures;
            std::cerr << t.name << ": unexpected exception: " << e.what() << '\n';
        }
        std::cout << (g_failures == before ? "[ ok ] " : "[FAIL] ") << t.name << '\n';
    }
    std::cout << '\n' << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}

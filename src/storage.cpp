// OpenBooks file format (version 1)
//
// A UTF-8 text file with one record per line and tab-separated fields. Backslash,
// tab, CR and LF inside fields are escaped as \\ \t \r \n. The first line is
// "OPENBOOKS<TAB>1". Child records (SPLIT, LINE, APPLY) refer to their parent by id.
// The format is deliberately simple so books stay readable, diffable and recoverable
// without OpenBooks itself.

#include "openbooks/book.hpp"
#include "openbooks/util.hpp"

#include <algorithm>
#include <climits>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <istream>
#include <ostream>
#include <set>

namespace ob {
namespace {

std::string escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c;
        }
    }
    return out;
}

std::string unescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char n = s[++i];
            switch (n) {
                case 't': out += '\t'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                default: out += n;
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

void record(std::ostream& out, std::initializer_list<std::string> fields) {
    bool first = true;
    for (const auto& f : fields) {
        if (!first) out << '\t';
        out << escape(f);
        first = false;
    }
    out << '\n';
}

std::string num(int v) { return std::to_string(v); }
std::string flag(bool v) { return v ? "1" : "0"; }
std::string stateCode(SplitState s) {
    switch (s) {
        case SplitState::Cleared: return "c";
        case SplitState::Reconciled: return "R";
        default: return "";
    }
}

class Fields {
public:
    Fields(std::vector<std::string> fields, int line) : fields_(std::move(fields)), line_(line) {}

    const std::string& str(std::size_t i) const {
        if (i >= fields_.size()) fail("missing field " + std::to_string(i));
        return fields_[i];
    }
    int integer(std::size_t i) const {
        const auto v = parseInt(str(i));
        if (!v || *v < INT_MIN || *v > INT_MAX) fail("invalid number '" + str(i) + "'");
        return static_cast<int>(*v);
    }
    Money money(std::size_t i) const {
        const auto v = Money::parse(str(i));
        if (!v) fail("invalid amount '" + str(i) + "'");
        return *v;
    }
    Decimal decimal(std::size_t i) const {
        const auto v = Decimal::parse(str(i));
        if (!v) fail("invalid decimal '" + str(i) + "'");
        return *v;
    }
    Date date(std::size_t i) const {
        const std::string& s = str(i);
        std::optional<Date> v;
        if (s != "today") v = Date::parse(s);  // "today" is valid input elsewhere but never in a file
        if (!v) fail("invalid date '" + s + "'");
        return *v;
    }
    std::optional<Date> optionalDate(std::size_t i) const {
        if (str(i).empty()) return std::nullopt;
        return date(i);
    }
    bool boolean(std::size_t i) const {
        const std::string& s = str(i);
        if (s == "1") return true;
        if (s == "0") return false;
        fail("invalid flag '" + s + "'");
    }
    SplitState state(std::size_t i) const {
        const std::string& s = str(i);
        if (s.empty()) return SplitState::Uncleared;
        if (s == "c") return SplitState::Cleared;
        if (s == "R") return SplitState::Reconciled;
        fail("invalid cleared state '" + s + "'");
    }
    template <typename T>
    T parsed(std::size_t i, std::optional<T> (*parser)(std::string_view), const char* what) const {
        const auto v = parser(str(i));
        if (!v) fail(std::string("invalid ") + what + " '" + str(i) + "'");
        return *v;
    }
    [[noreturn]] void fail(const std::string& message) const {
        throw Error("corrupt books file, line " + std::to_string(line_) + ": " + message);
    }

private:
    std::vector<std::string> fields_;
    int line_;
};

template <typename T>
int nextId(const std::vector<T>& v) {
    int maxId = 0;
    for (const auto& x : v) maxId = std::max(maxId, x.id);
    return maxId + 1;
}

template <typename T>
void sortAndCheckIds(std::vector<T>& v, const char* what) {
    std::stable_sort(v.begin(), v.end(), [](const T& a, const T& b) { return a.id < b.id; });
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i].id <= 0 || (i > 0 && v[i].id == v[i - 1].id))
            throw Error(std::string("corrupt books file: duplicate or invalid ") + what + " id " +
                        std::to_string(v[i].id));
    }
}

}  // namespace

void Book::write(std::ostream& out) const {
    out << "OPENBOOKS\t1\n";
    const Company& c = company;
    record(out, {"COMPANY", c.name, c.address, num(c.fiscalYearStartMonth), num(c.defaultTermsDays),
                 c.closedThrough ? c.closedThrough->str() : "", num(c.nextInvoiceNumber),
                 num(c.receivablesAccountId), num(c.payablesAccountId), num(c.salesTaxAccountId),
                 num(c.retainedEarningsAccountId)});
    for (const auto& a : accounts_)
        record(out, {"ACCOUNT", num(a.id), a.number, a.name, toString(a.type), a.description, flag(a.active)});
    for (const auto& k : contacts_)
        record(out, {"CONTACT", num(k.id), toString(k.kind), k.name, k.email, k.phone, k.address, num(k.termsDays),
                     flag(k.active)});
    for (const auto& i : items_)
        record(out, {"ITEM", num(i.id), i.name, i.description, i.price.str(), num(i.incomeAccountId),
                     num(i.expenseAccountId), flag(i.taxable), flag(i.active)});
    for (const auto& t : transactions_) {
        record(out, {"TXN", num(t.id), t.date.str(), toString(t.kind), t.ref, t.payee, t.memo, flag(t.voided)});
        for (const auto& s : t.splits)
            record(out, {"SPLIT", num(t.id), num(s.accountId), s.amount.str(), stateCode(s.state), s.memo});
    }
    for (const auto& d : documents_) {
        record(out, {"DOC", num(d.id), toString(d.kind), d.number, num(d.contactId), d.date.str(), d.dueDate.str(),
                     d.taxRate.str(), d.memo, num(d.txnId), flag(d.voided)});
        for (const auto& l : d.lines)
            record(out, {"LINE", num(d.id), num(l.itemId), num(l.accountId), l.quantity.str(), l.rate.str(),
                         l.amount.str(), flag(l.taxable), l.description});
    }
    for (const auto& p : payments_) {
        record(out, {"PAY", num(p.id), toString(p.kind), num(p.contactId), p.date.str(), num(p.accountId),
                     p.amount.str(), p.ref, p.memo, num(p.txnId), flag(p.voided)});
        for (const auto& a : p.applications) record(out, {"APPLY", num(p.id), num(a.documentId), a.amount.str()});
    }
}

Book Book::read(std::istream& in) {
    Book b;
    std::string line;
    int lineNo = 1;
    if (!std::getline(in, line)) throw Error("the books file is empty");
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (startsWith(line, "\xEF\xBB\xBF")) line.erase(0, 3);
    const auto header = split(line, '\t');
    if (header.size() < 2 || header[0] != "OPENBOOKS") throw Error("this is not an OpenBooks file");
    if (header[1] != "1") throw Error("unsupported OpenBooks file version " + header[1] + "; upgrade OpenBooks");

    std::map<int, std::size_t> txnIndex;
    std::map<int, std::size_t> docIndex;
    std::map<int, std::size_t> payIndex;

    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        auto raw = split(line, '\t');
        for (auto& f : raw) f = unescape(f);
        const Fields f(std::move(raw), lineNo);
        const std::string& type = f.str(0);

        if (type == "COMPANY") {
            Company& c = b.company;
            c.name = f.str(1);
            c.address = f.str(2);
            c.fiscalYearStartMonth = f.integer(3);
            if (c.fiscalYearStartMonth < 1 || c.fiscalYearStartMonth > 12) f.fail("invalid fiscal year start month");
            c.defaultTermsDays = f.integer(4);
            c.closedThrough = f.optionalDate(5);
            c.nextInvoiceNumber = f.integer(6);
            c.receivablesAccountId = f.integer(7);
            c.payablesAccountId = f.integer(8);
            c.salesTaxAccountId = f.integer(9);
            c.retainedEarningsAccountId = f.integer(10);
        } else if (type == "ACCOUNT") {
            Account a;
            a.id = f.integer(1);
            a.number = f.str(2);
            a.name = f.str(3);
            a.type = f.parsed<AccountType>(4, parseAccountType, "account type");
            a.description = f.str(5);
            a.active = f.boolean(6);
            b.accounts_.push_back(std::move(a));
        } else if (type == "CONTACT") {
            Contact k;
            k.id = f.integer(1);
            k.kind = f.parsed<ContactKind>(2, parseContactKind, "contact kind");
            k.name = f.str(3);
            k.email = f.str(4);
            k.phone = f.str(5);
            k.address = f.str(6);
            k.termsDays = f.integer(7);
            k.active = f.boolean(8);
            b.contacts_.push_back(std::move(k));
        } else if (type == "ITEM") {
            Item i;
            i.id = f.integer(1);
            i.name = f.str(2);
            i.description = f.str(3);
            i.price = f.money(4);
            i.incomeAccountId = f.integer(5);
            i.expenseAccountId = f.integer(6);
            i.taxable = f.boolean(7);
            i.active = f.boolean(8);
            b.items_.push_back(std::move(i));
        } else if (type == "TXN") {
            Transaction t;
            t.id = f.integer(1);
            t.date = f.date(2);
            t.kind = f.parsed<TxnKind>(3, parseTxnKind, "transaction kind");
            t.ref = f.str(4);
            t.payee = f.str(5);
            t.memo = f.str(6);
            t.voided = f.boolean(7);
            if (!txnIndex.emplace(t.id, b.transactions_.size()).second) f.fail("duplicate transaction id");
            b.transactions_.push_back(std::move(t));
        } else if (type == "SPLIT") {
            const auto it = txnIndex.find(f.integer(1));
            if (it == txnIndex.end()) f.fail("line refers to an unknown transaction");
            Split s;
            s.accountId = f.integer(2);
            s.amount = f.money(3);
            s.state = f.state(4);
            s.memo = f.str(5);
            b.transactions_[it->second].splits.push_back(std::move(s));
        } else if (type == "DOC") {
            Document d;
            d.id = f.integer(1);
            d.kind = f.parsed<DocKind>(2, parseDocKind, "document kind");
            d.number = f.str(3);
            d.contactId = f.integer(4);
            d.date = f.date(5);
            d.dueDate = f.date(6);
            d.taxRate = f.decimal(7);
            d.memo = f.str(8);
            d.txnId = f.integer(9);
            d.voided = f.boolean(10);
            if (!docIndex.emplace(d.id, b.documents_.size()).second) f.fail("duplicate document id");
            b.documents_.push_back(std::move(d));
        } else if (type == "LINE") {
            const auto it = docIndex.find(f.integer(1));
            if (it == docIndex.end()) f.fail("line refers to an unknown invoice or bill");
            DocLine l;
            l.itemId = f.integer(2);
            l.accountId = f.integer(3);
            l.quantity = f.decimal(4);
            l.rate = f.money(5);
            l.amount = f.money(6);
            l.taxable = f.boolean(7);
            l.description = f.str(8);
            b.documents_[it->second].lines.push_back(std::move(l));
        } else if (type == "PAY") {
            Payment p;
            p.id = f.integer(1);
            p.kind = f.parsed<PaymentKind>(2, parsePaymentKind, "payment kind");
            p.contactId = f.integer(3);
            p.date = f.date(4);
            p.accountId = f.integer(5);
            p.amount = f.money(6);
            p.ref = f.str(7);
            p.memo = f.str(8);
            p.txnId = f.integer(9);
            p.voided = f.boolean(10);
            if (!payIndex.emplace(p.id, b.payments_.size()).second) f.fail("duplicate payment id");
            b.payments_.push_back(std::move(p));
        } else if (type == "APPLY") {
            const auto it = payIndex.find(f.integer(1));
            if (it == payIndex.end()) f.fail("application refers to an unknown payment");
            b.payments_[it->second].applications.push_back(Application{f.integer(2), f.money(3)});
        } else {
            f.fail("unknown record type '" + type + "'");
        }
    }

    sortAndCheckIds(b.accounts_, "account");
    sortAndCheckIds(b.contacts_, "contact");
    sortAndCheckIds(b.items_, "item");
    sortAndCheckIds(b.transactions_, "transaction");
    sortAndCheckIds(b.documents_, "document");
    sortAndCheckIds(b.payments_, "payment");

    // Integrity: every transaction must balance and reference real accounts.
    std::set<int> accountIds;
    for (const auto& a : b.accounts_) accountIds.insert(a.id);
    for (const auto& t : b.transactions_) {
        Money sum;
        for (const auto& s : t.splits) {
            if (!accountIds.count(s.accountId))
                throw Error("corrupt books file: transaction #" + std::to_string(t.id) + " uses unknown account " +
                            std::to_string(s.accountId));
            sum += s.amount;
        }
        if (!sum.isZero())
            throw Error("corrupt books file: transaction #" + std::to_string(t.id) + " is out of balance by " +
                        sum.formatted());
    }

    b.nextAccountId_ = nextId(b.accounts_);
    b.nextContactId_ = nextId(b.contacts_);
    b.nextItemId_ = nextId(b.items_);
    b.nextTxnId_ = nextId(b.transactions_);
    b.nextDocId_ = nextId(b.documents_);
    b.nextPaymentId_ = nextId(b.payments_);
    return b;
}

void Book::save(const std::string& path) const {
    namespace fs = std::filesystem;
    const fs::path target = fs::u8path(path);
    fs::path tmp = target;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw Error("cannot write '" + tmp.u8string() + "'");
        write(out);
        out.flush();
        if (!out) throw Error("failed while writing '" + tmp.u8string() + "'");
    }
    std::error_code ec;
    if (fs::exists(target, ec)) {
        fs::path backup = target;
        backup += ".bak";
        fs::copy_file(target, backup, fs::copy_options::overwrite_existing, ec);
    }
    ec.clear();
    fs::rename(tmp, target, ec);
    if (ec) {  // some platforms refuse to rename over an existing file
        ec.clear();
        fs::remove(target, ec);
        ec.clear();
        fs::rename(tmp, target, ec);
        if (ec) throw Error("cannot replace '" + path + "': " + ec.message());
    }
}

Book Book::load(const std::string& path) {
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
    if (!in) throw Error("cannot open '" + path + "' (create new books with: openbooks init)");
    return read(in);
}

}  // namespace ob

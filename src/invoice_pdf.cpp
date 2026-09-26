#include "openbooks/invoice_pdf.hpp"

#include "openbooks/pdf.hpp"
#include "openbooks/util.hpp"

#include <algorithm>

namespace ob {
namespace {

using pdf::Align;
using pdf::Color;
using pdf::Font;

constexpr Color kInk{0.12, 0.13, 0.15};
constexpr Color kMuted{0.42, 0.45, 0.49};
constexpr Color kAccent{0.17, 0.63, 0.11};
constexpr Color kAccentLight{0.90, 0.96, 0.89};
constexpr Color kRule{0.84, 0.86, 0.89};
constexpr Color kHeaderBg{0.94, 0.95, 0.96};

constexpr double kMargin = 50;
constexpr double kBody = 10;      // body font size
constexpr double kSmall = 8.5;    // labels
constexpr double kLineGap = 13;   // baseline-to-baseline for body text

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    for (auto& l : split(text, '\n')) {
        std::string t = trim(l);
        if (!t.empty()) out.push_back(t);
    }
    return out;
}

}  // namespace

std::string documentPdfFileName(const Book& book, int documentId) {
    const Document& d = book.document(documentId);
    const std::string raw = std::string(d.kind == DocKind::Invoice ? "Invoice " : "Bill ") + d.number + " - " +
                            book.contact(d.contactId).name;
    std::string name;
    for (char ch : raw) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 32 || std::string_view("<>:\"/\\|?*").find(ch) != std::string_view::npos) continue;
        name += ch;
    }
    name = trim(name);
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();  // Windows quirk
    while (!name.empty() && name.front() == '.') name.erase(0, 1);                         // no hidden / ".." names
    if (name.empty()) name = "document";
    if (name.size() > 120) name.resize(120);
    return name + ".pdf";
}

std::string documentPdf(const Book& book, int documentId) {
    const Document& d = book.document(documentId);
    const Contact& contact = book.contact(d.contactId);
    const Company& company = book.company;
    const bool invoice = d.kind == DocKind::Invoice;
    const Money paid = book.documentPaid(documentId);
    const Money balance = book.documentBalance(documentId);

    const pdf::PageSize size = company.paperSize == PaperSize::A4 ? pdf::kA4 : pdf::kLetter;
    pdf::Document doc(size);
    doc.setTitle(std::string(invoice ? "Invoice " : "Bill ") + d.number);
    doc.setAuthor(company.name);

    const double left = kMargin;
    const double right = size.width - kMargin;
    const double top = size.height - 56;

    // The footer (payment instructions etc.) is reserved on every page.
    const auto footerLines = pdf::wrapText(company.invoiceFooter, Font::Regular, kSmall, right - left);
    const bool hasFooter = !trim(company.invoiceFooter).empty();
    const double footerHeight = hasFooter ? static_cast<double>(footerLines.size()) * 11.0 + 10.0 : 0.0;
    const double bottom = 48 + footerHeight + 12;

    pdf::Page* page = &doc.addPage();

    // ---- stamp (drawn first so it sits behind the content)
    if (d.voided) {
        page->rotatedText(size.width / 2, size.height / 2, 30, "VOID", Font::Bold, 110, Color{0.98, 0.84, 0.84});
    } else if (balance.isZero() && d.total() > Money()) {
        page->rotatedText(size.width / 2, size.height / 2, 30, "PAID", Font::Bold, 110, Color{0.86, 0.94, 0.85});
    }

    // ---- header: company on the left, title and key facts on the right
    double y = top;
    page->text(left, y, company.name, Font::Bold, 16, kInk);
    double companyY = y - 17;
    std::vector<std::string> companyLines = lines(company.address);
    if (!trim(company.phone).empty()) companyLines.push_back(trim(company.phone));
    if (!trim(company.email).empty()) companyLines.push_back(trim(company.email));
    for (const auto& l : companyLines) {
        page->text(left, companyY, l, Font::Regular, 9.5, kMuted);
        companyY -= 12;
    }

    page->text(right, y - 6, invoice ? "INVOICE" : "BILL", Font::Bold, 26, kAccent, Align::Right);
    double metaY = y - 32;
    auto meta = [&](const std::string& label, const std::string& value) {
        page->text(right - 96, metaY, label, Font::Regular, 9.5, kMuted, Align::Right);
        page->text(right, metaY, value, Font::Bold, 9.5, kInk, Align::Right);
        metaY -= 14;
    };
    meta(invoice ? "Invoice #" : "Bill #", d.number);
    meta("Date", d.date.str());
    meta("Due date", d.dueDate.str());

    y = std::min(companyY, metaY) - 8;
    page->line(left, y, right, y, 1.5, kAccent);
    y -= 24;

    // ---- bill-to block and balance-due box
    const double boxWidth = 180;
    const double boxTop = y + 12;
    page->fillRect(right - boxWidth, boxTop - 54, boxWidth, 54, d.voided ? kHeaderBg : kAccentLight);
    page->text(right - boxWidth + 12, boxTop - 17, d.voided ? "VOIDED" : "BALANCE DUE", Font::Bold, kSmall, kMuted);
    page->text(right - 12, boxTop - 42, balance.formatted(), Font::Bold, 20, kInk, Align::Right);

    page->text(left, y, invoice ? "BILL TO" : "VENDOR", Font::Bold, kSmall, kMuted);
    y -= 15;
    const double partyWidth = right - boxWidth - 20 - left;
    for (const auto& l : pdf::wrapText(contact.name, Font::Bold, 11, partyWidth)) {
        page->text(left, y, l, Font::Bold, 11, kInk);
        y -= 14;
    }
    std::vector<std::string> partyLines = lines(contact.address);
    if (!trim(contact.email).empty()) partyLines.push_back(trim(contact.email));
    for (const auto& l : partyLines) {
        page->text(left, y, l, Font::Regular, 9.5, kMuted);
        y -= 12;
    }
    y = std::min(y, boxTop - 54) - 26;

    // ---- line items
    const double qtyRight = right - 190;
    const double rateRight = right - 100;
    const double amountRight = right - 8;
    const double descLeft = left + 8;
    const double descWidth = qtyRight - 60 - descLeft;

    auto tableHeader = [&] {
        page->fillRect(left, y - 7, right - left, 22, kHeaderBg);
        page->text(descLeft, y, "DESCRIPTION", Font::Bold, kSmall, kMuted);
        page->text(qtyRight, y, "QTY", Font::Bold, kSmall, kMuted, Align::Right);
        page->text(rateRight, y, "RATE", Font::Bold, kSmall, kMuted, Align::Right);
        page->text(amountRight, y, "AMOUNT", Font::Bold, kSmall, kMuted, Align::Right);
        y -= 26;
    };
    auto newPage = [&](bool withTableHeader) {
        page = &doc.addPage();
        y = top;
        page->text(left, y, company.name, Font::Bold, 11, kInk);
        page->text(right, y, std::string(invoice ? "Invoice " : "Bill ") + d.number + " (continued)", Font::Regular,
                   9.5, kMuted, Align::Right);
        y -= 12;
        page->line(left, y, right, y, 1.0, kRule);
        y -= 24;
        if (withTableHeader) tableHeader();
    };

    tableHeader();
    for (const auto& l : d.lines) {
        const auto descLines = pdf::wrapText(l.description, Font::Regular, kBody, descWidth);
        const double rowHeight = static_cast<double>(descLines.size() - 1) * kLineGap + 21;
        if (y - rowHeight < bottom) newPage(true);
        double lineY = y;
        for (const auto& dl : descLines) {
            page->text(descLeft, lineY, dl, Font::Regular, kBody, kInk);
            lineY -= kLineGap;
        }
        page->text(qtyRight, y, l.quantity.str(), Font::Regular, kBody, kInk, Align::Right);
        page->text(rateRight, y, l.rate.formatted(), Font::Regular, kBody, kInk, Align::Right);
        page->text(amountRight, y, l.amount.formatted(), Font::Regular, kBody, kInk, Align::Right);
        y = lineY + kLineGap - 8;
        page->line(left, y, right, y, 0.5, kRule);
        y -= 15;
    }

    // ---- totals
    const Money tax = d.tax();
    const int totalRows = 3 + (tax.isZero() ? 0 : 1) + (paid.isZero() ? 0 : 1);
    if (y - totalRows * 17.0 - 10 < bottom) newPage(false);
    y -= 4;
    const double labelRight = right - 120;
    auto totalRow = [&](const std::string& label, Money amount, bool strong) {
        const Font font = strong ? Font::Bold : Font::Regular;
        const double sz = strong ? 11 : kBody;
        page->text(labelRight, y, label, font, sz, strong ? kInk : kMuted, Align::Right);
        page->text(amountRight, y, amount.formatted(), font, sz, kInk, Align::Right);
        y -= strong ? 19 : 16;
    };
    totalRow("Subtotal", d.subtotal(), false);
    if (!tax.isZero()) totalRow("Sales tax (" + d.taxRate.str() + "%)", tax, false);
    totalRow("Total", d.total(), true);
    if (!paid.isZero()) totalRow("Payments received", -paid, false);
    y -= 5;  // clear the previous row's descenders before the highlight band
    page->fillRect(right - 250, y - 7, 250, 22, kAccentLight);
    totalRow("Balance due", balance, true);

    // ---- notes
    if (!trim(d.memo).empty()) {
        const auto memoLines = pdf::wrapText(d.memo, Font::Regular, 9.5, right - left);
        if (y - 30 - static_cast<double>(memoLines.size()) * 12 < bottom) newPage(false);
        y -= 16;
        page->text(left, y, "NOTES", Font::Bold, kSmall, kMuted);
        y -= 14;
        for (const auto& l : memoLines) {
            page->text(left, y, l, Font::Regular, 9.5, kInk);
            y -= 12;
        }
    }

    // ---- footer on every page
    const std::size_t pages = doc.pageCount();
    for (std::size_t i = 0; i < pages; ++i) {
        pdf::Page& p = doc.page(i);
        double fy = 48 + footerHeight - 10;
        if (hasFooter) {
            p.line(left, fy + 12, right, fy + 12, 0.5, kRule);
            for (const auto& l : footerLines) {
                p.text(size.width / 2, fy, l, Font::Regular, kSmall, kMuted, Align::Center);
                fy -= 11;
            }
        }
        if (pages > 1) {
            p.text(right, 30, "Page " + std::to_string(i + 1) + " of " + std::to_string(pages), Font::Regular, 8,
                   kMuted, Align::Right);
        }
    }
    return doc.build();
}

}  // namespace ob

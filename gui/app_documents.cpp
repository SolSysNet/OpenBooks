// Invoices, estimates, credit memos, sales receipts, bills and recurring invoice templates:
// the list screens, the detail panel, the shared editor and the apply-credit dialog.

#include "app.hpp"

#include "imgui.h"
#include "openbooks/invoice_pdf.hpp"
#include "openbooks/reports.hpp"
#include "openbooks/util.hpp"
#include "platform.hpp"
#include "theme.hpp"
#include "widgets.hpp"

#include <algorithm>
#include <cfloat>
#include <filesystem>
#include <fstream>

namespace obgui {

using namespace ob;

namespace {

const ImGuiTableFlags kListFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV |
                                   ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;

Money lookup(const std::unordered_map<int, Money>& m, int id) {
    const auto it = m.find(id);
    return it == m.end() ? Money() : it->second;
}

bool matches(const std::string& haystack, const std::string& needle) {
    return needle.empty() || toLower(haystack).find(toLower(trim(needle))) != std::string::npos;
}

int defaultMoneyAccount(const Book& b) {
    for (const auto& a : b.accounts()) {
        if (a.active && ui::isMoneyAccount(b, a) && iequals(a.name, "Checking")) return a.id;
    }
    for (const auto& a : b.accounts()) {
        if (a.active && ui::isMoneyAccount(b, a) && a.type == AccountType::Asset) return a.id;
    }
    return 0;
}

float buttonWidth(const char* label) { return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2; }

void rightAlign(float width) {
    ImGui::SameLine();
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > width) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - width);
}

const char* newLabel(DocKind k) {
    switch (k) {
        case DocKind::Invoice: return "New invoice";
        case DocKind::Bill: return "Enter bill";
        case DocKind::Estimate: return "New estimate";
        case DocKind::CreditMemo: return "New credit memo";
        case DocKind::SalesReceipt: return "New sales receipt";
    }
    return "New";
}

// Status filters shown above each list.
std::vector<const char*> filtersFor(DocKind k) {
    switch (k) {
        case DocKind::Estimate: return {"All", "Open", "Converted", "Declined", "Expired", "Void"};
        case DocKind::CreditMemo: return {"All", "Unapplied", "Applied", "Void"};
        case DocKind::SalesReceipt: return {"All", "Void"};
        default: return {"All", "Open", "Overdue", "Paid", "Void"};
    }
}

bool passesFilter(const Book& b, const Document& d, const std::string& status, const char* filter) {
    const std::string f = filter;
    if (f == "All") return true;
    if (f == "Void") return d.voided;
    if (d.voided) return false;
    if (f == "Open") return documentIsOpen(b, d);
    if (f == "Unapplied") return !b.documentBalance(d.id).isZero();
    return status == f;
}

void writeFile(const std::filesystem::path& target, const std::string& bytes, bool& ok) {
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    ok = static_cast<bool>(out);
}

}  // namespace

// ------------------------------------------------------------------ routing

ListState& App::listFor(DocKind kind) {
    switch (kind) {
        case DocKind::Bill: return bills_;
        case DocKind::Estimate: return estimates_;
        case DocKind::CreditMemo: return creditMemos_;
        case DocKind::SalesReceipt: return salesReceipts_;
        default: return invoices_;
    }
}

Screen App::screenFor(DocKind kind) const {
    switch (kind) {
        case DocKind::Bill: return Screen::Bills;
        case DocKind::Estimate: return Screen::Estimates;
        case DocKind::CreditMemo: return Screen::CreditMemos;
        case DocKind::SalesReceipt: return Screen::SalesReceipts;
        default: return Screen::Invoices;
    }
}

void App::openDocument(int documentId) {
    const DocKind kind = book_->document(documentId).kind;
    listFor(kind).selectedId = documentId;
    listFor(kind).filter = 0;
    go(screenFor(kind));
}

// -------------------------------------------------------------- list screen

void App::drawDocuments(DocKind kind) {
    ListState& st = listFor(kind);
    const Book& b = *book_;
    const Derived& d = derived();
    const Date today = Date::today();
    const bool customerSide = partyKind(kind) == ContactKind::Customer;

    ui::Heading(documentListTitle(kind));
    const auto filters = filtersFor(kind);
    if (st.filter >= static_cast<int>(filters.size())) st.filter = 0;
    for (int i = 0; i < static_cast<int>(filters.size()); ++i) {
        if (i) ImGui::SameLine(0, 2);
        const bool active = st.filter == i;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
        if (ImGui::Button(filters[static_cast<std::size_t>(i)])) st.filter = i;
        if (active) ImGui::PopStyleColor();
    }
    ImGui::SameLine(0, 16);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    ui::InputStringHint("##search", customerSide ? "Search number or customer" : "Search number or vendor", st.search);
    rightAlign(buttonWidth(newLabel(kind)));
    if (ui::PrimaryButton(newLabel(kind))) startDocument(kind, 0, screenFor(kind));
    ImGui::Dummy(ImVec2(0, 4));

    bool selectionVisible = false;
    for (const auto& doc : b.documents()) {
        if (doc.id == st.selectedId && doc.kind == kind) selectionVisible = true;
    }
    if (!selectionVisible) st.selectedId = 0;
    const bool compact = st.selectedId != 0;
    const float listWidth = compact ? ImGui::GetContentRegionAvail().x * 0.50f : 0.0f;
    const bool hasBalance = kind != DocKind::Estimate && kind != DocKind::SalesReceipt;

    Money totalShown;
    Money openShown;
    ImGui::BeginGroup();
    if (ImGui::BeginTable(compact ? "##docsCompact" : "##docs", 7, kListFlags | ImGuiTableFlags_Hideable,
                          ImVec2(listWidth, -ImGui::GetFrameHeightWithSpacing()))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Number", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5.5f);
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.0f);
        ImGui::TableSetupColumn(kind == DocKind::Estimate ? "Expires" : "Due", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::GetFontSize() * 6.0f);
        ImGui::TableSetupColumn(customerSide ? "Customer" : "Vendor", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.5f);
        ImGui::TableSetupColumn(kind == DocKind::CreditMemo ? "Credit left" : "Balance", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::GetFontSize() * 6.5f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.5f);
        ImGui::TableSetColumnEnabled(2, docHasDueDate(kind) && !compact);
        ImGui::TableSetColumnEnabled(4, !compact || !hasBalance);
        ImGui::TableSetColumnEnabled(5, hasBalance);
        ImGui::TableHeadersRow();

        std::vector<const Document*> rows;
        for (const auto& doc : b.documents()) {
            if (doc.kind != kind) continue;
            std::string status = documentStatus(b, doc, today);
            if (!passesFilter(b, doc, status, filters[static_cast<std::size_t>(st.filter)])) continue;
            if (!matches(doc.number + " " + b.contact(doc.contactId).name, st.search)) continue;
            rows.push_back(&doc);
        }
        std::stable_sort(rows.begin(), rows.end(), [](const Document* x, const Document* y) {
            return x->date != y->date ? x->date > y->date : x->id > y->id;
        });
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const Document& doc = *rows[static_cast<std::size_t>(i)];
                const std::string status = documentStatus(b, doc, today);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(doc.id);
                if (ImGui::Selectable(doc.number.c_str(), st.selectedId == doc.id, ImGuiSelectableFlags_SpanAllColumns))
                    st.selectedId = st.selectedId == doc.id ? 0 : doc.id;
                ImGui::PopID();
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(doc.date.str().c_str());
                if (ImGui::TableSetColumnIndex(2)) {
                    if (status == "Overdue" || status == "Expired") ImGui::TextColored(colorNegative(), "%s", doc.dueDate.str().c_str());
                    else ImGui::TextUnformatted(doc.dueDate.str().c_str());
                }
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(b.contact(doc.contactId).name.c_str());
                if (ImGui::TableSetColumnIndex(4)) ui::MoneyText(doc.voided ? Money() : doc.total());
                if (ImGui::TableSetColumnIndex(5)) ui::MoneyText(lookup(d.documentBalance, doc.id));
                ImGui::TableSetColumnIndex(6);
                ui::Badge(status.c_str(), ui::statusColor(status));
            }
        }
        for (const Document* doc : rows) {
            if (!doc->voided) totalShown += doc->total();
            openShown += lookup(d.documentBalance, doc->id);
        }
        if (rows.empty()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(3);
            ui::Muted("Nothing here yet.");
        }
        ImGui::EndTable();
    }
    ImGui::AlignTextToFramePadding();
    if (kind == DocKind::CreditMemo) ImGui::Text("Total %s    Unapplied %s", totalShown.formatted().c_str(), openShown.formatted().c_str());
    else if (hasBalance) ImGui::Text("Total %s    Open %s", totalShown.formatted().c_str(), openShown.formatted().c_str());
    else ImGui::Text("Total %s", totalShown.formatted().c_str());
    ImGui::EndGroup();

    if (st.selectedId) {
        ImGui::SameLine();
        drawDocumentDetail(st.selectedId);
    }
}

// ------------------------------------------------------------- detail panel

void App::drawDocumentDetail(int documentId) {
    const Book& b = *book_;
    const Derived& d = derived();
    const Document doc = b.document(documentId);  // copies: actions below may replace the books
    const DocKind kind = doc.kind;
    const std::string partyName = b.contact(doc.contactId).name;
    const Money bal = lookup(d.documentBalance, doc.id);
    const std::string status = documentStatus(b, doc, Date::today());
    std::vector<std::function<void()>> deferred;  // run after the panel is drawn

    ImGui::BeginChild("##docDetail", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ui::Heading((std::string(docTitle(kind)) + " " + doc.number).c_str());
    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6);
    ui::Badge(status.c_str(), ui::statusColor(status));

    const float col = ImGui::GetFontSize() * 6.5f;
    ui::FormLabel(kind == DocKind::Bill ? "Vendor" : "Customer", col);
    ImGui::TextUnformatted(partyName.c_str());
    ui::FormLabel("Date", col);
    ImGui::TextUnformatted(doc.date.str().c_str());
    if (docHasDueDate(kind)) {
        ui::FormLabel(kind == DocKind::Estimate ? "Expires" : "Due", col);
        ImGui::TextUnformatted(doc.dueDate.str().c_str());
    }
    if (kind == DocKind::SalesReceipt) {
        ui::FormLabel("Deposited to", col);
        ImGui::TextUnformatted(ui::accountName(b, doc.depositAccountId).c_str());
    }
    if (doc.linkedDocId) {
        const Document& linked = b.document(doc.linkedDocId);
        ui::FormLabel(kind == DocKind::Estimate ? "Invoice" : "Estimate", col);
        const int linkedId = linked.id;
        if (ImGui::TextLink((linked.number + (linked.voided ? " (void)" : "")).c_str()))
            deferred.push_back([this, linkedId] { openDocument(linkedId); });
    }
    if (doc.recurringId) {
        ui::FormLabel("Recurring", col);
        std::string name = "(deleted template)";
        for (const auto& r : b.recurringInvoices()) {
            if (r.id == doc.recurringId) name = r.name;
        }
        ImGui::TextUnformatted(name.c_str());
    }
    if (!doc.memo.empty()) {
        ui::FormLabel("Memo", col);
        ImGui::TextWrapped("%s", doc.memo.c_str());
    }
    ImGui::Dummy(ImVec2(0, 4));

    if (ImGui::BeginTable("##lines", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter)) {
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Qty", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3.5f);
        ImGui::TableSetupColumn("Rate", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.0f);
        ImGui::TableSetupColumn("Amount", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.5f);
        ImGui::TableHeadersRow();
        for (const auto& l : doc.lines) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(l.description.c_str());
            ImGui::SetItemTooltip("%s", ui::accountName(b, l.accountId).c_str());
            ImGui::TableSetColumnIndex(1);
            ui::TextRight(l.quantity.str().c_str());
            ImGui::TableSetColumnIndex(2);
            ui::MoneyText(l.rate);
            ImGui::TableSetColumnIndex(3);
            ui::MoneyText(l.amount);
        }
        ImGui::EndTable();
    }
    auto totalLine = [&](const std::string& label, Money amount, bool bold) {
        const float x = ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 15.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, x));
        if (bold) ImGui::PushFont(g_fonts.bold, 0.0f);
        ImGui::TextUnformatted(label.c_str());
        ImGui::SameLine();
        ui::MoneyText(amount);
        if (bold) ImGui::PopFont();
    };
    totalLine("Subtotal", doc.subtotal(), false);
    if (!doc.tax().isZero()) totalLine("Sales tax (" + doc.taxRate.str() + "%)", doc.tax(), false);
    totalLine("Total", doc.total(), true);
    const Money paid = b.documentPaid(doc.id);
    const Money credited = b.documentCredited(doc.id);
    if (kind == DocKind::Invoice || kind == DocKind::Bill) {
        if (!paid.isZero()) totalLine("Payments", -paid, false);
        if (!credited.isZero()) totalLine("Credits", -credited, false);
        totalLine("Balance due", bal, true);
    } else if (kind == DocKind::CreditMemo) {
        if (!doc.applications.empty()) totalLine("Applied", -doc.applied(), false);
        totalLine("Credit remaining", bal, true);
    }

    // Payments and credits applied, with a way to take a credit back off.
    if (!paid.isZero()) {
        ImGui::Dummy(ImVec2(0, 4));
        ui::Muted("Payments applied");
        for (const auto& p : b.payments()) {
            if (p.voided) continue;
            for (const auto& a : p.applications) {
                if (a.documentId == doc.id)
                    ImGui::BulletText("%s  payment #%d  %s", p.date.str().c_str(), p.id, a.amount.formatted().c_str());
            }
        }
    }
    auto creditRow = [&](int creditMemoId, int invoiceId, const std::string& text) {
        ImGui::PushID(creditMemoId * 100000 + invoiceId);
        ImGui::BulletText("%s", text.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            deferred.push_back([this, creditMemoId, invoiceId] {
                commit([=](Book& book) { book.unapplyCredit(creditMemoId, invoiceId); }, nullptr, "Credit removed");
            });
        }
        ImGui::PopID();
    };
    if (kind == DocKind::Invoice && !credited.isZero()) {
        ImGui::Dummy(ImVec2(0, 4));
        ui::Muted("Credits applied");
        for (const auto& cm : b.documents()) {
            if (cm.kind != DocKind::CreditMemo || cm.voided) continue;
            for (const auto& a : cm.applications) {
                if (a.documentId == doc.id) creditRow(cm.id, doc.id, "Credit memo " + cm.number + "  " + a.amount.formatted());
            }
        }
    }
    if (kind == DocKind::CreditMemo && !doc.applications.empty()) {
        ImGui::Dummy(ImVec2(0, 4));
        ui::Muted("Applied to");
        for (const auto& a : doc.applications)
            creditRow(doc.id, a.documentId, "Invoice " + b.document(a.documentId).number + "  " + a.amount.formatted());
    }

    // ---- actions
    ImGui::Dummy(ImVec2(0, 8));
    const int id = doc.id;
    const int contactId = doc.contactId;
    if (!doc.voided) {
        if ((kind == DocKind::Invoice || kind == DocKind::Bill) && !bal.isZero()) {
            if (ui::PrimaryButton(kind == DocKind::Invoice ? "Receive payment" : "Pay bill")) {
                const bool invoice = kind == DocKind::Invoice;
                startPayment(invoice ? PaymentKind::Received : PaymentKind::Paid, contactId);
                PaymentFormState& f = invoice ? receiveForm_ : payForm_;
                f.selected.clear();
                f.applied.clear();
                f.selected[id] = true;
                f.applied[id] = bal.str();
                f.amount = bal.str();
            }
            ImGui::SameLine();
        }
        if (kind == DocKind::Invoice && !bal.isZero()) {
            bool creditAvailable = false;
            for (const auto& cm : b.documents()) {
                if (cm.kind == DocKind::CreditMemo && cm.contactId == contactId && !lookup(d.documentBalance, cm.id).isZero())
                    creditAvailable = true;
            }
            if (creditAvailable) {
                if (ImGui::Button("Apply credit...")) openApplyCredit(contactId, 0, id);
                ImGui::SameLine();
            }
        }
        if (kind == DocKind::CreditMemo && !bal.isZero()) {
            if (ui::PrimaryButton("Apply to invoice...")) openApplyCredit(contactId, id, 0);
            ImGui::SameLine();
        }
        if (kind == DocKind::Estimate) {
            const EstimateStatus es = b.estimateStatus(id);
            if (es != EstimateStatus::Converted) {
                if (ui::PrimaryButton("Convert to invoice")) {
                    deferred.push_back([this, id] {
                        int invoiceId = 0;
                        if (commit([&](Book& book) { invoiceId = book.convertEstimate(id, Date::today()); }, nullptr)) {
                            notify("Created invoice " + book_->document(invoiceId).number);
                            openDocument(invoiceId);
                        }
                    });
                }
                ImGui::SameLine();
                auto statusButton = [&](const char* label, EstimateStatus target) {
                    if (es == target) return;
                    if (ImGui::Button(label)) {
                        deferred.push_back([this, id, target] {
                            commit([=](Book& book) { book.setEstimateStatus(id, target); }, nullptr,
                                   std::string("Estimate marked ") + toLower(toString(target)));
                        });
                    }
                    ImGui::SameLine();
                };
                statusButton("Mark accepted", EstimateStatus::Accepted);
                statusButton("Mark declined", EstimateStatus::Declined);
                if (es != EstimateStatus::Pending) statusButton("Reopen", EstimateStatus::Pending);
            }
        }
    }
    ImGui::NewLine();

    auto writePdf = [&](const std::filesystem::path& target) {
        bool ok = false;
        writeFile(target, documentPdf(b, id), ok);
        return ok;
    };
    if (ImGui::Button("Copy as text")) {
        ImGui::SetClipboardText(renderDocument(b, id).c_str());
        notify("Copied to clipboard");
    }
    ImGui::SameLine();
    if (ImGui::Button("Save PDF...")) {
        const std::string name = documentPdfFileName(b, id);
        std::optional<std::string> path;
        if (nativeFileDialogsAvailable()) path = saveFileDialog("Save PDF", {"PDF files (*.pdf)", "*.pdf"}, "pdf", name);
        else path = (std::filesystem::u8path(path_).parent_path() / std::filesystem::u8path(name)).u8string();
        if (path) {
            if (writePdf(std::filesystem::u8path(*path))) notify("Saved " + *path);
            else notify("Could not write " + *path, true);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Preview PDF")) {
        // Written to a private temp folder and opened in the local default PDF viewer.
        std::error_code ec;
        const auto dir = std::filesystem::temp_directory_path(ec) / "OpenBooks";
        std::filesystem::create_directories(dir, ec);
        const auto target = dir / std::filesystem::u8path(documentPdfFileName(b, id));
        if (!writePdf(target)) notify("Could not write " + target.u8string(), true);
        else if (!openWithDefaultApp(target.u8string())) notify("Saved preview to " + target.u8string());
    }
    ImGui::SetItemTooltip("Open in your PDF viewer to check or print");
    if (!doc.voided) {
        ImGui::SameLine();
        if (ImGui::Button("Void")) {
            const std::string label = std::string(docTitle(kind)) + " " + doc.number;
            confirm("Void " + label + "?",
                    kind == DocKind::Estimate
                        ? std::string("The estimate stays on file marked VOID.")
                        : std::string("It stays on file marked VOID and no longer affects any balance. "
                                      "Payments or credits applied to it must be removed first."),
                    "Void", [this, id, label] {
                        commit([id](Book& book) { book.voidDocument(id); }, nullptr, label + " voided");
                    });
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) listFor(kind).selectedId = 0;
    ImGui::EndChild();

    for (auto& action : deferred) action();
}

// --------------------------------------------------------------- apply credit

void App::openApplyCredit(int contactId, int creditMemoId, int invoiceId) {
    applyCredit_ = ApplyCreditState{};
    applyCredit_.contactId = contactId;
    applyCredit_.creditMemoId = creditMemoId;
    applyCredit_.invoiceId = invoiceId;
    // Pre-select the oldest credit / oldest open invoice so the common case is one click.
    const Derived& d = derived();
    for (const auto& doc : book_->documents()) {
        if (doc.contactId != contactId || doc.voided || lookup(d.documentBalance, doc.id).isZero()) continue;
        if (doc.kind == DocKind::CreditMemo && applyCredit_.creditMemoId == 0) applyCredit_.creditMemoId = doc.id;
        if (doc.kind == DocKind::Invoice && applyCredit_.invoiceId == 0) applyCredit_.invoiceId = doc.id;
    }
    requestPopup("Apply Credit");
}

void App::drawApplyCreditModal() {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Apply Credit", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ApplyCreditState& s = applyCredit_;
    const Book& b = *book_;
    const Derived& d = derived();

    std::vector<ui::Option> memos;
    std::vector<ui::Option> invoices;
    std::string memoPreview = "Choose a credit memo...";
    std::string invoicePreview = "Choose an invoice...";
    for (const auto& doc : b.documents()) {
        if (doc.contactId != s.contactId || doc.voided) continue;
        const Money left = lookup(d.documentBalance, doc.id);
        if (left.isZero()) continue;
        const std::string label = doc.number + "  (" + left.formatted() + (doc.kind == DocKind::CreditMemo ? " left)" : " due)");
        if (doc.kind == DocKind::CreditMemo) {
            memos.push_back({doc.id, label, false});
            if (doc.id == s.creditMemoId) memoPreview = label;
        } else if (doc.kind == DocKind::Invoice) {
            invoices.push_back({doc.id, label, false});
            if (doc.id == s.invoiceId) invoicePreview = label;
        }
    }
    // Default the amount to as much as both sides allow.
    auto suggest = [&] {
        if (!s.creditMemoId || !s.invoiceId) return;
        const Money a = lookup(d.documentBalance, s.creditMemoId);
        const Money bb = lookup(d.documentBalance, s.invoiceId);
        s.amount = (a < bb ? a : bb).str();
    };
    if (ImGui::IsWindowAppearing()) suggest();

    ui::SubHeading(("Apply credit for " + b.contact(s.contactId).name).c_str());
    const float col = ImGui::GetFontSize() * 6.0f;
    const float width = ImGui::GetFontSize() * 18.0f;
    ui::FormLabel("Credit memo", col);
    if (ui::SearchCombo("##memo", memoPreview, memos, s.creditMemoId, width)) suggest();
    ui::FormLabel("Invoice", col);
    if (ui::SearchCombo("##invoice", invoicePreview, invoices, s.invoiceId, width)) suggest();
    ui::FormLabel("Amount", col);
    ui::MoneyField("##amount", s.amount, ImGui::GetFontSize() * 8.0f);
    ui::ErrorText(s.error);
    ImGui::Separator();
    if (ui::PrimaryButton("Apply")) {
        const auto amount = ui::parseMoney(s.amount);
        if (!s.creditMemoId || !s.invoiceId) s.error = "Choose a credit memo and an invoice.";
        else if (!amount) s.error = "Enter a valid amount.";
        else {
            const int memo = s.creditMemoId;
            const int inv = s.invoiceId;
            const Money value = *amount;
            if (commit([=](Book& book) { book.applyCredit(memo, inv, value); }, &s.error,
                       "Applied " + value.formatted() + " of credit"))
                ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ----------------------------------------------------------------- editor

void App::startDocument(DocKind kind, int contactId, Screen returnTo) {
    editor_ = DocumentEditorState{};
    editor_.kind = kind;
    editor_.contactId = contactId;
    editor_.date = Date::today();
    editor_.returnTo = returnTo;
    editor_.lines.resize(1);
    editor_.depositAccountId = defaultMoneyAccount(*book_);
    int terms = book_->company.defaultTermsDays;
    if (contactId) {
        const Contact& c = book_->contact(contactId);
        if (c.termsDays >= 0) terms = c.termsDays;
    }
    editor_.dueDate = editor_.date.addDays(kind == DocKind::Estimate ? 30 : terms);
    go(Screen::DocumentEditor);
}

void App::startRecurringEditor(int recurringId) {
    editor_ = DocumentEditorState{};
    editor_.kind = DocKind::Invoice;
    editor_.templateMode = true;
    editor_.recurringId = recurringId;
    editor_.returnTo = Screen::Recurring;
    editor_.date = Date::today();
    editor_.endDate = Date::today().addMonths(12);
    if (recurringId) {
        const RecurringInvoice& r = book_->recurringInvoice(recurringId);
        editor_.templateName = r.name;
        editor_.contactId = r.contactId;
        editor_.memo = r.memo;
        editor_.taxRate = r.taxRate.isZero() ? std::string() : r.taxRate.str();
        editor_.frequency = static_cast<int>(r.frequency);
        editor_.interval = r.interval;
        editor_.date = r.startDate;
        editor_.hasEndDate = r.endDate.has_value();
        if (r.endDate) editor_.endDate = *r.endDate;
        editor_.templateActive = r.active;
        for (const auto& l : r.lines) {
            LineDraft draft;
            draft.itemId = l.itemId;
            draft.description = l.description;
            draft.accountId = l.accountId;
            draft.quantity = l.quantity.str();
            draft.rate = l.rate.str();
            draft.taxable = l.taxable;
            editor_.lines.push_back(draft);
        }
    }
    if (editor_.lines.empty()) editor_.lines.resize(1);
    go(Screen::DocumentEditor);
}

void App::drawDocumentEditor() {
    DocumentEditorState& e = editor_;
    const Book& b = *book_;
    const DocKind kind = e.kind;
    const bool sales = kind != DocKind::Bill;
    const ContactKind party = partyKind(kind);

    std::string heading;
    if (e.templateMode) heading = e.recurringId ? "Edit Recurring Invoice" : "New Recurring Invoice";
    else heading = kind == DocKind::Bill ? "Enter Bill" : std::string("New ") + docTitle(kind);
    ui::Heading(heading.c_str());
    if (e.templateMode) ui::Muted("Invoices are created from this template on schedule when you choose Create due invoices.");
    ImGui::Dummy(ImVec2(0, 4));

    auto recomputeDue = [&] {
        if (e.dueDateEdited || e.templateMode) return;
        int terms = b.company.defaultTermsDays;
        if (e.contactId) {
            const Contact& c = b.contact(e.contactId);
            if (c.termsDays >= 0) terms = c.termsDays;
        }
        e.dueDate = e.date.addDays(kind == DocKind::Estimate ? 30 : terms);
    };

    const float col = ImGui::GetFontSize() * 6.5f;
    const float fieldWidth = ImGui::GetFontSize() * 18.0f;
    if (ImGui::BeginTable("##header", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (e.templateMode) {
            ui::FormLabel("Name", col);
            ImGui::SetNextItemWidth(fieldWidth);
            ui::InputStringHint("##name", "e.g. Monthly retainer - Acme", e.templateName);
        }
        ui::FormLabel(party == ContactKind::Customer ? "Customer" : "Vendor", col);
        if (ui::ContactCombo("##contact", b, party, e.contactId, fieldWidth)) recomputeDue();
        ImGui::SameLine();
        if (ImGui::SmallButton("+")) openContactForm(party, 0);
        ImGui::SetItemTooltip(party == ContactKind::Customer ? "Add a new customer" : "Add a new vendor");
        if (!e.templateMode) {
            ui::FormLabel((std::string(docTitle(kind)) + " #").c_str(), col);
            ImGui::SetNextItemWidth(fieldWidth);
            ui::InputStringHint("##number", kind == DocKind::Bill ? "Vendor's invoice number (optional)" : "Automatic",
                                e.number);
        }
        ui::FormLabel("Memo", col);
        ImGui::SetNextItemWidth(fieldWidth);
        ui::InputString("##memo", e.memo);

        ImGui::TableSetColumnIndex(1);
        if (e.templateMode) {
            ui::FormLabel("Repeats", col);
            ImGui::TextUnformatted("every");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
            ImGui::InputInt("##interval", &e.interval);
            e.interval = std::clamp(e.interval, 1, 120);
            ImGui::SameLine();
            static const char* kUnits[] = {"week(s)", "month(s)", "year(s)"};
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
            ImGui::Combo("##unit", &e.frequency, kUnits, 3);
            ui::FormLabel("Starting", col);
            ui::DateField("##start", e.date);
            ui::FormLabel("Ends", col);
            ImGui::Checkbox("##hasEnd", &e.hasEndDate);
            ImGui::SameLine();
            if (e.hasEndDate) ui::DateField("##end", e.endDate);
            else ui::Muted("never");
        } else {
            ui::FormLabel("Date", col);
            if (ui::DateField("##date", e.date)) recomputeDue();
            if (docHasDueDate(kind)) {
                ui::FormLabel(kind == DocKind::Estimate ? "Expires" : "Due date", col);
                if (ui::DateField("##due", e.dueDate)) e.dueDateEdited = true;
            }
            if (kind == DocKind::SalesReceipt) {
                ui::FormLabel("Deposit to", col);
                ui::AccountCombo("##deposit", b, e.depositAccountId,
                                 [&b](const Account& a) { return ui::isMoneyAccount(b, a); }, nullptr, fieldWidth);
            }
        }
        if (sales) {
            ui::FormLabel("Sales tax %", col);
            ui::DecimalField("##tax", e.taxRate, ImGui::GetFontSize() * 5.0f);
        }
        ImGui::EndTable();
    }
    ImGui::Dummy(ImVec2(0, 6));

    // ---- lines
    const int columns = sales ? 8 : 7;
    const ImGuiTableFlags lineFlags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
    int removeLine = -1;
    if (ImGui::BeginTable("##lines", columns, lineFlags)) {
        ImGui::TableSetupColumn("Product / service", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch, 1.8f);
        ImGui::TableSetupColumn(sales ? "Income account" : "Expense account", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Qty", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4.0f);
        ImGui::TableSetupColumn("Rate", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.0f);
        if (sales) ImGui::TableSetupColumn("Tax", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 2.2f);
        ImGui::TableSetupColumn("Amount", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.5f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableHeadersRow();

        const ui::AccountFilter lineAccounts = [&b](const Account& a) { return !b.isSubledgerAccount(a.id); };
        for (std::size_t i = 0; i < e.lines.size(); ++i) {
            LineDraft& l = e.lines[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            int c = 0;
            ImGui::TableSetColumnIndex(c++);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ui::ItemCombo("##item", b, l.itemId) && l.itemId) {
                const Item& item = b.item(l.itemId);
                l.description = item.description.empty() ? item.name : item.description;
                l.rate = item.price.str();
                const int acct = sales ? item.incomeAccountId : item.expenseAccountId;
                if (acct) l.accountId = acct;
                l.taxable = item.taxable;
            }
            ImGui::TableSetColumnIndex(c++);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ui::InputString("##desc", l.description);
            ImGui::TableSetColumnIndex(c++);
            ui::AccountCombo("##acct", b, l.accountId, lineAccounts, nullptr, -FLT_MIN);
            ImGui::TableSetColumnIndex(c++);
            ui::DecimalField("##qty", l.quantity, -FLT_MIN);
            ImGui::TableSetColumnIndex(c++);
            ui::MoneyField("##rate", l.rate, -FLT_MIN);
            if (sales) {
                ImGui::TableSetColumnIndex(c++);
                ImGui::Checkbox("##taxable", &l.taxable);
            }
            ImGui::TableSetColumnIndex(c++);
            const auto q = trim(l.quantity).empty() ? std::optional<Decimal>(Decimal::fromInt(1)) : ui::parseDecimal(l.quantity);
            const auto r = ui::parseMoney(l.rate);
            ImGui::AlignTextToFramePadding();
            if (q && r) ui::MoneyText(multiply(*r, *q));
            ImGui::TableSetColumnIndex(c++);
            if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0))) removeLine = static_cast<int>(i);
            ImGui::SetItemTooltip("Remove line");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (removeLine >= 0) {
        e.lines.erase(e.lines.begin() + removeLine);
        if (e.lines.empty()) e.lines.resize(1);
    }
    if (ImGui::Button("+ Add line")) e.lines.emplace_back();

    // ---- build lines & totals preview
    std::vector<DocLine> lines;
    Decimal taxRate;
    std::string problem;
    if (sales && !trim(e.taxRate).empty()) {
        if (auto t = ui::parseDecimal(e.taxRate)) taxRate = *t;
        else problem = "Sales tax must be a percentage, e.g. 8.25.";
    }
    for (std::size_t i = 0; i < e.lines.size(); ++i) {
        const LineDraft& l = e.lines[i];
        if (l.itemId == 0 && trim(l.description).empty() && trim(l.rate).empty()) continue;  // blank row
        const std::string where = "Line " + std::to_string(i + 1) + ": ";
        DocLine line;
        line.itemId = l.itemId;
        line.description = trim(l.description);
        line.accountId = l.accountId;
        line.taxable = sales && l.taxable;
        const auto q = trim(l.quantity).empty() ? std::optional<Decimal>(Decimal::fromInt(1)) : ui::parseDecimal(l.quantity);
        const auto r = ui::parseMoney(l.rate);
        if (!q && problem.empty()) problem = where + "quantity is not a number.";
        if (!r && problem.empty()) problem = where + "enter a rate.";
        if (!l.accountId && problem.empty()) problem = where + "choose an account.";
        if (q) line.quantity = *q;
        if (r) line.rate = *r;
        line.amount = multiply(line.rate, line.quantity);
        if (line.description.empty() && l.accountId) line.description = ui::accountName(b, l.accountId);
        lines.push_back(line);
    }
    Document preview;
    preview.lines = lines;
    preview.taxRate = taxRate;

    const float totalsX = ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 15.0f;
    auto totalRow = [&](const char* label, Money m, bool bold) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, totalsX));
        if (bold) ImGui::PushFont(g_fonts.bold, kBaseFontSize * 1.15f);
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        ui::MoneyText(m);
        if (bold) ImGui::PopFont();
    };
    totalRow("Subtotal", preview.subtotal(), false);
    if (sales) totalRow("Sales tax", preview.tax(), false);
    totalRow("Total", preview.total(), true);

    ImGui::Dummy(ImVec2(0, 8));
    ui::ErrorText(e.error);
    auto save = [&](bool andNew) {
        if (e.contactId == 0) {
            e.error = party == ContactKind::Customer ? "Choose a customer." : "Choose a vendor.";
            return;
        }
        if (!problem.empty()) {
            e.error = problem;
            return;
        }
        if (e.templateMode) {
            RecurringInvoice r;
            if (e.recurringId) r = b.recurringInvoice(e.recurringId);
            r.name = e.templateName;
            r.contactId = e.contactId;
            r.lines = lines;
            r.taxRate = taxRate;
            r.memo = e.memo;
            r.frequency = static_cast<Frequency>(std::clamp(e.frequency, 0, 2));
            r.interval = e.interval;
            r.startDate = e.date;
            r.endDate = e.hasEndDate ? std::optional<Date>(e.endDate) : std::nullopt;
            r.active = e.templateActive;
            const bool editing = e.recurringId != 0;
            if (commit([&](Book& book) {
                    if (editing) book.updateRecurring(r);
                    else book.addRecurring(r);
                },
                       &e.error, (editing ? "Saved '" : "Added '") + trim(r.name) + "'"))
                go(Screen::Recurring);
            return;
        }
        Document doc = preview;
        doc.kind = kind;
        doc.contactId = e.contactId;
        doc.date = e.date;
        doc.dueDate = e.dueDate;
        doc.number = e.number;
        doc.memo = e.memo;
        doc.depositAccountId = e.depositAccountId;
        int id = 0;
        if (!commit([&](Book& book) { id = book.createDocument(doc); }, &e.error)) return;
        const Document& saved = book_->document(id);
        notify("Created " + std::string(docNoun(kind)) + " " + saved.number + " for " + saved.total().formatted());
        if (andNew) {
            startDocument(kind, 0, e.returnTo);
        } else {
            listFor(kind).selectedId = id;
            listFor(kind).filter = 0;
            go(screenFor(kind));
        }
    };
    if (ui::PrimaryButton("Save")) save(false);
    if (!e.templateMode) {
        ImGui::SameLine();
        if (ImGui::Button("Save and new")) save(true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) go(e.returnTo);
}

// -------------------------------------------------------------- recurring

void App::createDueRecurring() {
    std::vector<int> created;
    if (commit([&](Book& book) { created = book.createDueRecurringInvoices(Date::today()); }, nullptr)) {
        notify(created.empty() ? "No recurring invoices are due"
                               : "Created " + std::to_string(created.size()) + " invoice(s) from recurring templates");
        if (!created.empty()) {
            invoices_.filter = 0;
            invoices_.selectedId = created.back();
            go(Screen::Invoices);
        }
    }
}

void App::drawRecurring() {
    const Book& b = *book_;
    const Date today = Date::today();
    ui::Heading("Recurring Invoices");
    ui::Muted("Templates that create an invoice every week, month or year. Nothing is created until you click Create.");
    ImGui::Dummy(ImVec2(0, 4));
    if (ui::PrimaryButton("New recurring invoice")) startRecurringEditor(0);
    const int due = b.dueRecurringCount(today);
    if (due > 0) {
        ImGui::SameLine();
        const std::string label = "Create " + std::to_string(due) + " due invoice" + (due == 1 ? "" : "s");
        if (ui::PrimaryButton(label.c_str())) {
            createDueRecurring();
            return;
        }
    }
    ImGui::Dummy(ImVec2(0, 4));

    std::vector<std::function<void()>> deferred;
    if (ImGui::BeginTable("##recurring", 8, kListFlags, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn("Customer", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Schedule", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8.0f);
        ImGui::TableSetupColumn("Next", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.0f);
        ImGui::TableSetupColumn("Created", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4.0f);
        ImGui::TableSetupColumn("Amount", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7.0f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 11.0f);
        ImGui::TableHeadersRow();
        for (const auto& r : b.recurringInvoices()) {
            const std::string status = !r.active ? "Paused" : r.finished() ? "Finished" : r.nextDate() <= today ? "Due" : "Active";
            ImGui::PushID(r.id);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(r.name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(b.contact(r.contactId).name.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(r.scheduleText().c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(r.finished() ? "-" : r.nextDate().str().c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%d", r.occurrencesCreated);
            ImGui::TableSetColumnIndex(5);
            ui::MoneyText(r.total());
            ImGui::TableSetColumnIndex(6);
            ui::Badge(status.c_str(), ui::statusColor(status));
            ImGui::TableSetColumnIndex(7);
            const int id = r.id;
            if (ImGui::SmallButton("Edit")) deferred.push_back([this, id] { startRecurringEditor(id); });
            ImGui::SameLine();
            if (ImGui::SmallButton(r.active ? "Pause" : "Resume")) {
                const bool active = !r.active;
                deferred.push_back([this, id, active] {
                    commit([=](Book& book) {
                        RecurringInvoice copy = book.recurringInvoice(id);
                        copy.active = active;
                        book.updateRecurring(copy);
                    }, nullptr, active ? "Resumed" : "Paused");
                });
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete")) {
                const std::string name = r.name;
                deferred.push_back([this, id, name] {
                    confirm("Delete '" + name + "'?", "The template is removed. Invoices it already created are kept.",
                            "Delete", [this, id] { commit([id](Book& book) { book.deleteRecurring(id); }, nullptr, "Deleted"); });
                });
            }
            ImGui::PopID();
        }
        if (b.recurringInvoices().empty()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ui::Muted("No recurring invoices yet.");
        }
        ImGui::EndTable();
    }
    for (auto& action : deferred) action();
}

}  // namespace obgui

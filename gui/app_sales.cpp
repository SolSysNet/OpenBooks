// Customers, vendors, invoices, bills and payments.

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

void rightAlignedButtons(float width) {
    ImGui::SameLine();
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > width) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - width);
}

float buttonWidth(const char* label) {
    return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
}

const ImGuiTableFlags kListFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV |
                                   ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;

}  // namespace

// ------------------------------------------------------------------ contacts

void App::openContactForm(ContactKind kind, int editingId) {
    contactForm_ = ContactForm{};
    contactForm_.kind = kind;
    contactForm_.editingId = editingId;
    if (editingId) {
        const Contact& c = book_->contact(editingId);
        contactForm_.name = c.name;
        contactForm_.email = c.email;
        contactForm_.phone = c.phone;
        contactForm_.address = c.address;
        contactForm_.terms = c.termsDays < 0 ? "" : std::to_string(c.termsDays);
        contactForm_.active = c.active;
    }
    requestPopup("Contact");
}

void App::drawContactModal() {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Contact", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ContactForm& f = contactForm_;
    const bool customer = f.kind == ContactKind::Customer;
    ui::SubHeading(f.editingId ? (customer ? "Edit customer" : "Edit vendor") : (customer ? "New customer" : "New vendor"));
    const float col = ImGui::GetFontSize() * 7.0f;
    const float width = ImGui::GetFontSize() * 22.0f;
    ui::FormLabel("Name", col);
    ImGui::SetNextItemWidth(width);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ui::InputString("##name", f.name);
    ui::FormLabel("Email", col);
    ImGui::SetNextItemWidth(width);
    ui::InputString("##email", f.email);
    ui::FormLabel("Phone", col);
    ImGui::SetNextItemWidth(width);
    ui::InputString("##phone", f.phone);
    ui::FormLabel("Address", col);
    ui::InputMultiline("##address", f.address, ImVec2(width, ImGui::GetTextLineHeight() * 3.6f));
    ui::FormLabel("Terms (days)", col);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
    ui::InputStringHint("##terms", std::to_string(book_->company.defaultTermsDays).c_str(), f.terms);
    ImGui::SameLine();
    ui::Muted("blank = company default");
    if (f.editingId) {
        ImGui::SetCursorPosX(col + ImGui::GetStyle().WindowPadding.x);
        ImGui::Checkbox("Active", &f.active);
    }
    ui::ErrorText(f.error);
    ImGui::Separator();
    if (ui::PrimaryButton("Save")) {
        Contact c;
        c.kind = f.kind;
        c.name = f.name;
        c.email = f.email;
        c.phone = f.phone;
        c.address = f.address;
        c.active = f.active;
        bool ok = true;
        if (trim(f.terms).empty()) {
            c.termsDays = -1;
        } else if (auto t = parseInt(f.terms); t && *t >= 0 && *t <= 3650) {
            c.termsDays = static_cast<int>(*t);
        } else {
            f.error = "Terms must be a number of days.";
            ok = false;
        }
        int savedId = f.editingId;
        if (ok && commit(
                      [&](Book& b) {
                          if (f.editingId) {
                              c.id = f.editingId;
                              b.updateContact(c);
                          } else {
                              savedId = b.addContact(c);
                          }
                      },
                      &f.error, (f.editingId ? "Saved " : "Added ") + trim(c.name))) {
            (customer ? customers_ : vendors_).selectedId = savedId;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void App::drawContacts(ContactKind kind) {
    const bool customers = kind == ContactKind::Customer;
    ListState& st = customers ? customers_ : vendors_;
    const Book& b = *book_;
    const Derived& d = derived();
    const Date today = Date::today();

    ui::Heading(customers ? "Customers" : "Vendors");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    ui::InputStringHint("##search", "Search name, email, phone", st.search);
    ImGui::SameLine();
    ImGui::Checkbox("Show inactive", &st.showInactive);
    const char* newLabel = customers ? "New customer" : "New vendor";
    rightAlignedButtons(buttonWidth(newLabel));
    if (ui::PrimaryButton(newLabel)) openContactForm(kind, 0);
    ImGui::Dummy(ImVec2(0, 4));

    const bool hasSelection = st.selectedId != 0;
    const float listWidth = hasSelection ? ImGui::GetContentRegionAvail().x * 0.48f : 0.0f;
    if (ImGui::BeginTable("##contacts", 4, kListFlags | ImGuiTableFlags_Sortable, ImVec2(listWidth, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort, 2.0f);
        ImGui::TableSetupColumn("Email", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Phone", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Open balance", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7.0f);
        ImGui::TableHeadersRow();

        std::vector<const Contact*> rows;
        for (const auto& c : b.contacts()) {
            if (c.kind != kind || (!c.active && !st.showInactive)) continue;
            if (!matches(c.name + " " + c.email + " " + c.phone, st.search)) continue;
            rows.push_back(&c);
        }
        if (const ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs(); sort && sort->SpecsCount > 0) {
            const auto& spec = sort->Specs[0];
            const bool asc = spec.SortDirection == ImGuiSortDirection_Ascending;
            std::stable_sort(rows.begin(), rows.end(), [&](const Contact* x, const Contact* y) {
                int cmp = 0;
                switch (spec.ColumnIndex) {
                    case 1: cmp = toLower(x->email).compare(toLower(y->email)); break;
                    case 2: cmp = x->phone.compare(y->phone); break;
                    case 3: {
                        const Money bx = lookup(d.contactBalance, x->id);
                        const Money by = lookup(d.contactBalance, y->id);
                        cmp = bx < by ? -1 : (by < bx ? 1 : 0);
                        break;
                    }
                    default: cmp = toLower(x->name).compare(toLower(y->name));
                }
                return asc ? cmp < 0 : cmp > 0;
            });
        }
        for (const Contact* c : rows) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(c->id);
            const std::string label = c->active ? c->name : c->name + " (inactive)";
            if (ImGui::Selectable(label.c_str(), st.selectedId == c->id,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                st.selectedId = c->id;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) openContactForm(kind, c->id);
            }
            ImGui::PopID();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(c->email.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(c->phone.c_str());
            ImGui::TableSetColumnIndex(3);
            ui::MoneyText(lookup(d.contactBalance, c->id));
        }
        if (rows.empty()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ui::Muted(customers ? "No customers yet." : "No vendors yet.");
        }
        ImGui::EndTable();
    }
    if (!hasSelection) return;

    // ---- detail panel
    const Contact* selected = nullptr;
    for (const auto& c : b.contacts()) {
        if (c.id == st.selectedId && c.kind == kind) selected = &c;
    }
    if (!selected) {
        st.selectedId = 0;
        return;
    }
    const Contact c = *selected;  // copy: buttons below may change the books
    ImGui::SameLine();
    ImGui::BeginChild("##contactDetail", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ui::Heading(c.name.c_str());
    if (!c.active) {
        ImGui::SameLine();
        ui::Badge("Inactive", colorMuted());
    }
    if (!c.email.empty()) ImGui::TextUnformatted(c.email.c_str());
    if (!c.phone.empty()) ImGui::TextUnformatted(c.phone.c_str());
    if (!c.address.empty()) ImGui::TextUnformatted(c.address.c_str());
    const int terms = c.termsDays >= 0 ? c.termsDays : b.company.defaultTermsDays;
    ui::Muted(("Terms: Net " + std::to_string(terms)).c_str());
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::Text("Open balance:");
    ImGui::SameLine();
    ImGui::PushFont(g_fonts.bold, 0.0f);
    ui::MoneyText(lookup(d.contactBalance, c.id), false);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 4));

    if (ui::PrimaryButton(customers ? "New invoice" : "Enter bill"))
        startDocument(customers ? DocKind::Invoice : DocKind::Bill, c.id, customers ? Screen::Customers : Screen::Vendors);
    ImGui::SameLine();
    if (ImGui::Button(customers ? "Receive payment" : "Pay bills"))
        startPayment(customers ? PaymentKind::Received : PaymentKind::Paid, c.id);
    ImGui::SameLine();
    if (ImGui::Button("Edit")) openContactForm(kind, c.id);
    ImGui::SameLine();
    if (ImGui::Button(c.active ? "Make inactive" : "Make active")) {
        Contact updated = c;
        updated.active = !c.active;
        commit([updated](Book& book) { book.updateContact(updated); }, nullptr,
               updated.name + (updated.active ? " is active" : " is now inactive"));
        ImGui::EndChild();
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) st.selectedId = 0;

    ImGui::Dummy(ImVec2(0, 6));
    ui::SubHeading("Documents");
    if (ImGui::BeginTable("##contactDocs", 6, kListFlags, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.5f);
        ImGui::TableSetupColumn("Number", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4.5f);
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.0f);
        ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Balance", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.5f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.0f);
        ImGui::TableHeadersRow();
        int openId = 0;
        const auto& docs = b.documents();
        for (auto it = docs.rbegin(); it != docs.rend(); ++it) {
            const Document& doc = *it;
            if (doc.contactId != c.id) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(doc.id);
            if (ImGui::Selectable(docTitle(doc.kind), false, ImGuiSelectableFlags_SpanAllColumns)) openId = doc.id;
            ImGui::PopID();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(doc.number.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(doc.date.str().c_str());
            ImGui::TableSetColumnIndex(3);
            ui::MoneyText(doc.voided ? Money() : doc.total());
            ImGui::TableSetColumnIndex(4);
            ui::MoneyText(lookup(d.documentBalance, doc.id));
            ImGui::TableSetColumnIndex(5);
            const std::string status = documentStatus(b, doc, today);
            ImGui::TextColored(ui::statusColor(status), "%s", status.c_str());
        }
        ImGui::EndTable();
        if (openId) openDocument(openId);
    }
    ImGui::EndChild();
}

// ----------------------------------------------------------------- payments

void App::startPayment(PaymentKind kind, int contactId) {
    PaymentFormState& f = kind == PaymentKind::Received ? receiveForm_ : payForm_;
    const int keepAccount = f.accountId;
    f = PaymentFormState{};
    f.contactId = contactId;
    f.date = Date::today();
    f.accountId = keepAccount ? keepAccount : defaultMoneyAccount(*book_);
    go(kind == PaymentKind::Received ? Screen::ReceivePayment : Screen::PayBills);
}

void App::drawPaymentForm(PaymentKind kind) {
    const bool received = kind == PaymentKind::Received;
    PaymentFormState& f = received ? receiveForm_ : payForm_;
    const Book& b = *book_;
    const Derived& d = derived();
    const ContactKind partyKind = received ? ContactKind::Customer : ContactKind::Vendor;
    const DocKind docKind = received ? DocKind::Invoice : DocKind::Bill;
    if (f.accountId == 0) f.accountId = defaultMoneyAccount(b);
    if (f.date == Date()) f.date = Date::today();

    ui::Heading(received ? "Receive Payment" : "Pay Bills");
    ImGui::Dummy(ImVec2(0, 4));
    const float col = ImGui::GetFontSize() * 7.0f;
    const float fieldWidth = ImGui::GetFontSize() * 18.0f;
    if (ImGui::BeginTable("##header", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ui::FormLabel(received ? "Customer" : "Vendor", col);
        if (ui::ContactCombo("##contact", b, partyKind, f.contactId, fieldWidth)) {
            f.selected.clear();
            f.applied.clear();
        }
        ui::FormLabel("Amount", col);
        ui::MoneyField("##amount", f.amount, ImGui::GetFontSize() * 8.0f);
        ui::FormLabel(received ? "Deposit to" : "Pay from", col);
        ui::AccountCombo("##account", b, f.accountId, [&b](const Account& a) { return ui::isMoneyAccount(b, a); },
                         nullptr, fieldWidth);
        ImGui::TableSetColumnIndex(1);
        ui::FormLabel("Date", col);
        ui::DateField("##date", f.date);
        ui::FormLabel(received ? "Reference #" : "Check #", col);
        ImGui::SetNextItemWidth(fieldWidth * 0.6f);
        ui::InputString("##ref", f.ref);
        ui::FormLabel("Memo", col);
        ImGui::SetNextItemWidth(fieldWidth);
        ui::InputString("##memo", f.memo);
        ImGui::EndTable();
    }
    ImGui::Dummy(ImVec2(0, 6));

    // Open documents for this contact, oldest due first.
    std::vector<const Document*> open;
    for (const auto& doc : b.documents()) {
        if (doc.kind == docKind && doc.contactId == f.contactId && f.contactId && !doc.voided &&
            !lookup(d.documentBalance, doc.id).isZero())
            open.push_back(&doc);
    }
    std::stable_sort(open.begin(), open.end(), [](const Document* x, const Document* y) {
        return x->dueDate != y->dueDate ? x->dueDate < y->dueDate : x->id < y->id;
    });

    const auto amount = ui::parseMoney(f.amount);
    Money applied;
    for (const auto& [id, checked] : f.selected) {
        if (!checked) continue;
        if (auto m = ui::parseMoney(f.applied[id])) applied += *m;
    }

    ui::SubHeading(received ? "Outstanding invoices" : "Open bills");
    ImGui::SameLine();
    ImGui::BeginDisabled(!amount || open.empty());
    if (ImGui::SmallButton("Apply oldest first")) {
        f.selected.clear();
        f.applied.clear();
        Money left = amount ? *amount : Money();
        for (const Document* doc : open) {
            if (left <= Money()) break;
            const Money due = lookup(d.documentBalance, doc->id);
            const Money take = due < left ? due : left;
            f.selected[doc->id] = true;
            f.applied[doc->id] = take.str();
            left -= take;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        f.selected.clear();
        f.applied.clear();
    }

    const float tableHeight = ImGui::GetFontSize() * 14.0f;
    if (ImGui::BeginTable("##open", 6, kListFlags, ImVec2(0, tableHeight))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableSetupColumn("Number", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.0f);
        ImGui::TableSetupColumn("Due", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.0f);
        ImGui::TableSetupColumn("Open balance", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7.0f);
        ImGui::TableSetupColumn("Payment", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8.0f);
        ImGui::TableHeadersRow();
        for (const Document* doc : open) {
            const Money due = lookup(d.documentBalance, doc->id);
            ImGui::PushID(doc->id);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            bool checked = f.selected[doc->id];
            if (ImGui::Checkbox("##on", &checked)) {
                f.selected[doc->id] = checked;
                if (checked && ui::parseMoney(f.applied[doc->id]).value_or(Money()).isZero()) {
                    Money take = due;
                    if (amount && *amount - applied < take && *amount - applied > Money()) take = *amount - applied;
                    f.applied[doc->id] = take.str();
                }
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(doc->number.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(doc->date.str().c_str());
            ImGui::TableSetColumnIndex(3);
            if (doc->dueDate < Date::today()) ImGui::TextColored(colorNegative(), "%s", doc->dueDate.str().c_str());
            else ImGui::TextUnformatted(doc->dueDate.str().c_str());
            ImGui::TableSetColumnIndex(4);
            ui::MoneyText(due);
            ImGui::TableSetColumnIndex(5);
            ImGui::BeginDisabled(!checked);
            ui::MoneyField("##pay", f.applied[doc->id], -FLT_MIN);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        if (open.empty()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            if (f.contactId == 0) ui::Muted(received ? "Choose a customer to see their open invoices." : "Choose a vendor to see open bills.");
            else ui::Muted(received ? "No open invoices. The payment will be kept as a credit." : "No open bills. The payment will be kept as a credit.");
        }
        ImGui::EndTable();
    }

    const Money total = amount ? *amount : applied;
    ImGui::Text("Payment amount: %s", total.formatted().c_str());
    ImGui::SameLine(0, 24);
    ImGui::Text("Applied: %s", applied.formatted().c_str());
    ImGui::SameLine(0, 24);
    const Money credit = total - applied;
    if (credit > Money()) ImGui::TextColored(colorWarning(), "Unapplied credit: %s", credit.formatted().c_str());
    else if (credit < Money()) ImGui::TextColored(colorNegative(), "Applied is more than the payment by %s", (-credit).formatted().c_str());
    if (!open.empty() && applied.isZero() && amount)
        ui::Muted("Nothing selected: the payment will be applied to the oldest open items first.");

    ImGui::Dummy(ImVec2(0, 6));
    ui::ErrorText(f.error);
    if (ui::PrimaryButton(received ? "Save payment" : "Save bill payment")) {
        Payment p;
        p.kind = kind;
        p.contactId = f.contactId;
        p.date = f.date;
        p.accountId = f.accountId;
        p.amount = total;
        p.ref = f.ref;
        p.memo = f.memo;
        f.error.clear();
        for (const auto& [id, checked] : f.selected) {
            if (!checked) continue;
            const auto m = ui::parseMoney(f.applied[id]);
            if (!m) {
                f.error = "One of the payment amounts is not a valid number.";
                break;
            }
            if (!m->isZero()) p.applications.push_back(Application{id, *m});
        }
        if (f.contactId == 0) f.error = received ? "Choose a customer." : "Choose a vendor.";
        else if (!amount && !trim(f.amount).empty()) f.error = "The amount is not a valid number.";
        if (f.error.empty()) {
            int id = 0;
            if (commit([&](Book& book) { id = book.recordPayment(p); }, &f.error)) {
                notify(std::string(received ? "Received " : "Paid ") + p.amount.formatted() +
                       (received ? " from " : " to ") + book_->contact(p.contactId).name);
                startPayment(kind, 0);
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear form")) startPayment(kind, 0);
}

void App::drawPayments() {
    const Book& b = *book_;
    ListState& st = payments_;
    ui::Heading("Payments");
    static const char* kFilters[] = {"All", "Received", "Paid"};
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine(0, 2);
        const bool active = st.filter == i;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
        if (ImGui::Button(kFilters[i])) st.filter = i;
        if (active) ImGui::PopStyleColor();
    }
    ImGui::SameLine(0, 16);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    ui::InputStringHint("##search", "Search name or reference", st.search);
    ImGui::Dummy(ImVec2(0, 4));

    int voidId = 0;
    if (ImGui::BeginTable("##payments", 9, kListFlags, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3.0f);
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5.0f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn("Account", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Ref", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5.0f);
        ImGui::TableSetupColumn("Applied to", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn("Amount", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3.5f);
        ImGui::TableHeadersRow();
        const auto& all = b.payments();
        for (auto it = all.rbegin(); it != all.rend(); ++it) {
            const Payment& p = *it;
            if ((st.filter == 1 && p.kind != PaymentKind::Received) || (st.filter == 2 && p.kind != PaymentKind::Paid)) continue;
            const std::string name = b.contact(p.contactId).name;
            if (!matches(name + " " + p.ref, st.search)) continue;
            std::string appliedTo;
            for (const auto& a : p.applications) {
                if (!appliedTo.empty()) appliedTo += ", ";
                appliedTo += b.document(a.documentId).number;
            }
            if (!p.voided && !p.unapplied().isZero())
                appliedTo += (appliedTo.empty() ? "" : ", ") + std::string("credit ") + p.unapplied().formatted();
            ImGui::PushID(p.id);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%d", p.id);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(p.date.str().c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(p.kind == PaymentKind::Received ? "Received" : "Paid");
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(name.c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(ui::accountName(b, p.accountId).c_str());
            ImGui::TableSetColumnIndex(5);
            ImGui::TextUnformatted(p.ref.c_str());
            ImGui::TableSetColumnIndex(6);
            ImGui::TextUnformatted(appliedTo.c_str());
            ImGui::TableSetColumnIndex(7);
            if (p.voided) ui::Muted("VOID");
            else ui::MoneyText(p.amount);
            ImGui::TableSetColumnIndex(8);
            if (!p.voided && ImGui::SmallButton("Void")) voidId = p.id;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (voidId) {
        const Payment& p = b.payment(voidId);
        confirm("Void payment #" + std::to_string(voidId) + "?",
                "The " + p.amount.formatted() + " payment " + (p.kind == PaymentKind::Received ? "from " : "to ") +
                    b.contact(p.contactId).name + " will be marked VOID and its invoices or bills reopened.",
                "Void payment", [this, voidId] {
                    commit([voidId](Book& book) { book.voidPayment(voidId); }, nullptr,
                           "Payment #" + std::to_string(voidId) + " voided");
                });
    }
}

}  // namespace obgui

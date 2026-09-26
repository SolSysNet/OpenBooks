// Chart of accounts, products & services, and company settings.

#include "app.hpp"

#include "imgui.h"
#include "openbooks/util.hpp"
#include "theme.hpp"
#include "widgets.hpp"

#include <algorithm>
#include <cfloat>

namespace obgui {

using namespace ob;

namespace {

const ImGuiTableFlags kListFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV |
                                   ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;

const char* kTypeNames[] = {"Asset", "Liability", "Equity", "Income", "Expense"};
const char* kGroupNames[] = {"Assets", "Liabilities", "Equity", "Income", "Expenses"};
const char* kMonths[] = {"January", "February", "March",     "April",   "May",      "June",
                         "July",    "August",   "September", "October", "November", "December"};

bool matches(const std::string& haystack, const std::string& needle) {
    return needle.empty() || toLower(haystack).find(toLower(trim(needle))) != std::string::npos;
}

}  // namespace

// --------------------------------------------------------------- accounts

void App::openAccountForm(int editingId) {
    accountForm_ = AccountForm{};
    accountForm_.editingId = editingId;
    accountForm_.type = static_cast<int>(AccountType::Expense);
    if (editingId) {
        const Account& a = book_->account(editingId);
        accountForm_.name = a.name;
        accountForm_.number = a.number;
        accountForm_.description = a.description;
        accountForm_.type = static_cast<int>(a.type);
        accountForm_.active = a.active;
    }
    requestPopup("Account");
}

void App::drawAccountModal() {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Account", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    AccountForm& f = accountForm_;
    ui::SubHeading(f.editingId ? "Edit account" : "New account");
    const float col = ImGui::GetFontSize() * 6.5f;
    const float width = ImGui::GetFontSize() * 20.0f;
    ui::FormLabel("Type", col);
    ImGui::SetNextItemWidth(width);
    ImGui::BeginDisabled(f.editingId != 0);
    ImGui::Combo("##type", &f.type, kTypeNames, 5);
    ImGui::EndDisabled();
    ui::FormLabel("Name", col);
    ImGui::SetNextItemWidth(width);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ui::InputString("##name", f.name);
    ui::FormLabel("Number", col);
    ImGui::SetNextItemWidth(width * 0.4f);
    ui::InputStringHint("##number", "optional", f.number);
    ui::FormLabel("Description", col);
    ImGui::SetNextItemWidth(width);
    ui::InputString("##desc", f.description);
    if (f.editingId) {
        ImGui::SetCursorPosX(col + ImGui::GetStyle().WindowPadding.x);
        ImGui::Checkbox("Active", &f.active);
    }
    ui::ErrorText(f.error);
    ImGui::Separator();
    if (ui::PrimaryButton("Save")) {
        Account a;
        a.id = f.editingId;
        a.name = f.name;
        a.number = f.number;
        a.description = f.description;
        a.type = static_cast<AccountType>(f.type);
        a.active = f.active;
        if (commit(
                [&](Book& b) {
                    if (a.id) b.updateAccount(a);
                    else b.addAccount(a);
                },
                &f.error, (f.editingId ? "Saved account " : "Added account ") + trim(a.name)))
            ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void App::drawAccounts() {
    const Book& b = *book_;
    const Derived& d = derived();
    ListState& st = accountsList_;

    ui::Heading("Chart of Accounts");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    ui::InputStringHint("##search", "Search accounts", st.search);
    ImGui::SameLine();
    ImGui::Checkbox("Show inactive", &st.showInactive);
    ImGui::SameLine();
    const float w = ImGui::CalcTextSize("New account").x + ImGui::GetStyle().FramePadding.x * 2;
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
    if (ui::PrimaryButton("New account")) openAccountForm(0);
    ImGui::Dummy(ImVec2(0, 4));

    const Company& co = b.company;
    if (ImGui::BeginTable("##accounts", 6, kListFlags, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Number", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4.5f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn("Balance", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8.0f);
        ImGui::TableSetupColumn("Notes", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7.5f);
        ImGui::TableHeadersRow();

        for (int type = 0; type < 5; ++type) {
            std::vector<const Account*> rows;
            for (const auto& a : b.accounts()) {
                if (static_cast<int>(a.type) != type || (!a.active && !st.showInactive)) continue;
                if (!matches(a.number + " " + a.name + " " + a.description, st.search)) continue;
                rows.push_back(&a);
            }
            if (rows.empty()) continue;
            std::stable_sort(rows.begin(), rows.end(), [](const Account* x, const Account* y) {
                if (x->number != y->number) {
                    if (x->number.empty()) return false;
                    if (y->number.empty()) return true;
                    return x->number < y->number;
                }
                return toLower(x->name) < toLower(y->name);
            });
            ImGui::TableNextRow();
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImGuiCol_TableHeaderBg, 0.7f));
            ImGui::TableSetColumnIndex(1);
            ImGui::PushFont(g_fonts.bold, 0.0f);
            ImGui::TextUnformatted(kGroupNames[type]);
            ImGui::PopFont();
            Money groupTotal;
            for (const Account* a : rows) {
                const auto it = d.accountBalance.find(a->id);
                const Money bal = it == d.accountBalance.end() ? Money() : naturalSign(a->type, it->second);
                groupTotal += bal;
                ImGui::PushID(a->id);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable(a->number.c_str(), st.selectedId == a->id,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick |
                                          ImGuiSelectableFlags_AllowOverlap)) {
                    st.selectedId = a->id;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        register_.accountId = a->id;
                        go(Screen::Register);
                    }
                }
                ImGui::TableSetColumnIndex(1);
                if (a->active) ImGui::TextUnformatted(a->name.c_str());
                else ui::Muted((a->name + " (inactive)").c_str());
                ImGui::TableSetColumnIndex(2);
                ui::Muted(a->description.c_str());
                ImGui::TableSetColumnIndex(3);
                ui::MoneyText(bal);
                ImGui::TableSetColumnIndex(4);
                const char* note = a->id == co.receivablesAccountId ? "A/R"
                                   : a->id == co.payablesAccountId  ? "A/P"
                                   : a->id == co.salesTaxAccountId  ? "Sales tax"
                                   : a->id == co.retainedEarningsAccountId ? "Retained earnings"
                                                                            : "";
                ui::Muted(note);
                ImGui::TableSetColumnIndex(5);
                if (ImGui::SmallButton("Register")) {
                    register_.accountId = a->id;
                    go(Screen::Register);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Edit")) openAccountForm(a->id);
                ImGui::PopID();
            }
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            ui::Muted((std::string("Total ") + kGroupNames[type]).c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::PushFont(g_fonts.bold, 0.0f);
            ui::MoneyText(groupTotal);
            ImGui::PopFont();
        }
        ImGui::EndTable();
    }
}

// ------------------------------------------------------------------ items

void App::drawItemModal() {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Item", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ItemForm& f = itemForm_;
    const Book& b = *book_;
    ui::SubHeading("New product or service");
    const float col = ImGui::GetFontSize() * 8.0f;
    const float width = ImGui::GetFontSize() * 20.0f;
    ui::FormLabel("Name", col);
    ImGui::SetNextItemWidth(width);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ui::InputString("##name", f.name);
    ui::FormLabel("Description", col);
    ImGui::SetNextItemWidth(width);
    ui::InputString("##desc", f.description);
    ui::FormLabel("Price / rate", col);
    ui::MoneyField("##price", f.price, ImGui::GetFontSize() * 8.0f);
    ui::FormLabel("Income account", col);
    ui::AccountCombo("##income", b, f.incomeAccountId, [](const Account& a) { return a.type == AccountType::Income; },
                     "(not sold)", width);
    ui::FormLabel("Expense account", col);
    ui::AccountCombo("##expense", b, f.expenseAccountId,
                     [&b](const Account& a) { return a.type == AccountType::Expense || (a.type == AccountType::Asset && !b.isSystemAccount(a.id)); },
                     "(not purchased)", width);
    ImGui::SetCursorPosX(col + ImGui::GetStyle().WindowPadding.x);
    ImGui::Checkbox("Taxable", &f.taxable);
    ui::ErrorText(f.error);
    ImGui::Separator();
    if (ui::PrimaryButton("Save")) {
        Item item;
        item.name = f.name;
        item.description = f.description;
        const auto price = ui::parseMoney(f.price);
        item.price = price.value_or(Money());
        item.incomeAccountId = f.incomeAccountId;
        item.expenseAccountId = f.expenseAccountId;
        item.taxable = f.taxable;
        if (!trim(f.price).empty() && !price) f.error = "The price is not a valid amount.";
        else if (commit([&](Book& book) { book.addItem(item); }, &f.error, "Added " + trim(item.name)))
            ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void App::drawItems() {
    const Book& b = *book_;
    ListState& st = itemsList_;
    ui::Heading("Products & Services");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    ui::InputStringHint("##search", "Search", st.search);
    ImGui::SameLine();
    ImGui::Checkbox("Show inactive", &st.showInactive);
    ImGui::SameLine();
    const float w = ImGui::CalcTextSize("New item").x + ImGui::GetStyle().FramePadding.x * 2;
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
    if (ui::PrimaryButton("New item")) {
        itemForm_ = ItemForm{};
        for (const auto& a : b.accounts()) {
            if (a.active && iequals(a.name, "Service Revenue")) itemForm_.incomeAccountId = a.id;
        }
        requestPopup("Item");
    }
    ImGui::Dummy(ImVec2(0, 4));

    int toggleId = 0;
    if (ImGui::BeginTable("##items", 7, kListFlags, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Price", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.5f);
        ImGui::TableSetupColumn("Income account", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Expense account", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Taxable", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3.5f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5.5f);
        ImGui::TableHeadersRow();
        for (const auto& i : b.items()) {
            if ((!i.active && !st.showInactive) || !matches(i.name + " " + i.description, st.search)) continue;
            ImGui::PushID(i.id);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            if (i.active) ImGui::TextUnformatted(i.name.c_str());
            else ui::Muted((i.name + " (inactive)").c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(i.description.c_str());
            ImGui::TableSetColumnIndex(2);
            ui::MoneyText(i.price);
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(ui::accountName(b, i.incomeAccountId).c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(ui::accountName(b, i.expenseAccountId).c_str());
            ImGui::TableSetColumnIndex(5);
            ImGui::TextUnformatted(i.taxable ? "Yes" : "No");
            ImGui::TableSetColumnIndex(6);
            if (ImGui::SmallButton(i.active ? "Deactivate" : "Activate")) toggleId = i.id;
            ImGui::PopID();
        }
        if (b.items().empty()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            ui::Muted("Add the products and services you sell to fill in invoice lines automatically.");
        }
        ImGui::EndTable();
    }
    if (toggleId) {
        const bool active = !b.item(toggleId).active;
        commit([toggleId, active](Book& book) { book.setItemActive(toggleId, active); }, nullptr,
               active ? "Item activated" : "Item deactivated");
    }
}

// ---------------------------------------------------------------- company

void App::drawCompany() {
    const Book& b = *book_;
    CompanyForm& f = company_;
    if (!f.loaded) {
        const Company& c = b.company;
        f = CompanyForm{};
        f.loaded = true;
        f.name = c.name;
        f.address = c.address;
        f.email = c.email;
        f.phone = c.phone;
        f.invoiceFooter = c.invoiceFooter;
        f.paperSize = c.paperSize == PaperSize::A4 ? 1 : 0;
        f.fiscalYearStartMonth = c.fiscalYearStartMonth;
        f.terms = std::to_string(c.defaultTermsDays);
        f.nextInvoice = std::to_string(c.nextInvoiceNumber);
        f.closed = c.closedThrough.has_value();
        f.closedThrough = c.closedThrough.value_or(Date::fromYMD(Date::today().year() - 1, 12, 31));
        f.receivables = c.receivablesAccountId;
        f.payables = c.payablesAccountId;
        f.salesTax = c.salesTaxAccountId;
        f.retainedEarnings = c.retainedEarningsAccountId;
    }

    ui::Heading("Company Settings");
    ui::Muted(("Books file: " + path_).c_str());
    ImGui::Dummy(ImVec2(0, 6));
    const float col = ImGui::GetFontSize() * 11.0f;
    const float width = ImGui::GetFontSize() * 22.0f;

    ui::SubHeading("Company");
    ui::FormLabel("Company name", col);
    ImGui::SetNextItemWidth(width);
    ui::InputString("##name", f.name);
    ui::FormLabel("Address", col);
    ui::InputMultiline("##address", f.address, ImVec2(width, ImGui::GetTextLineHeight() * 3.6f));
    ui::FormLabel("Email", col);
    ImGui::SetNextItemWidth(width);
    ui::InputString("##email", f.email);
    ui::FormLabel("Phone", col);
    ImGui::SetNextItemWidth(width);
    ui::InputString("##phone", f.phone);

    ImGui::Dummy(ImVec2(0, 6));
    ui::SubHeading("Invoices");
    ui::FormLabel("Invoice footer", col);
    ui::InputMultiline("##footer", f.invoiceFooter, ImVec2(width, ImGui::GetTextLineHeight() * 3.6f));
    ImGui::SetCursorPosX(col + ImGui::GetStyle().WindowPadding.x);
    ui::Muted("Printed at the bottom of every PDF, e.g. payment instructions.");
    ui::FormLabel("Paper size", col);
    ImGui::SetNextItemWidth(width * 0.4f);
    static const char* kPaper[] = {"Letter", "A4"};
    ImGui::Combo("##paper", &f.paperSize, kPaper, 2);

    ImGui::Dummy(ImVec2(0, 6));
    ui::SubHeading("Accounting");
    ui::FormLabel("Fiscal year starts", col);
    int month = f.fiscalYearStartMonth - 1;
    ImGui::SetNextItemWidth(width * 0.5f);
    if (ImGui::Combo("##fy", &month, kMonths, 12)) f.fiscalYearStartMonth = month + 1;
    ui::FormLabel("Default terms (days)", col);
    ImGui::SetNextItemWidth(width * 0.25f);
    ui::InputString("##terms", f.terms, ImGuiInputTextFlags_CharsDecimal);
    ui::FormLabel("Next invoice number", col);
    ImGui::SetNextItemWidth(width * 0.25f);
    ui::InputString("##next", f.nextInvoice, ImGuiInputTextFlags_CharsDecimal);
    ui::FormLabel("Close the books", col);
    ImGui::Checkbox("##closed", &f.closed);
    if (f.closed) {
        ImGui::SameLine();
        ImGui::TextUnformatted("through");
        ImGui::SameLine();
        ui::DateField("##closedThrough", f.closedThrough);
    }
    ImGui::SetCursorPosX(col + ImGui::GetStyle().WindowPadding.x);
    ui::Muted("Nothing dated on or before the closing date can be added, changed or voided.");

    ImGui::Dummy(ImVec2(0, 6));
    ui::SubHeading("System accounts");
    const auto ofType = [](AccountType t) { return [t](const Account& a) { return a.type == t; }; };
    ui::FormLabel("Accounts receivable", col);
    ui::AccountCombo("##ar", b, f.receivables, ofType(AccountType::Asset), nullptr, width);
    ui::FormLabel("Accounts payable", col);
    ui::AccountCombo("##ap", b, f.payables, ofType(AccountType::Liability), nullptr, width);
    ui::FormLabel("Sales tax payable", col);
    ui::AccountCombo("##tax", b, f.salesTax, ofType(AccountType::Liability), nullptr, width);
    ui::FormLabel("Retained earnings", col);
    ui::AccountCombo("##re", b, f.retainedEarnings, ofType(AccountType::Equity), nullptr, width);

    ImGui::Dummy(ImVec2(0, 8));
    ui::ErrorText(f.error);
    if (ui::PrimaryButton("Save settings")) {
        const auto terms = parseInt(f.terms);
        const auto next = parseInt(f.nextInvoice);
        if (trim(f.name).empty()) f.error = "Company name is required.";
        else if (!terms || *terms < 0 || *terms > 3650) f.error = "Default terms must be a number of days.";
        else if (!next || *next < 1 || *next > 2000000000) f.error = "Next invoice number must be a positive number.";
        else {
            const CompanyForm copy = f;
            if (commit(
                    [&copy, terms, next](Book& book) {
                        Company& c = book.company;
                        c.name = trim(copy.name);
                        c.address = copy.address;
                        c.email = trim(copy.email);
                        c.phone = trim(copy.phone);
                        c.invoiceFooter = copy.invoiceFooter;
                        c.paperSize = copy.paperSize == 1 ? PaperSize::A4 : PaperSize::Letter;
                        c.fiscalYearStartMonth = copy.fiscalYearStartMonth;
                        c.defaultTermsDays = static_cast<int>(*terms);
                        c.nextInvoiceNumber = static_cast<int>(*next);
                        if (copy.closed) c.closedThrough = copy.closedThrough;
                        else c.closedThrough.reset();
                        if (copy.receivables) book.setSystemAccount(SystemAccount::Receivables, copy.receivables);
                        if (copy.payables) book.setSystemAccount(SystemAccount::Payables, copy.payables);
                        if (copy.salesTax) book.setSystemAccount(SystemAccount::SalesTax, copy.salesTax);
                        if (copy.retainedEarnings)
                            book.setSystemAccount(SystemAccount::RetainedEarnings, copy.retainedEarnings);
                    },
                    &f.error, "Company settings saved"))
                f.loaded = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert")) f.loaded = false;
}

}  // namespace obgui

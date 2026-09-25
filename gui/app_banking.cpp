// Account registers, quick entries, CSV import, reconciliation and journal entries.

#include "app.hpp"

#include "imgui.h"
#include "openbooks/import.hpp"
#include "openbooks/util.hpp"
#include "platform.hpp"
#include "theme.hpp"
#include "widgets.hpp"

#include <algorithm>
#include <cfloat>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace obgui {

using namespace ob;

namespace {

const ImGuiTableFlags kListFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV |
                                   ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;

std::string offsetName(const Book& b, const Transaction& t, int accountId) {
    std::set<int> others;
    for (const auto& s : t.splits) {
        if (s.accountId != accountId) others.insert(s.accountId);
    }
    if (others.empty()) return {};
    if (others.size() > 1) return "-split-";
    return ui::accountName(b, *others.begin());
}

std::string payeeMemo(const Transaction& t) {
    if (t.payee.empty()) return t.memo;
    if (t.memo.empty()) return t.payee;
    return t.payee + " - " + t.memo;
}

int firstMoneyAccount(const Book& b) {
    for (const auto& a : b.accounts()) {
        if (a.active && ui::isMoneyAccount(b, a) && iequals(a.name, "Checking")) return a.id;
    }
    for (const auto& a : b.accounts()) {
        if (a.active && ui::isMoneyAccount(b, a)) return a.id;
    }
    return 0;
}

const char* clearedMark(SplitState s) {
    switch (s) {
        case SplitState::Cleared: return "c";
        case SplitState::Reconciled: return "R";
        default: return "";
    }
}

}  // namespace

// --------------------------------------------------------------- register

void App::drawRegister() {
    const Book& b = *book_;
    const Derived& d = derived();
    RegisterState& st = register_;
    if (st.accountId == 0) st.accountId = firstMoneyAccount(b);
    if (st.from == Date()) {
        st.to = Date::today();
        st.from = Date::fromYMD(st.to.year(), st.to.month(), 1);
    }

    ui::Heading("Account Register");
    const int previous = st.accountId;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Account");
    ImGui::SameLine();
    ui::AccountCombo("##account", b, st.accountId, {}, nullptr, ImGui::GetFontSize() * 18.0f);
    static bool scrollToBottom = true;
    if (previous != st.accountId) scrollToBottom = true;
    ImGui::SameLine(0, 16);
    ImGui::Checkbox("Date range", &st.limitDates);
    if (st.limitDates) {
        ImGui::SameLine();
        ui::DateField("##from", st.from);
        ImGui::SameLine();
        ImGui::TextUnformatted("to");
        ImGui::SameLine();
        ui::DateField("##to", st.to);
    }

    ImGui::Dummy(ImVec2(0, 2));
    if (ui::PrimaryButton("Record expense")) openQuickEntry(TxnKind::Expense);
    ImGui::SameLine();
    if (ImGui::Button("Record deposit")) openQuickEntry(TxnKind::Deposit);
    ImGui::SameLine();
    if (ImGui::Button("Transfer")) openQuickEntry(TxnKind::Transfer);
    ImGui::SameLine();
    if (ImGui::Button("Import statement (CSV)...")) {
        import_.error.clear();
        if (ui::isMoneyAccount(b, b.account(st.accountId))) import_.accountId = st.accountId;
        requestPopup("Import Bank Statement");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reconcile")) {
        reconcile_.accountId = st.accountId;
        go(Screen::Reconcile);
    }
    if (st.accountId == 0) {
        ui::Muted("Choose an account.");
        return;
    }
    const Account& account = b.account(st.accountId);

    struct Line {
        int txnId;
        Money amount;
        Money balance;
        SplitState state;
    };
    std::vector<Line> lines;
    Money running;
    if (st.limitDates) running = naturalSign(account.type, b.balance(st.accountId, Period{std::nullopt, st.from.addDays(-1)}));
    const Money opening = running;
    Money cleared;
    for (int id : d.transactionsByDate) {
        const Transaction& t = b.transaction(id);
        if (t.voided) continue;
        for (const auto& s : t.splits) {
            if (s.accountId != st.accountId) continue;
            const Money amount = naturalSign(account.type, s.amount);
            if (s.state != SplitState::Uncleared) cleared += amount;
            if (st.limitDates && (t.date < st.from || t.date > st.to)) continue;
            running += amount;
            lines.push_back(Line{t.id, amount, running, s.state});
        }
    }

    ImGui::Dummy(ImVec2(0, 4));
    if (ImGui::BeginTable("##register", 9, kListFlags, ImVec2(0, -ImGui::GetFrameHeightWithSpacing()))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.0f);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.0f);
        ImGui::TableSetupColumn("Ref", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5.0f);
        ImGui::TableSetupColumn("Payee / Memo", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Account", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Amount", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7.0f);
        ImGui::TableSetupColumn("Balance", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7.5f);
        ImGui::TableSetupColumn("Clr", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 1.8f);
        ImGui::TableHeadersRow();
        if (st.limitDates) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(st.from.str().c_str());
            ImGui::TableSetColumnIndex(4);
            ui::Muted("Opening balance");
            ImGui::TableSetColumnIndex(7);
            ui::MoneyText(opening);
        }
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(lines.size()));
        if (scrollToBottom && !lines.empty()) clipper.IncludeItemByIndex(static_cast<int>(lines.size()) - 1);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const Line& line = lines[static_cast<std::size_t>(i)];
                const Transaction& t = b.transaction(line.txnId);
                ImGui::PushID(line.txnId);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable(t.date.str().c_str(), st.selectedTxn == t.id,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    st.selectedTxn = t.id;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        detailTxn_ = t.id;
                        requestPopup("Transaction");
                    }
                }
                if (ImGui::BeginPopupContextItem("##ctx")) {
                    st.selectedTxn = t.id;
                    transactionContextMenu(t.id, st.accountId);
                    ImGui::EndPopup();
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%d", t.id);
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(toString(t.kind));
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(t.ref.c_str());
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(payeeMemo(t).c_str());
                ImGui::TableSetColumnIndex(5);
                const std::string offset = offsetName(b, t, st.accountId);
                if (offset.rfind("Uncategorized", 0) == 0) ImGui::TextColored(colorWarning(), "%s", offset.c_str());
                else ImGui::TextUnformatted(offset.c_str());
                ImGui::TableSetColumnIndex(6);
                ui::MoneyText(line.amount);
                ImGui::TableSetColumnIndex(7);
                ui::MoneyText(line.balance);
                ImGui::TableSetColumnIndex(8);
                ImGui::TextUnformatted(clearedMark(line.state));
                ImGui::PopID();
            }
        }
        if (scrollToBottom) {
            ImGui::SetScrollY(ImGui::GetScrollMaxY() + 10000.0f);
            scrollToBottom = false;
        }
        if (lines.empty()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(4);
            ui::Muted("No transactions in this account yet.");
        }
        ImGui::EndTable();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Ending balance: %s", running.formatted().c_str());
    ImGui::SameLine(0, 24);
    ui::Muted(("Cleared balance: " + cleared.formatted() + "    Right-click a row for more actions.").c_str());
}

void App::transactionContextMenu(int txnId, int accountId) {
    const Book& b = *book_;
    const Transaction t = b.transaction(txnId);
    if (ImGui::MenuItem("View details")) {
        detailTxn_ = txnId;
        requestPopup("Transaction");
    }
    const bool manual = isManualKind(t.kind);
    if (!manual) {
        for (const auto& doc : b.documents()) {
            if (doc.txnId != txnId) continue;
            const bool invoice = doc.kind == DocKind::Invoice;
            if (ImGui::MenuItem(invoice ? "Open invoice" : "Open bill")) {
                (invoice ? invoices_ : bills_).selectedId = doc.id;
                go(invoice ? Screen::Invoices : Screen::Bills);
            }
        }
    }
    if (ImGui::MenuItem("Change account...", nullptr, false, manual)) {
        recategorize_ = RecategorizeState{};
        recategorize_.txnId = txnId;
        for (const auto& s : t.splits) {
            if (s.accountId != accountId) {
                recategorize_.fromAccountId = s.accountId;
                break;
            }
        }
        requestPopup("Change Account");
    }
    if (!manual) ImGui::SetItemTooltip("Created by an invoice, bill or payment; edit that instead.");
    ImGui::Separator();
    if (ImGui::MenuItem("Void...", nullptr, false, manual)) {
        confirm("Void transaction #" + std::to_string(txnId) + "?",
                "It stays on file marked VOID and no longer affects any balance.", "Void",
                [this, txnId] {
                    commit([txnId](Book& book) { book.voidTransaction(txnId); }, nullptr,
                           "Transaction #" + std::to_string(txnId) + " voided");
                });
    }
}

void App::drawTransactionModal() {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 36.0f, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Transaction", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    try {
        const Table t = transactionDetail(*book_, detailTxn_);
        ui::SubHeading(t.title.c_str());
        for (const auto& s : t.subtitles) ImGui::TextUnformatted(s.c_str());
        ImGui::Dummy(ImVec2(0, 4));
        ui::ReportTable("##detail", t, ImGui::GetFrameHeightWithSpacing() * (static_cast<float>(t.rows.size()) + 1.6f));
    } catch (const std::exception& e) {
        ui::ErrorText(e.what());
    }
    if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void App::drawRecategorizeModal() {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Change Account", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    RecategorizeState& r = recategorize_;
    const Book& b = *book_;
    const Transaction& t = b.transaction(r.txnId);
    ui::SubHeading(("Transaction #" + std::to_string(t.id)).c_str());
    ImGui::Text("%s   %s   %s", t.date.str().c_str(), payeeMemo(t).c_str(), t.debitTotal().formatted().c_str());
    ImGui::Dummy(ImVec2(0, 4));
    const float col = ImGui::GetFontSize() * 6.0f;
    const float width = ImGui::GetFontSize() * 18.0f;
    std::vector<ui::Option> fromOptions;
    std::string fromPreview;
    for (const auto& s : t.splits) {
        const std::string name = ui::accountName(b, s.accountId);
        if (std::none_of(fromOptions.begin(), fromOptions.end(), [&](const ui::Option& o) { return o.id == s.accountId; }))
            fromOptions.push_back({s.accountId, name, false});
        if (s.accountId == r.fromAccountId) fromPreview = name;
    }
    ui::FormLabel("Move line in", col);
    ui::SearchCombo("##from", fromPreview, fromOptions, r.fromAccountId, width);
    ui::FormLabel("To account", col);
    ui::AccountCombo("##to", b, r.toAccountId, [&b](const Account& a) { return !b.isSubledgerAccount(a.id); }, nullptr, width);
    ui::ErrorText(r.error);
    ImGui::Separator();
    if (ui::PrimaryButton("Save")) {
        if (r.toAccountId == 0) {
            r.error = "Choose the account to move the line to.";
        } else {
            const int txn = r.txnId, from = r.fromAccountId, to = r.toAccountId;
            if (commit([=](Book& book) { book.recategorize(txn, from, to); }, &r.error,
                       "Moved to " + ui::accountName(b, to)))
                ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ------------------------------------------------------------ quick entry

void App::openQuickEntry(TxnKind kind) {
    quick_ = QuickEntryState{};
    quick_.kind = kind;
    quick_.date = Date::today();
    const Book& b = *book_;
    quick_.moneyAccountId = register_.accountId && ui::isMoneyAccount(b, b.account(register_.accountId))
                                ? register_.accountId
                                : firstMoneyAccount(b);
    requestPopup("Record Transaction");
}

void App::drawQuickEntryModal() {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Record Transaction", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    QuickEntryState& q = quick_;
    const Book& b = *book_;
    const float col = ImGui::GetFontSize() * 6.5f;
    const float width = ImGui::GetFontSize() * 20.0f;

    int kindIndex = q.kind == TxnKind::Expense ? 0 : q.kind == TxnKind::Deposit ? 1 : 2;
    ImGui::RadioButton("Expense", &kindIndex, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Deposit", &kindIndex, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Transfer", &kindIndex, 2);
    const TxnKind kinds[] = {TxnKind::Expense, TxnKind::Deposit, TxnKind::Transfer};
    if (kinds[kindIndex] != q.kind) {
        q.kind = kinds[kindIndex];
        q.otherAccountId = 0;
    }
    ImGui::Separator();

    const ui::AccountFilter money = [&b](const Account& a) { return ui::isMoneyAccount(b, a); };
    const ui::AccountFilter category = [&b](const Account& a) { return !b.isSubledgerAccount(a.id); };
    ui::FormLabel("Date", col);
    ui::DateField("##date", q.date);
    if (q.kind == TxnKind::Expense) {
        ui::FormLabel("Paid from", col);
        ui::AccountCombo("##money", b, q.moneyAccountId, money, nullptr, width);
        ui::FormLabel("Category", col);
        ui::AccountCombo("##other", b, q.otherAccountId, category, nullptr, width);
        ui::FormLabel("Payee", col);
        ImGui::SetNextItemWidth(width);
        ui::InputString("##payee", q.payee);
    } else if (q.kind == TxnKind::Deposit) {
        ui::FormLabel("Deposit to", col);
        ui::AccountCombo("##money", b, q.moneyAccountId, money, nullptr, width);
        ui::FormLabel("Category", col);
        ui::AccountCombo("##other", b, q.otherAccountId, category, nullptr, width);
        ui::FormLabel("Received from", col);
        ImGui::SetNextItemWidth(width);
        ui::InputString("##payee", q.payee);
    } else {
        ui::FormLabel("From", col);
        ui::AccountCombo("##money", b, q.moneyAccountId, money, nullptr, width);
        ui::FormLabel("To", col);
        ui::AccountCombo("##other", b, q.otherAccountId, money, nullptr, width);
    }
    ui::FormLabel("Amount", col);
    ui::MoneyField("##amount", q.amount, ImGui::GetFontSize() * 8.0f);
    ui::FormLabel("Memo", col);
    ImGui::SetNextItemWidth(width);
    ui::InputString("##memo", q.memo);
    ui::FormLabel("Ref / check #", col);
    ImGui::SetNextItemWidth(width * 0.5f);
    ui::InputString("##ref", q.ref);

    ui::ErrorText(q.error);
    ImGui::Separator();
    if (ui::PrimaryButton("Save")) {
        const auto amount = ui::parseMoney(q.amount);
        if (!amount || *amount <= Money()) q.error = "Enter an amount greater than zero.";
        else if (!q.moneyAccountId || !q.otherAccountId) q.error = "Choose both accounts.";
        else {
            Transaction t;
            t.kind = q.kind;
            t.date = q.date;
            t.payee = q.payee;
            t.memo = q.memo;
            t.ref = q.ref;
            // Expense: money leaves the bank. Deposit: money enters it. Transfer: from -> to.
            const bool intoMoney = q.kind == TxnKind::Deposit;
            const int debit = intoMoney ? q.moneyAccountId : q.otherAccountId;
            const int credit = intoMoney ? q.otherAccountId : q.moneyAccountId;
            t.splits = {Split{debit, *amount, "", SplitState::Uncleared}, Split{credit, -*amount, "", SplitState::Uncleared}};
            if (commit([&](Book& book) { book.postTransaction(t); }, &q.error,
                       std::string("Recorded ") + toLower(toString(q.kind)) + " of " + amount->formatted())) {
                register_.accountId = q.moneyAccountId;
                ImGui::CloseCurrentPopup();
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ----------------------------------------------------------------- import

void App::drawImportModal() {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Import Bank Statement", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImportState& s = import_;
    const Book& b = *book_;
    if (s.accountId == 0) s.accountId = firstMoneyAccount(b);
    const float col = ImGui::GetFontSize() * 7.0f;
    const float width = ImGui::GetFontSize() * 24.0f;

    ImGui::TextUnformatted("Import a CSV statement exported from your bank or card.");
    ui::Muted("Positive amounts are money into the account. Rows already imported are skipped.");
    ImGui::Dummy(ImVec2(0, 4));

    auto load = [&] {
        s.error.clear();
        s.rows.clear();
        s.text.clear();
        s.loadedPath = s.path;
        std::ifstream in(std::filesystem::u8path(trim(s.path)), std::ios::binary);
        if (!in) {
            s.error = "Cannot open that file.";
            return;
        }
        std::stringstream buffer;
        buffer << in.rdbuf();
        s.text = buffer.str();
        s.rows = parseCsv(s.text);
    };
    ui::FormLabel("CSV file", col);
    ImGui::SetNextItemWidth(width);
    if (ui::InputString("##path", s.path, ImGuiInputTextFlags_EnterReturnsTrue)) load();
    ImGui::SameLine();
    if (nativeFileDialogsAvailable()) {
        if (ImGui::Button("Browse...")) {
            if (auto p = openFileDialog("Choose statement", {"CSV files (*.csv)", "*.csv"})) {
                s.path = *p;
                load();
            }
        }
    } else if (ImGui::Button("Load")) {
        load();
    }
    ui::FormLabel("Into account", col);
    ui::AccountCombo("##account", b, s.accountId, [&b](const Account& a) { return ui::isMoneyAccount(b, a); }, nullptr, width);
    ui::FormLabel("Columns", col);
    auto columnInput = [](const char* label, int& value) {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.5f);
        ImGui::InputInt(label, &value);
        value = std::clamp(value, 1, 99);
    };
    columnInput("Date##c", s.dateColumn);
    ImGui::SameLine();
    columnInput("Description##c", s.descriptionColumn);
    ImGui::SameLine();
    columnInput("Amount##c", s.amountColumn);
    ImGui::SetCursorPosX(col + ImGui::GetStyle().WindowPadding.x);
    ImGui::Checkbox("First row is a header", &s.hasHeader);
    ImGui::SameLine();
    ImGui::Checkbox("Flip signs", &s.negate);
    ImGui::SetItemTooltip("Use this if your bank exports deposits as negative numbers.");
    ui::FormLabel("Categorize as", col);
    ui::AccountCombo("##offset", b, s.offsetAccountId, [&b](const Account& a) { return !b.isSubledgerAccount(a.id); },
                     "Uncategorized income / expense", width);

    if (!s.rows.empty()) {
        ImGui::Dummy(ImVec2(0, 4));
        ui::Muted("Preview");
        const float h = ImGui::GetTextLineHeightWithSpacing() * 9.0f;
        if (ImGui::BeginTable("##preview", 3, kListFlags, ImVec2(width + col, h))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.0f);
            ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Amount", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.0f);
            ImGui::TableHeadersRow();
            auto cell = [](const std::vector<std::string>& row, int col1) -> std::string {
                const auto i = static_cast<std::size_t>(col1 - 1);
                return i < row.size() ? row[i] : std::string();
            };
            int shown = 0;
            for (std::size_t i = s.hasHeader ? 1 : 0; i < s.rows.size() && shown < 50; ++i, ++shown) {
                const auto& row = s.rows[i];
                if (row.size() == 1 && trim(row[0]).empty()) continue;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                const std::string date = cell(row, s.dateColumn);
                if (Date::parse(date)) ImGui::TextUnformatted(date.c_str());
                else ImGui::TextColored(colorNegative(), "%s", date.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(cell(row, s.descriptionColumn).c_str());
                ImGui::TableSetColumnIndex(2);
                const auto amount = Money::parse(cell(row, s.amountColumn));
                if (amount) ui::MoneyText(s.negate ? -*amount : *amount);
                else ImGui::TextColored(colorNegative(), "%s", cell(row, s.amountColumn).c_str());
            }
            ImGui::EndTable();
        }
    }

    ui::ErrorText(s.error);
    ImGui::Separator();
    ImGui::BeginDisabled(s.rows.empty() || s.accountId == 0);
    if (ui::PrimaryButton("Import")) {
        CsvImportOptions options;
        options.dateColumn = s.dateColumn;
        options.descriptionColumn = s.descriptionColumn;
        options.amountColumn = s.amountColumn;
        options.hasHeader = s.hasHeader;
        options.negate = s.negate;
        if (s.offsetAccountId) options.offsetAccountId = s.offsetAccountId;
        CsvImportResult result;
        const int accountId = s.accountId;
        const std::string text = s.text;
        if (commit([&](Book& book) { result = importCsv(book, accountId, text, options); }, &s.error)) {
            notify("Imported " + std::to_string(result.imported) + " transaction(s), skipped " +
                   std::to_string(result.skipped) + " duplicate(s)");
            register_.accountId = accountId;
            go(Screen::Register);
            s.rows.clear();
            s.path.clear();
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// -------------------------------------------------------------- reconcile

void App::drawReconcile() {
    const Book& b = *book_;
    ReconcileState& st = reconcile_;
    if (st.accountId == 0) st.accountId = firstMoneyAccount(b);
    if (st.through == Date()) st.through = Date::today();

    ui::Heading("Reconcile");
    ui::Muted("Tick each transaction that appears on your statement until the difference is zero.");
    ImGui::Dummy(ImVec2(0, 4));
    const float col = ImGui::GetFontSize() * 9.0f;
    ui::FormLabel("Account", col);
    ui::AccountCombo("##account", b, st.accountId, [&b](const Account& a) { return ui::isMoneyAccount(b, a); }, nullptr,
                     ImGui::GetFontSize() * 18.0f);
    ui::FormLabel("Statement date", col);
    ui::DateField("##through", st.through);
    ui::FormLabel("Statement balance", col);
    ui::MoneyField("##balance", st.statementBalance, ImGui::GetFontSize() * 8.0f);
    if (st.accountId == 0) return;
    const Account& account = b.account(st.accountId);

    struct Item {
        int txnId;
        Date date;
        std::string text;
        Money amount;
        bool cleared;
    };
    std::vector<Item> items;
    Money reconciled;
    Money cleared;
    for (int id : derived().transactionsByDate) {
        const Transaction& t = b.transaction(id);
        if (t.voided) continue;
        Money amount;
        bool involved = false;
        bool isCleared = false;
        bool isReconciled = false;
        for (const auto& s : t.splits) {
            if (s.accountId != st.accountId) continue;
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
        if (t.date <= st.through || isCleared) items.push_back(Item{t.id, t.date, payeeMemo(t), amount, isCleared});
    }

    std::vector<std::pair<int, bool>> toggles;
    ImGui::Dummy(ImVec2(0, 4));
    if (ImGui::SmallButton("Mark all")) {
        for (const auto& i : items) {
            if (!i.cleared) toggles.emplace_back(i.txnId, true);
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Unmark all")) {
        for (const auto& i : items) {
            if (i.cleared) toggles.emplace_back(i.txnId, false);
        }
    }
    const float footer = ImGui::GetFrameHeightWithSpacing() * 4.5f;
    if (ImGui::BeginTable("##recon", 5, kListFlags, ImVec2(0, -footer))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Cleared", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4.0f);
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.0f);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3.0f);
        ImGui::TableSetupColumn("Payee / Memo", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Amount", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8.0f);
        ImGui::TableHeadersRow();
        for (const auto& i : items) {
            ImGui::PushID(i.txnId);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            bool checked = i.cleared;
            if (ImGui::Checkbox("##c", &checked)) toggles.emplace_back(i.txnId, checked);
            ImGui::TableSetColumnIndex(1);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(i.date.str().c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%d", i.txnId);
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(i.text.c_str());
            ImGui::TableSetColumnIndex(4);
            ui::MoneyText(i.amount);
            ImGui::PopID();
        }
        if (items.empty()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(3);
            ui::Muted("Nothing left to reconcile through this date.");
        }
        ImGui::EndTable();
    }

    const Money clearedBalance = reconciled + cleared;
    const auto statement = ui::parseMoney(st.statementBalance);
    ImGui::Text("Previously reconciled: %s", reconciled.formatted().c_str());
    ImGui::Text("Cleared balance:        %s", clearedBalance.formatted().c_str());
    if (statement) {
        const Money diff = *statement - clearedBalance;
        ImGui::Text("Difference:");
        ImGui::SameLine();
        ImGui::PushFont(g_fonts.bold, 0.0f);
        ImGui::TextColored(diff.isZero() ? colorPositive() : colorNegative(), "%s", diff.formatted().c_str());
        ImGui::PopFont();
        const bool nothingCleared = std::none_of(items.begin(), items.end(), [](const Item& i) { return i.cleared; });
        ImGui::BeginDisabled(!diff.isZero() || nothingCleared);
        if (ui::PrimaryButton("Finish reconciliation")) {
            const int id = st.accountId;
            const std::string name = account.name;  // `account` dangles once the books are replaced
            int count = 0;
            if (commit([&](Book& book) { count = book.finishReconciliation(id); }, &st.error)) {
                notify(name + " reconciled (" + std::to_string(count) + " transactions)");
                st.statementBalance.clear();
                toggles.clear();
            }
        }
        ImGui::EndDisabled();
    } else {
        ui::Muted("Enter the ending balance from your statement.");
    }
    ui::ErrorText(st.error);

    if (!toggles.empty()) {
        const int id = st.accountId;
        commit(
            [&](Book& book) {
                for (const auto& [txn, on] : toggles)
                    book.setSplitState(txn, id, on ? SplitState::Cleared : SplitState::Uncleared);
            },
            &st.error);
    }
}

// ---------------------------------------------------------------- journal

void App::drawJournal() {
    const Book& b = *book_;
    JournalState& j = journal_;
    if (j.date == Date()) j.date = Date::today();
    if (j.lines.size() < 2) j.lines.resize(2);

    ui::Heading("Journal Entry");
    ui::Muted("For adjustments, opening balances, depreciation and anything without its own form.");
    ImGui::Dummy(ImVec2(0, 4));
    const float col = ImGui::GetFontSize() * 4.5f;
    ui::FormLabel("Date", col);
    ui::DateField("##date", j.date);
    ImGui::SameLine(0, 24);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Ref");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
    ui::InputString("##ref", j.ref);
    ui::FormLabel("Memo", col);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 28.0f);
    ui::InputString("##memo", j.memo);
    ImGui::Dummy(ImVec2(0, 4));

    int remove = -1;
    Money debits;
    Money credits;
    bool invalid = false;
    if (ImGui::BeginTable("##lines", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Account", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Debit", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7.0f);
        ImGui::TableSetupColumn("Credit", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7.0f);
        ImGui::TableSetupColumn("Line memo", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < j.lines.size(); ++i) {
            JournalLineDraft& l = j.lines[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ui::AccountCombo("##acct", b, l.accountId, [&b](const Account& a) { return !b.isSubledgerAccount(a.id); },
                             nullptr, -FLT_MIN);
            ImGui::TableSetColumnIndex(1);
            if (ui::MoneyField("##dr", l.debit, -FLT_MIN) && !trim(l.debit).empty()) l.credit.clear();
            ImGui::TableSetColumnIndex(2);
            if (ui::MoneyField("##cr", l.credit, -FLT_MIN) && !trim(l.credit).empty()) l.debit.clear();
            ImGui::TableSetColumnIndex(3);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ui::InputString("##memo", l.memo);
            ImGui::TableSetColumnIndex(4);
            if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0))) remove = static_cast<int>(i);
            ImGui::PopID();
            const auto dr = ui::parseMoney(l.debit);
            const auto cr = ui::parseMoney(l.credit);
            if ((!trim(l.debit).empty() && !dr) || (!trim(l.credit).empty() && !cr)) invalid = true;
            if (dr) debits += *dr;
            if (cr) credits += *cr;
        }
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::PushFont(g_fonts.bold, 0.0f);
        ui::TextRight("Totals");
        ImGui::TableSetColumnIndex(1);
        ui::MoneyText(debits);
        ImGui::TableSetColumnIndex(2);
        ui::MoneyText(credits);
        ImGui::PopFont();
        ImGui::EndTable();
    }
    if (remove >= 0 && j.lines.size() > 2) j.lines.erase(j.lines.begin() + remove);
    if (ImGui::Button("+ Add line")) j.lines.emplace_back();

    const Money difference = debits - credits;
    ImGui::SameLine(0, 24);
    ImGui::AlignTextToFramePadding();
    if (difference.isZero() && !debits.isZero()) ImGui::TextColored(colorPositive(), "Balanced");
    else if (!difference.isZero()) ImGui::TextColored(colorNegative(), "Out of balance by %s", difference.formatted().c_str());

    ImGui::Dummy(ImVec2(0, 6));
    ui::ErrorText(j.error);
    ImGui::BeginDisabled(invalid || !difference.isZero() || debits.isZero());
    if (ui::PrimaryButton("Post entry")) {
        Transaction t;
        t.kind = TxnKind::Journal;
        t.date = j.date;
        t.ref = j.ref;
        t.memo = j.memo;
        j.error.clear();
        for (const auto& l : j.lines) {
            const Money amount = ui::parseMoney(l.debit).value_or(Money()) - ui::parseMoney(l.credit).value_or(Money());
            if (amount.isZero()) continue;
            if (l.accountId == 0) {
                j.error = "Every line with an amount needs an account.";
                break;
            }
            t.splits.push_back(Split{l.accountId, amount, l.memo, SplitState::Uncleared});
        }
        int id = 0;
        if (j.error.empty() && commit([&](Book& book) { id = book.postTransaction(t); }, &j.error)) {
            notify("Posted journal entry #" + std::to_string(id) + " for " + debits.formatted());
            journal_ = JournalState{};
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Clear")) journal_ = JournalState{};
}

}  // namespace obgui

// Financial reports: pick a report and a period, view it, export to CSV or copy as text.

#include "app.hpp"

#include "imgui.h"
#include "openbooks/util.hpp"
#include "platform.hpp"
#include "theme.hpp"
#include "widgets.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace obgui {

using namespace ob;

namespace {

struct ReportInfo {
    ReportKind kind;
    const char* name;
    const char* description;
    bool usesRange;  // false = "as of" a single date
};

const ReportInfo kReports[] = {
    {ReportKind::ProfitLoss, "Profit and Loss", "Income and expenses over a period.", true},
    {ReportKind::BalanceSheet, "Balance Sheet", "What the business owns and owes on a date.", false},
    {ReportKind::TrialBalance, "Trial Balance", "Debit and credit balance of every account.", false},
    {ReportKind::ArAging, "A/R Aging", "Unpaid invoices by how overdue they are.", false},
    {ReportKind::ApAging, "A/P Aging", "Unpaid bills by how overdue they are.", false},
    {ReportKind::Journal, "Journal", "Every transaction with its debits and credits.", true},
    {ReportKind::AccountRegister, "Account Detail", "All activity in one account with a running balance.", true},
};

const char* kPresetNames[] = {"This month", "Last month", "This quarter", "This fiscal year", "Last fiscal year",
                              "All dates", "Custom"};

Date firstOfMonth(int year, int month) {
    while (month < 1) {
        month += 12;
        --year;
    }
    while (month > 12) {
        month -= 12;
        ++year;
    }
    return Date::fromYMD(year, static_cast<unsigned>(month), 1);
}

void applyPreset(const Book& b, ReportState& st) {
    const Date today = Date::today();
    const int y = today.year();
    const int m = static_cast<int>(today.month());
    const Date fy = b.fiscalYearStart(today);
    const int fyMonth = static_cast<int>(fy.month());
    switch (st.preset) {
        case DatePreset::ThisMonth:
            st.from = firstOfMonth(y, m);
            st.to = firstOfMonth(y, m + 1).addDays(-1);
            break;
        case DatePreset::LastMonth:
            st.from = firstOfMonth(y, m - 1);
            st.to = firstOfMonth(y, m).addDays(-1);
            break;
        case DatePreset::ThisQuarter: {
            const int q = (m - 1) / 3 * 3 + 1;
            st.from = firstOfMonth(y, q);
            st.to = firstOfMonth(y, q + 3).addDays(-1);
            break;
        }
        case DatePreset::ThisFiscalYear:
            st.from = fy;
            st.to = firstOfMonth(fy.year(), fyMonth + 12).addDays(-1);
            break;
        case DatePreset::LastFiscalYear:
            st.from = firstOfMonth(fy.year() - 1, fyMonth);
            st.to = fy.addDays(-1);
            break;
        case DatePreset::AllDates: {
            Date earliest = today;
            for (const auto& t : b.transactions()) earliest = std::min(earliest, t.date);
            st.from = earliest;
            st.to = today;
            break;
        }
        case DatePreset::Custom:
            break;
    }
}

}  // namespace

void App::drawReports() {
    const Book& b = *book_;
    ReportState& st = report_;
    if (!st.initialized) {
        st.initialized = true;
        applyPreset(b, st);
    }

    // ---- report list
    ImGui::BeginChild("##reportList", ImVec2(ImGui::GetFontSize() * 13.0f, 0), ImGuiChildFlags_Borders);
    ui::SubHeading("Reports");
    ImGui::Dummy(ImVec2(0, 2));
    for (const auto& r : kReports) {
        if (ImGui::Selectable(r.name, st.kind == r.kind)) st.kind = r.kind;
        ImGui::SetItemTooltip("%s", r.description);
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##report", ImVec2(0, 0));
    const ReportInfo* info = &kReports[0];
    for (const auto& r : kReports) {
        if (r.kind == st.kind) info = &r;
    }

    // ---- parameters
    if (st.kind == ReportKind::AccountRegister) {
        if (st.accountId == 0 && !b.accounts().empty()) st.accountId = b.accounts().front().id;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Account");
        ImGui::SameLine();
        ui::AccountCombo("##account", b, st.accountId, {}, nullptr, ImGui::GetFontSize() * 16.0f);
        ImGui::SameLine(0, 16);
    }
    if (info->usesRange) {
        int preset = static_cast<int>(st.preset);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
        if (ImGui::Combo("##preset", &preset, kPresetNames, 7)) {
            st.preset = static_cast<DatePreset>(preset);
            applyPreset(b, st);
        }
        ImGui::SameLine();
        if (ui::DateField("##from", st.from)) st.preset = DatePreset::Custom;
        ImGui::SameLine();
        ImGui::TextUnformatted("to");
        ImGui::SameLine();
        if (ui::DateField("##to", st.to)) st.preset = DatePreset::Custom;
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("As of");
        ImGui::SameLine();
        ui::DateField("##asof", st.to);
        ImGui::SameLine();
        if (ImGui::SmallButton("Today")) st.to = Date::today();
        ImGui::SameLine();
        if (ImGui::SmallButton("End of last month")) {
            const Date t = Date::today();
            st.to = firstOfMonth(t.year(), static_cast<int>(t.month())).addDays(-1);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("End of last fiscal year")) st.to = b.fiscalYearStart(Date::today()).addDays(-1);
    }

    // ---- build (cached until the books or the parameters change)
    const std::string key = std::to_string(static_cast<int>(st.kind)) + "|" + st.from.str() + "|" + st.to.str() + "|" +
                            std::to_string(st.accountId);
    if (st.builtVersion != version_ || st.builtKey != key) {
        try {
            switch (st.kind) {
                case ReportKind::ProfitLoss: st.table = profitAndLoss(b, st.from, st.to); break;
                case ReportKind::BalanceSheet: st.table = balanceSheet(b, st.to); break;
                case ReportKind::TrialBalance: st.table = trialBalance(b, st.to); break;
                case ReportKind::ArAging: st.table = agingReport(b, DocKind::Invoice, st.to); break;
                case ReportKind::ApAging: st.table = agingReport(b, DocKind::Bill, st.to); break;
                case ReportKind::Journal: st.table = journalReport(b, Period{st.from, st.to}); break;
                case ReportKind::AccountRegister:
                    st.table = st.accountId ? accountRegister(b, st.accountId, Period{st.from, st.to}) : Table{};
                    break;
            }
        } catch (const std::exception& e) {
            st.table = Table{};
            st.table.title = e.what();
        }
        st.builtVersion = version_;
        st.builtKey = key;
    }

    // ---- actions
    ImGui::SameLine(0, 24);
    if (ImGui::Button("Copy as text")) {
        ImGui::SetClipboardText(renderText(st.table).c_str());
        notify("Report copied to clipboard");
    }
    ImGui::SameLine();
    if (ImGui::Button("Export CSV...")) {
        const std::string suggested = std::string(info->name) + " " + st.to.str() + ".csv";
        std::optional<std::string> path;
        if (nativeFileDialogsAvailable()) {
            path = saveFileDialog("Export report", {"CSV files (*.csv)", "*.csv"}, "csv", suggested);
        } else {
            path = (std::filesystem::u8path(path_).parent_path() / std::filesystem::u8path(suggested)).u8string();
        }
        if (path) {
            std::ofstream out(std::filesystem::u8path(*path), std::ios::binary);
            out << renderCsv(st.table);
            if (out) notify("Exported " + *path);
            else notify("Could not write " + *path, true);
        }
    }

    // ---- report
    ImGui::Dummy(ImVec2(0, 8));
    auto centered = [](const std::string& text) {
        const float w = ImGui::CalcTextSize(text.c_str()).x;
        const float avail = ImGui::GetContentRegionAvail().x;
        if (avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - w) * 0.5f);
        ImGui::TextUnformatted(text.c_str());
    };
    ImGui::PushFont(g_fonts.bold, kBaseFontSize * 1.3f);
    centered(st.table.title);
    ImGui::PopFont();
    for (const auto& s : st.table.subtitles) {
        if (startsWith(s, "WARNING")) {
            ImGui::PushStyleColor(ImGuiCol_Text, colorNegative());
            centered(s);
            ImGui::PopStyleColor();
        } else {
            centered(s);
        }
    }
    ImGui::Dummy(ImVec2(0, 6));
    if (!st.table.headers.empty()) ui::ReportTable("##table", st.table, 0.0f);
    ImGui::EndChild();
}

}  // namespace obgui

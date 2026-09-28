#include "widgets.hpp"

#include "imgui_stdlib.h"
#include "openbooks/util.hpp"
#include "theme.hpp"

#include <cfloat>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace obgui::ui {

// ------------------------------------------------------------------- input

bool InputString(const char* label, std::string& value, ImGuiInputTextFlags flags) {
    return ImGui::InputText(label, &value, flags);
}

bool InputStringHint(const char* label, const char* hint, std::string& value, ImGuiInputTextFlags flags) {
    return ImGui::InputTextWithHint(label, hint, &value, flags);
}

bool InputMultiline(const char* label, std::string& value, ImVec2 size) {
    return ImGui::InputTextMultiline(label, &value, size);
}

std::optional<ob::Money> parseMoney(const std::string& text) {
    if (ob::trim(text).empty()) return std::nullopt;
    return ob::Money::parse(text);
}

std::optional<ob::Decimal> parseDecimal(const std::string& text) {
    std::string t = ob::trim(text);
    if (!t.empty() && t.back() == '%') t.pop_back();
    if (t.empty()) return std::nullopt;
    return ob::Decimal::parse(t);
}

namespace {

bool validatedField(const char* label, std::string& text, float width, bool valid) {
    if (width != 0.0f) ImGui::SetNextItemWidth(width);
    if (!valid) ImGui::PushStyleColor(ImGuiCol_Text, colorNegative());
    const bool changed = ImGui::InputText(label, &text);
    if (!valid) {
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Not a valid number");
    }
    return changed;
}

int weekday(ob::Date d) {  // 0 = Sunday; 1970-01-01 was a Thursday
    return ((d.serial() + 4) % 7 + 7) % 7;
}

int daysInMonth(int year, int month) {
    const ob::Date first = ob::Date::fromYMD(year, static_cast<unsigned>(month), 1);
    const ob::Date next = month == 12 ? ob::Date::fromYMD(year + 1, 1, 1)
                                      : ob::Date::fromYMD(year, static_cast<unsigned>(month + 1), 1);
    return next - first;
}

const char* kMonthNames[] = {"January", "February", "March",     "April",   "May",      "June",
                             "July",    "August",   "September", "October", "November", "December"};

// Month grid; returns true when a day was picked.
bool calendar(ob::Date& date, int& viewYear, int& viewMonth) {
    bool picked = false;
    if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) {
        if (--viewMonth < 1) {
            viewMonth = 12;
            --viewYear;
        }
    }
    ImGui::SameLine();
    if (ImGui::ArrowButton("##next", ImGuiDir_Right)) {
        if (++viewMonth > 12) {
            viewMonth = 1;
            ++viewYear;
        }
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s %d", kMonthNames[viewMonth - 1], viewYear);

    const float cell = ImGui::GetFrameHeight() * 1.25f;
    const ob::Date first = ob::Date::fromYMD(viewYear, static_cast<unsigned>(viewMonth), 1);
    const int offset = weekday(first);
    const int days = daysInMonth(viewYear, viewMonth);
    const ob::Date today = ob::Date::today();
    if (!ImGui::BeginTable("##grid", 7, ImGuiTableFlags_SizingFixedSame | ImGuiTableFlags_NoPadOuterX)) return false;
    static const char* kDays[] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
    for (const char* name : kDays) ImGui::TableSetupColumn(name, ImGuiTableColumnFlags_WidthFixed, cell);
    ImGui::TableNextRow();
    for (const char* name : kDays) {
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", name);
    }
    for (int slot = 0; slot < offset + days; ++slot) {
        ImGui::TableNextColumn();
        if (slot < offset) continue;
        const int day = slot - offset + 1;
        const ob::Date d = first.addDays(day - 1);
        const bool selected = d == date;
        int colors = 0;
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, colorAccent());
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
            colors = 2;
        } else if (d == today) {
            ImGui::PushStyleColor(ImGuiCol_Text, colorAccent());
            colors = 1;
        }
        char label[12];  // fits any int, so no truncation warnings
        std::snprintf(label, sizeof label, "%d", day);
        ImGui::PushID(day);
        if (ImGui::Button(label, ImVec2(cell, cell))) {
            date = d;
            picked = true;
        }
        ImGui::PopID();
        ImGui::PopStyleColor(colors);
    }
    ImGui::EndTable();
    ImGui::Separator();
    if (ImGui::SmallButton("Today")) {
        date = today;
        picked = true;
    }
    return picked;
}

std::string visibleLabel(const char* label) {
    const char* hash = std::strstr(label, "##");
    return hash ? std::string(label, static_cast<std::size_t>(hash - label)) : std::string(label);
}

}  // namespace

bool MoneyField(const char* label, std::string& text, float width) {
    const bool valid = ob::trim(text).empty() || ob::Money::parse(text).has_value();
    return validatedField(label, text, width, valid);
}

bool DecimalField(const char* label, std::string& text, float width) {
    const bool valid = ob::trim(text).empty() || parseDecimal(text).has_value();
    return validatedField(label, text, width, valid);
}

bool DateField(const char* label, ob::Date& date, float width) {
    struct State {
        std::string text;
        bool editing = false;
        int viewYear = 0;
        int viewMonth = 1;
    };
    static std::unordered_map<ImGuiID, State> states;

    ImGui::PushID(label);
    State& s = states[ImGui::GetID("state")];
    if (!s.editing) s.text = date.str();

    bool changed = false;
    const bool valid = ob::Date::parse(s.text).has_value();
    ImGui::SetNextItemWidth(width > 0.0f ? width : ImGui::GetFontSize() * 6.5f);
    if (!valid) ImGui::PushStyleColor(ImGuiCol_Text, colorNegative());
    if (ImGui::InputTextWithHint("##text", "YYYY-MM-DD", &s.text)) {
        if (auto d = ob::Date::parse(s.text); d && *d != date) {
            date = *d;
            changed = true;
        }
    }
    if (!valid) ImGui::PopStyleColor();
    ImGui::SetItemTooltip("YYYY-MM-DD, MM/DD/YYYY or 'today'");
    s.editing = ImGui::IsItemActive();

    ImGui::SameLine(0, 2.0f);
    if (ImGui::ArrowButton("##open", ImGuiDir_Down)) {
        s.viewYear = date.year();
        s.viewMonth = static_cast<int>(date.month());
        ImGui::OpenPopup("calendar");
    }
    if (ImGui::BeginPopup("calendar")) {
        if (calendar(date, s.viewYear, s.viewMonth)) {
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    const std::string visible = visibleLabel(label);
    if (!visible.empty()) {
        ImGui::SameLine();
        ImGui::TextUnformatted(visible.c_str());
    }
    ImGui::PopID();
    return changed;
}

// ----------------------------------------------------------------- pickers

bool SearchCombo(const char* label, const std::string& preview, const std::vector<Option>& options, int& value,
                 float width) {
    bool changed = false;
    if (width != 0.0f) ImGui::SetNextItemWidth(width);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, ImGui::GetFontSize() * 22));
    if (ImGui::BeginCombo(label, preview.c_str(), ImGuiComboFlags_HeightLarge)) {
        static std::string filter;
        if (ImGui::IsWindowAppearing()) {
            filter.clear();
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool enter = ImGui::InputTextWithHint("##search", "Type to search...", &filter,
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
        const std::string needle = ob::toLower(ob::trim(filter));
        bool firstPicked = false;
        for (const auto& o : options) {
            if (!needle.empty() && (o.header || ob::toLower(o.label).find(needle) == std::string::npos)) continue;
            if (o.header) {
                ImGui::Spacing();
                ImGui::TextDisabled("%s", o.label.c_str());
                continue;
            }
            if (enter && !firstPicked) {
                value = o.id;
                changed = true;
                firstPicked = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PushID(o.id);
            const bool selected = o.id == value;
            if (ImGui::Selectable(o.label.c_str(), selected)) {
                value = o.id;
                changed = true;
            }
            if (selected && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool isMoneyAccount(const ob::Book& b, const ob::Account& a) {
    return (a.type == ob::AccountType::Asset || a.type == ob::AccountType::Liability) && !b.isSystemAccount(a.id);
}

bool isCategoryAccount(const ob::Book& b, const ob::Account& a) { return !b.isSubledgerAccount(a.id); }

std::string accountName(const ob::Book& b, int accountId) {
    if (accountId == 0) return {};
    for (const auto& a : b.accounts()) {
        if (a.id == accountId) return a.name;
    }
    return "?";
}

bool AccountCombo(const char* label, const ob::Book& b, int& accountId, const AccountFilter& filter,
                  const char* noneLabel, float width) {
    std::vector<Option> options;
    if (noneLabel) options.push_back({0, noneLabel, false});
    for (ob::AccountType type : {ob::AccountType::Asset, ob::AccountType::Liability, ob::AccountType::Equity,
                                 ob::AccountType::Income, ob::AccountType::Expense}) {
        bool headerAdded = false;
        for (const auto& a : b.accounts()) {
            if (a.type != type || (!a.active && a.id != accountId) || (filter && !filter(a))) continue;
            if (!headerAdded) {
                static const char* kGroupNames[] = {"Assets", "Liabilities", "Equity", "Income", "Expenses"};
                options.push_back({-1, kGroupNames[static_cast<int>(type)], true});
                headerAdded = true;
            }
            options.push_back({a.id, ob::accountLabel(a), false});
        }
    }
    std::string preview = noneLabel ? noneLabel : "Select account...";
    for (const auto& a : b.accounts()) {
        if (a.id == accountId) preview = ob::accountLabel(a);
    }
    return SearchCombo(label, preview, options, accountId, width);
}

bool ContactCombo(const char* label, const ob::Book& b, ob::ContactKind kind, int& contactId, float width) {
    std::vector<Option> options;
    std::string preview = kind == ob::ContactKind::Customer ? "Select customer..." : "Select vendor...";
    for (const auto& c : b.contacts()) {
        if (c.kind != kind) continue;
        if (c.id == contactId) preview = c.name;
        if (c.active || c.id == contactId) options.push_back({c.id, c.name, false});
    }
    return SearchCombo(label, preview, options, contactId, width);
}

bool ItemCombo(const char* label, const ob::Book& b, int& itemId, float width) {
    std::vector<Option> options{{0, "(no item)", false}};
    std::string preview = "(no item)";
    for (const auto& i : b.items()) {
        if (i.id == itemId) preview = i.name;
        if (i.active || i.id == itemId) options.push_back({i.id, i.name, false});
    }
    return SearchCombo(label, preview, options, itemId, width);
}

// -------------------------------------------------------------------- text

void Heading(const char* text) {
    ImGui::PushFont(g_fonts.bold, kBaseFontSize * 1.55f);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

void SubHeading(const char* text) {
    ImGui::PushFont(g_fonts.bold, kBaseFontSize * 1.1f);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

void Muted(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, colorMuted());
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void TextRight(const char* text) {
    const float w = ImGui::CalcTextSize(text).x;
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
    ImGui::TextUnformatted(text);
}

void MoneyText(ob::Money amount, bool rightAlign, bool redIfNegative) {
    const std::string s = amount.formatted();
    const bool red = redIfNegative && amount < ob::Money();
    if (red) ImGui::PushStyleColor(ImGuiCol_Text, colorNegative());
    if (rightAlign) TextRight(s.c_str());
    else ImGui::TextUnformatted(s.c_str());
    if (red) ImGui::PopStyleColor();
}

void Badge(const char* text, ImVec4 color) {
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 pad(ImGui::GetFontSize() * 0.45f, 1.0f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec4 bg = color;
    bg.w = 0.16f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, ImVec2(pos.x + size.x + pad.x * 2, pos.y + size.y + pad.y * 2),
                        ImGui::GetColorU32(bg), size.y * 0.5f);
    ImGui::SetCursorScreenPos(ImVec2(pos.x + pad.x, pos.y + pad.y));
    ImGui::TextColored(color, "%s", text);
}

ImVec4 statusColor(const std::string& s) {
    if (s == "Paid" || s == "Applied" || s == "Converted" || s == "Accepted") return colorPositive();
    if (s == "Overdue" || s == "Declined" || s == "Expired") return colorNegative();
    if (s == "Partial" || s == "Partly applied" || s == "Due") return colorWarning();
    if (s == "Void" || s == "Paused" || s == "Finished") return colorMuted();
    return colorAccent();  // Open, Pending, Unapplied, Active
}

void ErrorText(const std::string& error) {
    if (error.empty()) return;
    ImGui::PushStyleColor(ImGuiCol_Text, colorNegative());
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    ImGui::TextUnformatted(error.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// ----------------------------------------------------------------- buttons

namespace {
bool coloredButton(const char* label, ImVec2 size, ImVec4 base) {
    ImVec4 hover = base;
    hover.x *= 1.1f;
    hover.y *= 1.1f;
    hover.z *= 1.1f;
    ImVec4 active = base;
    active.x *= 0.9f;
    active.y *= 0.9f;
    active.z *= 0.9f;
    ImGui::PushStyleColor(ImGuiCol_Button, base);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}
}  // namespace

bool PrimaryButton(const char* label, ImVec2 size) { return coloredButton(label, size, colorAccent()); }
bool DangerButton(const char* label, ImVec2 size) { return coloredButton(label, size, colorNegative()); }

// ------------------------------------------------------------------ layout

void StatCard(const char* id, const char* title, const std::string& value, ImVec4 valueColor, const std::string& note,
              float width) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, colorCardBg());
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    const float height = ImGui::GetFontSize() * 5.4f;
    ImGui::BeginChild(id, ImVec2(width, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    Muted(title);
    ImGui::PushFont(g_fonts.bold, kBaseFontSize * 1.7f);
    ImGui::TextColored(valueColor, "%s", value.c_str());
    ImGui::PopFont();
    Muted(note.c_str());
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void ReportTable(const char* id, const ob::Table& table, float height) {
    const int cols = static_cast<int>(table.headers.size());
    if (cols == 0) return;
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable(id, cols, flags, ImVec2(0.0f, height))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    for (int c = 0; c < cols; ++c) {
        const bool numeric = static_cast<std::size_t>(c) < table.numeric.size() && table.numeric[static_cast<std::size_t>(c)];
        ImGui::TableSetupColumn(table.headers[static_cast<std::size_t>(c)].c_str(),
                                numeric ? ImGuiTableColumnFlags_WidthFixed : ImGuiTableColumnFlags_WidthStretch,
                                numeric ? ImGui::GetFontSize() * 7.0f : (c == 0 ? 3.0f : 1.0f));
    }
    ImGui::TableHeadersRow();

    const ImU32 sectionBg = ImGui::GetColorU32(ImGuiCol_TableHeaderBg, 0.6f);
    const ImU32 totalBg = ImGui::GetColorU32(ImGuiCol_Header, 0.5f);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(table.rows.size()));
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            const ob::Row& row = table.rows[static_cast<std::size_t>(r)];
            ImGui::TableNextRow();
            if (row.style == ob::RowStyle::Spacer) {
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted("");
                continue;
            }
            const bool bold = row.style == ob::RowStyle::Section || row.style == ob::RowStyle::Total;
            if (row.style == ob::RowStyle::Section) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, sectionBg);
            if (row.style == ob::RowStyle::Total) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, totalBg);
            if (bold) ImGui::PushFont(g_fonts.bold, 0.0f);
            for (int c = 0; c < cols; ++c) {
                ImGui::TableSetColumnIndex(c);
                const auto uc = static_cast<std::size_t>(c);
                if (uc >= row.cells.size()) continue;
                const std::string& cell = row.cells[uc];
                const bool numeric = uc < table.numeric.size() && table.numeric[uc];
                if (numeric) {
                    const bool negative = !cell.empty() && cell[0] == '-';
                    if (negative) ImGui::PushStyleColor(ImGuiCol_Text, colorNegative());
                    TextRight(cell.c_str());
                    if (negative) ImGui::PopStyleColor();
                } else {
                    if (c == 0 && row.indent > 0) ImGui::Indent(ImGui::GetFontSize() * 1.2f * static_cast<float>(row.indent));
                    ImGui::TextUnformatted(cell.c_str());
                    if (c == 0 && row.indent > 0) ImGui::Unindent(ImGui::GetFontSize() * 1.2f * static_cast<float>(row.indent));
                }
            }
            if (bold) ImGui::PopFont();
        }
    }
    ImGui::EndTable();
}

void FormLabel(const char* label, float column) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(column);
}

}  // namespace obgui::ui

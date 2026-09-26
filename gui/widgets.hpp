#pragma once

// Reusable ImGui widgets for OpenBooks: validated inputs, searchable pickers, and
// renderers for money and report tables.

#include "imgui.h"
#include "openbooks/book.hpp"
#include "openbooks/reports.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace obgui::ui {

// ---- text input
bool InputString(const char* label, std::string& value, ImGuiInputTextFlags flags = 0);
bool InputStringHint(const char* label, const char* hint, std::string& value, ImGuiInputTextFlags flags = 0);
bool InputMultiline(const char* label, std::string& value, ImVec2 size);

// ---- validated fields (text turns red while invalid)
std::optional<ob::Money> parseMoney(const std::string& text);    // empty -> nullopt
std::optional<ob::Decimal> parseDecimal(const std::string& text);  // empty -> nullopt
bool MoneyField(const char* label, std::string& text, float width = 0.0f);
bool DecimalField(const char* label, std::string& text, float width = 0.0f);
// Text entry plus a calendar popup. Returns true when the date changed.
bool DateField(const char* label, ob::Date& date, float width = 0.0f);

// ---- pickers with type-to-search
struct Option {
    int id = 0;
    std::string label;
    bool header = false;  // non-selectable group heading
};
bool SearchCombo(const char* label, const std::string& preview, const std::vector<Option>& options, int& value,
                 float width = 0.0f);

using AccountFilter = std::function<bool(const ob::Account&)>;
bool AccountCombo(const char* label, const ob::Book& b, int& accountId, const AccountFilter& filter = {},
                  const char* noneLabel = nullptr, float width = 0.0f);
bool ContactCombo(const char* label, const ob::Book& b, ob::ContactKind kind, int& contactId, float width = 0.0f);
bool ItemCombo(const char* label, const ob::Book& b, int& itemId, float width = 0.0f);

// Asset or liability accounts money moves through: bank, cash, credit cards, loans.
bool isMoneyAccount(const ob::Book& b, const ob::Account& a);
bool isCategoryAccount(const ob::Book& b, const ob::Account& a);  // anything but A/R and A/P
std::string accountName(const ob::Book& b, int accountId);

// ---- text
void Heading(const char* text);
void SubHeading(const char* text);
void Muted(const char* text);
void TextRight(const char* text);
void MoneyText(ob::Money amount, bool rightAlign = true, bool redIfNegative = true);
void Badge(const char* text, ImVec4 color);
// Color for a document status label from ob::documentStatus ("Paid", "Overdue", ...).
ImVec4 statusColor(const std::string& status);
void ErrorText(const std::string& error);

// ---- buttons
bool PrimaryButton(const char* label, ImVec2 size = ImVec2(0, 0));
bool DangerButton(const char* label, ImVec2 size = ImVec2(0, 0));

// ---- layout pieces
void StatCard(const char* id, const char* title, const std::string& value, ImVec4 valueColor, const std::string& note,
              float width);
// Renders a core report Table. Height 0 = fill the remaining space.
void ReportTable(const char* id, const ob::Table& table, float height = 0.0f);
// Label column helper for forms: draws `label` then positions the next widget at `column`.
void FormLabel(const char* label, float column);

}  // namespace obgui::ui

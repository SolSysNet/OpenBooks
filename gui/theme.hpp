#pragma once

#include "imgui.h"

namespace obgui {

struct Fonts {
    ImFont* regular = nullptr;
    ImFont* bold = nullptr;  // falls back to regular
};

extern Fonts g_fonts;

// Loads UI fonts (system fonts where available, ImGui's built-in font otherwise).
void loadFonts();

// Base size (unscaled pixels) used for body text.
constexpr float kBaseFontSize = 16.0f;

void applyTheme(bool dark);
bool themeIsDark();

// Semantic colors that follow the current theme.
ImVec4 colorAccent();    // primary action
ImVec4 colorPositive();  // paid, balanced
ImVec4 colorNegative();  // overdue, negative amounts, errors
ImVec4 colorWarning();   // partial, attention
ImVec4 colorMuted();     // secondary text
ImVec4 colorCardBg();

}  // namespace obgui

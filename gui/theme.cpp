#include "theme.hpp"

#include "platform.hpp"

#include <filesystem>

namespace obgui {

Fonts g_fonts;

namespace {

bool g_dark = false;

ImFont* loadFirst(const char* const* candidates) {
    ImGuiIO& io = ImGui::GetIO();
    for (auto p = candidates; *p; ++p) {
        std::error_code ec;
        if (std::filesystem::exists(*p, ec)) {
            if (ImFont* f = io.Fonts->AddFontFromFileTTF(*p)) return f;
        }
    }
    return nullptr;
}

ImVec4 rgb(int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); }

}  // namespace

void loadFonts() {
    ImGui::GetStyle().FontSizeBase = kBaseFontSize;
    g_fonts.regular = loadFirst(preferredFonts());
    if (!g_fonts.regular) g_fonts.regular = ImGui::GetIO().Fonts->AddFontDefault();
    g_fonts.bold = loadFirst(preferredBoldFonts());
    if (!g_fonts.bold) g_fonts.bold = g_fonts.regular;
}

bool themeIsDark() { return g_dark; }

void applyTheme(bool dark) {
    g_dark = dark;
    ImGuiStyle& style = ImGui::GetStyle();
    if (dark) ImGui::StyleColorsDark(&style);
    else ImGui::StyleColorsLight(&style);

    style.WindowRounding = 6.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;

    ImVec4* c = style.Colors;
    const ImVec4 accent = colorAccent();
    if (dark) {
        c[ImGuiCol_WindowBg] = rgb(30, 32, 36);
        c[ImGuiCol_ChildBg] = rgb(30, 32, 36);
        c[ImGuiCol_PopupBg] = rgb(38, 41, 46);
        c[ImGuiCol_MenuBarBg] = rgb(24, 26, 29);
        c[ImGuiCol_FrameBg] = rgb(45, 48, 54);
        c[ImGuiCol_FrameBgHovered] = rgb(55, 59, 66);
        c[ImGuiCol_FrameBgActive] = rgb(62, 66, 74);
        c[ImGuiCol_TableHeaderBg] = rgb(40, 43, 48);
        c[ImGuiCol_TableRowBgAlt] = rgb(255, 255, 255, 0.03f);
        c[ImGuiCol_Header] = rgb(58, 110, 70, 0.55f);
        c[ImGuiCol_HeaderHovered] = rgb(58, 110, 70, 0.75f);
        c[ImGuiCol_HeaderActive] = rgb(58, 110, 70, 0.95f);
        c[ImGuiCol_Button] = rgb(52, 56, 63);
        c[ImGuiCol_ButtonHovered] = rgb(64, 69, 78);
        c[ImGuiCol_ButtonActive] = rgb(74, 80, 90);
    } else {
        c[ImGuiCol_WindowBg] = rgb(247, 248, 250);
        c[ImGuiCol_ChildBg] = rgb(247, 248, 250);
        c[ImGuiCol_PopupBg] = rgb(255, 255, 255);
        c[ImGuiCol_MenuBarBg] = rgb(236, 238, 241);
        c[ImGuiCol_FrameBg] = rgb(255, 255, 255);
        c[ImGuiCol_FrameBgHovered] = rgb(240, 246, 241);
        c[ImGuiCol_FrameBgActive] = rgb(232, 242, 234);
        c[ImGuiCol_Border] = rgb(210, 214, 220);
        c[ImGuiCol_TableHeaderBg] = rgb(236, 238, 241);
        c[ImGuiCol_TableRowBgAlt] = rgb(0, 0, 0, 0.025f);
        c[ImGuiCol_Header] = rgb(44, 160, 28, 0.20f);
        c[ImGuiCol_HeaderHovered] = rgb(44, 160, 28, 0.28f);
        c[ImGuiCol_HeaderActive] = rgb(44, 160, 28, 0.38f);
        c[ImGuiCol_Button] = rgb(228, 231, 235);
        c[ImGuiCol_ButtonHovered] = rgb(214, 219, 225);
        c[ImGuiCol_ButtonActive] = rgb(200, 206, 214);
        style.FrameBorderSize = 1.0f;
    }
    c[ImGuiCol_TitleBg] = dark ? rgb(36, 39, 44) : rgb(236, 238, 241);
    c[ImGuiCol_TitleBgActive] = dark ? rgb(44, 48, 54) : rgb(226, 230, 234);
    c[ImGuiCol_TitleBgCollapsed] = c[ImGuiCol_TitleBg];
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_TabSelected] = dark ? rgb(58, 110, 70) : rgb(255, 255, 255);
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_NavCursor] = accent;
}

ImVec4 colorAccent() { return g_dark ? rgb(76, 175, 80) : rgb(44, 160, 28); }
ImVec4 colorPositive() { return g_dark ? rgb(110, 200, 120) : rgb(30, 130, 40); }
ImVec4 colorNegative() { return g_dark ? rgb(240, 110, 100) : rgb(200, 45, 40); }
ImVec4 colorWarning() { return g_dark ? rgb(235, 180, 80) : rgb(190, 120, 0); }
ImVec4 colorMuted() { return g_dark ? rgb(150, 155, 162) : rgb(110, 116, 125); }
ImVec4 colorCardBg() { return g_dark ? rgb(38, 41, 46) : rgb(255, 255, 255); }

}  // namespace obgui

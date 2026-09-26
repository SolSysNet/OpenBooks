// Windows entry point: Win32 window + Direct3D 11 renderer for Dear ImGui.
// Adapted from Dear ImGui's example_win32_directx11 (MIT).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>

#include "app.hpp"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "theme.hpp"

#include <algorithm>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_renderTarget = nullptr;
bool g_occluded = false;
UINT g_resizeWidth = 0;
UINT g_resizeHeight = 0;

void createRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (backBuffer) {
        g_device->CreateRenderTargetView(backBuffer, nullptr, &g_renderTarget);
        backBuffer->Release();
    }
}

void cleanupRenderTarget() {
    if (g_renderTarget) {
        g_renderTarget->Release();
        g_renderTarget = nullptr;
    }
}

bool createDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &level, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)  // no GPU: fall back to the WARP software rasterizer
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                           &sd, &g_swapChain, &g_device, &level, &g_context);
    if (hr != S_OK) return false;
    createRenderTarget();
    return true;
}

void cleanupDevice() {
    cleanupRenderTarget();
    if (g_swapChain) g_swapChain->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
    g_swapChain = nullptr;
    g_context = nullptr;
    g_device = nullptr;
}

LRESULT WINAPI wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;
    switch (msg) {
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) return 0;
            g_resizeWidth = LOWORD(lParam);
            g_resizeHeight = HIWORD(lParam);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = 900;
            info->ptMinTrackSize.y = 600;
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;  // no ALT application menu
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string firstArgumentUtf8() {
    int count = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &count);
    std::string result;
    if (argv && count > 1) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, nullptr, 0, nullptr, nullptr);
        if (n > 1) {
            result.resize(static_cast<std::size_t>(n - 1));
            WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, result.data(), n, nullptr, nullptr);
        }
    }
    LocalFree(argv);
    return result;
}

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    ImGui_ImplWin32_EnableDpiAwareness();
    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"OpenBooksWindow";
    RegisterClassExW(&wc);
    // Default to 1360x860 (scaled), but never larger than the desktop work area.
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int workWidth = work.right - work.left;
    const int workHeight = work.bottom - work.top;
    const int width = std::min(static_cast<int>(1360 * scale), workWidth - 40);
    const int height = std::min(static_cast<int>(860 * scale), workHeight - 40);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"OpenBooks", WS_OVERLAPPEDWINDOW, work.left + (workWidth - width) / 2,
                              work.top + (workHeight - height) / 2, width, height, nullptr, nullptr, instance, nullptr);
    if (!createDevice(hwnd)) {
        cleanupDevice();
        UnregisterClassW(wc.lpszClassName, instance);
        MessageBoxW(nullptr, L"Could not initialize Direct3D 11.", L"OpenBooks", MB_ICONERROR);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsLight();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    obgui::loadFonts();

    try {
        obgui::App app(firstArgumentUtf8());

        // Title bar follows the app theme on Windows 11.
        const BOOL dark = obgui::themeIsDark() ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        ShowWindow(hwnd, SW_SHOWDEFAULT);
        UpdateWindow(hwnd);

        std::string title;
        bool lastDark = obgui::themeIsDark();
        int busyFrames = 3;  // keep rendering briefly after input so animations settle
        bool done = false;
        while (!done) {
            // Sleep until there is input, unless something on screen is animating.
            if (busyFrames <= 0 && !app.wantsFrequentRedraw())
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 500, QS_ALLINPUT);
            MSG msg;
            bool hadInput = false;
            while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                hadInput = true;
                if (msg.message == WM_QUIT) done = true;
            }
            if (done) break;
            busyFrames = hadInput ? 3 : busyFrames - 1;

            if (g_occluded && g_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
                Sleep(10);
                continue;
            }
            g_occluded = false;
            if (g_resizeWidth != 0 && g_resizeHeight != 0) {
                cleanupRenderTarget();
                g_swapChain->ResizeBuffers(0, g_resizeWidth, g_resizeHeight, DXGI_FORMAT_UNKNOWN, 0);
                g_resizeWidth = g_resizeHeight = 0;
                createRenderTarget();
            }

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            app.frame();
            ImGui::Render();

            const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
            const float clear[4] = {bg.x, bg.y, bg.z, 1.0f};
            g_context->OMSetRenderTargets(1, &g_renderTarget, nullptr);
            g_context->ClearRenderTargetView(g_renderTarget, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            g_occluded = g_swapChain->Present(1, 0) == DXGI_STATUS_OCCLUDED;

            if (app.windowTitle() != title) {
                title = app.windowTitle();
                SetWindowTextW(hwnd, widen(title).c_str());
            }
            if (obgui::themeIsDark() != lastDark) {
                lastDark = obgui::themeIsDark();
                const BOOL value = lastDark ? TRUE : FALSE;
                DwmSetWindowAttribute(hwnd, 20, &value, sizeof(value));
            }
            if (app.quitRequested()) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
    } catch (const std::exception& e) {
        // Last resort: report instead of vanishing (per-frame errors are already handled in App::frame).
        const std::wstring message = L"OpenBooks hit an unexpected error and has to close:\n\n" + widen(e.what());
        MessageBoxW(hwnd, message.c_str(), L"OpenBooks", MB_ICONERROR);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanupDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, instance);
    return 0;
}
